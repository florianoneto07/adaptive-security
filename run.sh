#!/usr/bin/env bash
#
# Orquestrador do testbed (requisitos R1 e R6).
#
# Uso:
#   ./run.sh control
#   ./run.sh control telemetry
#   ./run.sh all
#
# Canais: control (C1), telemetry (C2), media (C3), bulk (C4), ou "all".
#
# Cada canal é um par de binários independente: sobe, roda e é derrubado sem
# afetar os outros. Qualquer subconjunto é aceito, e a falha de um canal não
# interrompe os demais — o relatório final diz quais passaram.
#
# Opções:
#   --role both|server|client  papel desta máquina (padrão: both, em loopback)
#   --host ENDERECO            servidor a contatar quando role=client
#   --duration SEGUNDOS        duração por execução (padrão: 60)
#   --repeat N                 repetições por canal (padrão: 10)
#   --bulk-size MiB            tamanho da transferência do C4 (padrão: 32)
#   --out DIR                  raiz dos resultados (padrão: results)
#   --netem PERFIL             aplica o perfil antes e remove depois
#   --capture                  grava pcap por canal (exige CAP_NET_RAW)
#   --port-offset N            desloca todas as portas, para rodar em paralelo
#   --list                     lista os canais e sai
#
# Saída: results/<run_id>/rep<NN>/<canal>_<papel>.csv e .summary, mais
# results/<run_id>/manifest.json com commit, versões, netem e parâmetros.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${REPO_ROOT}"

# ---------------------------------------------------------------------------
# Padrões
# ---------------------------------------------------------------------------

ROLE="both"
HOST="127.0.0.1"
DURATION=60          # escolha do experimento, não da literatura
REPEAT=10            # idem
BULK_MIB=32          # padrão do testbed
OUT_ROOT="results"
NETEM_PROFILE=""
CAPTURE=0
PORT_OFFSET=0

# wolfSSL fora de /usr/local exige LD_LIBRARY_PATH em toda execução. Resolver
# aqui evita que o operador descubra isso como "error while loading shared
# libraries" no meio da campanha.
WOLFSSL_DIR="${WOLFSSL_DIR:-}"
if [[ -z "${WOLFSSL_DIR}" && -f "${HOME}/.local/lib/libwolfssl.so" ]]; then
  WOLFSSL_DIR="${HOME}/.local"
fi
if [[ -n "${WOLFSSL_DIR}" ]]; then
  export LD_LIBRARY_PATH="${WOLFSSL_DIR}/lib:${LD_LIBRARY_PATH:-}"
fi

# canal:slug:prefixo:porta
CHANNEL_SPEC=(
  "control:c1_control:c1:5001"
  "telemetry:c2_telemetry:c2:5002"
  "media:c3_media:c3:5003"
  "bulk:c4_bulk:c4:5004"
)

# ---------------------------------------------------------------------------
# Utilidades
# ---------------------------------------------------------------------------

usage() { sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; }

spec_for() {
  local name="$1" entry
  for entry in "${CHANNEL_SPEC[@]}"; do
    [[ "${entry%%:*}" == "${name}" ]] && { printf '%s' "${entry}"; return 0; }
  done
  return 1
}

log()  { printf '%s\n' "$*"; }
warn() { printf '%s\n' "$*" >&2; }

# Escapa uma string para caber num literal JSON.
json_str() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/\\t/g' \
                         -e ':a' -e 'N' -e '$!ba' -e 's/\n/\\n/g'
}

# ---------------------------------------------------------------------------
# Argumentos
# ---------------------------------------------------------------------------

CHANNELS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --role)        ROLE="${2:-}"; shift 2 ;;
    --host)        HOST="${2:-}"; shift 2 ;;
    --duration)    DURATION="${2:-}"; shift 2 ;;
    --repeat)      REPEAT="${2:-}"; shift 2 ;;
    --bulk-size)   BULK_MIB="${2:-}"; shift 2 ;;
    --out)         OUT_ROOT="${2:-}"; shift 2 ;;
    --netem)       NETEM_PROFILE="${2:-}"; shift 2 ;;
    --port-offset) PORT_OFFSET="${2:-}"; shift 2 ;;
    --capture)     CAPTURE=1; shift ;;
    --list)
      printf '%-12s %-14s %s\n' CANAL SLUG PORTA
      for e in "${CHANNEL_SPEC[@]}"; do
        IFS=: read -r n s _ p <<<"${e}"
        printf '%-12s %-14s %s\n' "${n}" "${s}" "${p}"
      done
      exit 0 ;;
    -h|--help)     usage; exit 0 ;;
    all)           CHANNELS=(control telemetry media bulk); shift ;;
    -*)            warn "Opção desconhecida: $1"; usage >&2; exit 1 ;;
    *)             CHANNELS+=("$1"); shift ;;
  esac
done

if [[ ${#CHANNELS[@]} -eq 0 ]]; then
  warn "Nenhum canal indicado."
  warn ""
  usage >&2
  exit 1
fi

for c in "${CHANNELS[@]}"; do
  spec_for "${c}" >/dev/null || { warn "Canal desconhecido: ${c}"; warn "Conhecidos: control telemetry media bulk (ou all)"; exit 1; }
done

case "${ROLE}" in
  both|server|client) ;;
  *) warn "role inválido: ${ROLE} (both, server ou client)"; exit 1 ;;
esac

if [[ "${ROLE}" == "client" && "${HOST}" == "127.0.0.1" ]]; then
  warn "Aviso: role=client com host 127.0.0.1. Use --host para apontar a VM servidora."
fi

# ---------------------------------------------------------------------------
# Pré-condições
# ---------------------------------------------------------------------------

for c in "${CHANNELS[@]}"; do
  IFS=: read -r _ slug prefix _ <<<"$(spec_for "${c}")"
  for role_bin in server client; do
    [[ "${ROLE}" == "server" && "${role_bin}" == "client" ]] && continue
    [[ "${ROLE}" == "client" && "${role_bin}" == "server" ]] && continue
    if [[ ! -x "build/${prefix}_${role_bin}" ]]; then
      warn "Binário ausente: build/${prefix}_${role_bin}"
      warn "Compile antes:  make"
      exit 1
    fi
  done
  # A chave é por canal: sem ela o canal falha em voz alta, mas é melhor
  # descobrir isso agora do que no meio de dez repetições.
  key_file="keys/${prefix}.key"
  if [[ ! -f "${key_file}" && -z "${!prefix:-}" ]]; then
    if [[ ! -f "${key_file}" ]]; then
      warn "Chave ausente: ${key_file}"
      warn "Gere as chaves: ./scripts/generate_keys.sh"
      exit 1
    fi
  fi
done

# ---------------------------------------------------------------------------
# Identidade da execução
# ---------------------------------------------------------------------------

GIT_COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo desconhecido)"
GIT_DIRTY="false"
if ! git diff --quiet 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then
  GIT_DIRTY="true"
fi

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-${GIT_COMMIT}"
OUT_DIR="${OUT_ROOT}/${RUN_ID}"
mkdir -p "${OUT_DIR}"

log "=============================================================="
log " Testbed de perfis fixos por classe de aplicação UAV"
log "=============================================================="
log " run_id     ${RUN_ID}"
log " papel      ${ROLE}"
log " canais     ${CHANNELS[*]}"
log " duração    ${DURATION}s x ${REPEAT} repetições"
log " saída      ${OUT_DIR}"
[[ -n "${NETEM_PROFILE}" ]] && log " netem      ${NETEM_PROFILE}"
[[ "${GIT_DIRTY}" == "true" ]] && log " ATENÇÃO    árvore git com alterações não commitadas"
log ""

# ---------------------------------------------------------------------------
# netem
# ---------------------------------------------------------------------------

NETEM_APPLIED=0
cleanup_netem() {
  if [[ "${NETEM_APPLIED}" -eq 1 ]]; then
    log "Removendo emulação de enlace..."
    ./netem.sh off >/dev/null 2>&1 || true
    NETEM_APPLIED=0
  fi
}

if [[ -n "${NETEM_PROFILE}" ]]; then
  if ./netem.sh "${NETEM_PROFILE}" >/dev/null 2>&1; then
    NETEM_APPLIED=1
    log "Emulação de enlace aplicada: ${NETEM_PROFILE}"
    log "Lembre-se de aplicar o mesmo perfil na outra VM (netem age só na saída)."
  else
    warn "Não foi possível aplicar o netem '${NETEM_PROFILE}'. Detalhes:"
    ./netem.sh "${NETEM_PROFILE}" 2>&1 | sed 's/^/  /' >&2
    warn "Seguindo SEM emulação — o manifesto vai registrar netem=nenhum."
    NETEM_PROFILE=""
  fi
fi

# ---------------------------------------------------------------------------
# Limpeza
# ---------------------------------------------------------------------------

SERVER_PIDS=()
TCPDUMP_PIDS=()

cleanup() {
  local pid
  for pid in ${SERVER_PIDS[@]+"${SERVER_PIDS[@]}"}; do
    kill -TERM "${pid}" 2>/dev/null || true
  done
  for pid in ${TCPDUMP_PIDS[@]+"${TCPDUMP_PIDS[@]}"}; do
    kill -INT "${pid}" 2>/dev/null || true
  done
  cleanup_netem
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Execução de um canal
# ---------------------------------------------------------------------------

# run_channel <canal> <dir_da_repetição>
# Retorna 0 se o canal completou.
run_channel() {
  local name="$1" rep_dir="$2"
  local slug prefix port server_log client_log server_pid rc=0
  local pcap="" tcpdump_pid=""

  IFS=: read -r _ slug prefix port <<<"$(spec_for "${name}")"
  port=$((port + PORT_OFFSET))

  server_log="${rep_dir}/${slug}_server.log"
  client_log="${rep_dir}/${slug}_client.log"

  # --- captura opcional -----------------------------------------------------
  if [[ "${CAPTURE}" -eq 1 ]]; then
    local proto="udp"
    [[ "${name}" == "bulk" ]] && proto="tcp"
    pcap="${rep_dir}/${slug}.pcap"
    tcpdump -i any -w "${pcap}" -U "${proto} port ${port}" \
      >"${rep_dir}/${slug}_tcpdump.log" 2>&1 &
    tcpdump_pid=$!
    TCPDUMP_PIDS+=("${tcpdump_pid}")
    sleep 0.7
    if ! kill -0 "${tcpdump_pid}" 2>/dev/null; then
      warn "  tcpdump não subiu para ${name}; bytes no fio virão só dos"
      warn "  contadores internos (ausentes no C2). Ver ${rep_dir}/${slug}_tcpdump.log"
      pcap=""
      tcpdump_pid=""
    fi
  fi

  # --- servidor -------------------------------------------------------------
  if [[ "${ROLE}" == "both" || "${ROLE}" == "server" ]]; then
    "./build/${prefix}_server" -p "${port}" -o "${rep_dir}" \
      >"${server_log}" 2>&1 &
    server_pid=$!
    SERVER_PIDS+=("${server_pid}")

    # Espera o anúncio de escuta. Sem isso o cliente dispara antes do bind e
    # falha por "connection refused", que seria lido como falha do perfil.
    local up=0 i
    for i in $(seq 1 100); do
      if grep -q "aguardando" "${server_log}" 2>/dev/null; then up=1; break; fi
      kill -0 "${server_pid}" 2>/dev/null || break
      sleep 0.1
    done
    if [[ "${up}" -eq 0 ]]; then
      warn "  servidor de ${name} não subiu:"
      sed 's/^/    /' "${server_log}" >&2
      kill -TERM "${server_pid}" 2>/dev/null || true
      [[ -n "${tcpdump_pid}" ]] && kill -INT "${tcpdump_pid}" 2>/dev/null
      return 1
    fi
  fi

  # --- cliente --------------------------------------------------------------
  if [[ "${ROLE}" == "both" || "${ROLE}" == "client" ]]; then
    local -a args=(-H "${HOST}" -p "${port}" -o "${rep_dir}")
    if [[ "${name}" == "bulk" ]]; then
      args+=(-s "${BULK_MIB}")
    else
      args+=(-d "${DURATION}")
    fi

    if "./build/${prefix}_client" "${args[@]}" >"${client_log}" 2>&1; then
      rc=0
    else
      rc=$?
      warn "  cliente de ${name} falhou (rc=${rc}):"
      tail -5 "${client_log}" | sed 's/^/    /' >&2
    fi
  else
    # Papel só de servidor: espera o cliente remoto terminar, ou o operador.
    log "  servidor de ${name} no ar na porta ${port}. Ctrl+C encerra."
    wait "${server_pid}" 2>/dev/null
    rc=$?
  fi

  # --- derrubada ------------------------------------------------------------
  # SIGTERM, e não SIGKILL: os servidores gravam o resumo no caminho de saída,
  # e um kill descartaria as métricas do lado servidor.
  if [[ -n "${server_pid:-}" ]]; then
    kill -TERM "${server_pid}" 2>/dev/null || true
    for i in $(seq 1 50); do
      kill -0 "${server_pid}" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
  if [[ -n "${tcpdump_pid}" ]]; then
    sleep 0.3
    kill -INT "${tcpdump_pid}" 2>/dev/null || true
    wait "${tcpdump_pid}" 2>/dev/null || true
  fi

  return "${rc}"
}

# ---------------------------------------------------------------------------
# Campanha
# ---------------------------------------------------------------------------

declare -A CHANNEL_OK CHANNEL_FAIL
for c in "${CHANNELS[@]}"; do CHANNEL_OK["${c}"]=0; CHANNEL_FAIL["${c}"]=0; done

for rep in $(seq 1 "${REPEAT}"); do
  REP_DIR="$(printf '%s/rep%02d' "${OUT_DIR}" "${rep}")"
  mkdir -p "${REP_DIR}"
  log "--- repetição ${rep}/${REPEAT} ---"

  for c in "${CHANNELS[@]}"; do
    printf '  %-10s ' "${c}"
    if run_channel "${c}" "${REP_DIR}"; then
      printf 'ok\n'
      CHANNEL_OK["${c}"]=$(( ${CHANNEL_OK["${c}"]} + 1 ))
    else
      printf 'FALHOU\n'
      CHANNEL_FAIL["${c}"]=$(( ${CHANNEL_FAIL["${c}"]} + 1 ))
    fi
  done
done

# ---------------------------------------------------------------------------
# Manifesto (R6)
# ---------------------------------------------------------------------------

wolfssl_version() {
  PKG_CONFIG_PATH="${WOLFSSL_DIR:-/usr/local}/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
    pkg-config --modversion wolfssl 2>/dev/null || echo desconhecida
}
libcoap_version() {
  PKG_CONFIG_PATH="${WOLFSSL_DIR:-/usr/local}/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
    pkg-config --modversion libcoap-3-wolfssl 2>/dev/null || echo desconhecida
}
libsrtp_version() {
  pkg-config --modversion libsrtp2 2>/dev/null || echo desconhecida
}

# Fingerprint de cada chave usada, para conferir que as duas VMs partilham a
# mesma e que os canais têm chaves distintas. Nunca a chave em si.
key_fingerprints() {
  local first=1 e name slug prefix
  for e in "${CHANNEL_SPEC[@]}"; do
    IFS=: read -r name slug prefix _ <<<"${e}"
    [[ " ${CHANNELS[*]} " == *" ${name} "* ]] || continue
    [[ -f "keys/${prefix}.key" ]] || continue
    local fp
    fp="$(xxd -r -p "keys/${prefix}.key" 2>/dev/null | openssl dgst -sha256 -binary 2>/dev/null | xxd -p -l 8)"
    [[ ${first} -eq 1 ]] || printf ',\n'
    first=0
    printf '      "%s": "%s"' "${slug}" "${fp}"
  done
  [[ ${first} -eq 0 ]] && printf '\n'
}

NETEM_DESC="nenhum"
[[ -n "${NETEM_PROFILE}" ]] && NETEM_DESC="${NETEM_PROFILE}"

{
  printf '{\n'
  printf '  "run_id": "%s",\n' "$(json_str "${RUN_ID}")"
  printf '  "timestamp_utc": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '  "git": {\n'
  printf '    "commit": "%s",\n' "$(json_str "${GIT_COMMIT}")"
  printf '    "dirty": %s\n' "${GIT_DIRTY}"
  printf '  },\n'
  printf '  "ambiente": {\n'
  printf '    "kernel": "%s",\n' "$(json_str "$(uname -r)")"
  printf '    "uname": "%s",\n' "$(json_str "$(uname -sm)")"
  printf '    "wolfssl": "%s",\n' "$(json_str "$(wolfssl_version)")"
  printf '    "libcoap": "%s",\n' "$(json_str "$(libcoap_version)")"
  printf '    "libsrtp2": "%s",\n' "$(json_str "$(libsrtp_version)")"
  printf '    "wolfssl_prefix": "%s"\n' "$(json_str "${WOLFSSL_DIR:-/usr/local}")"
  printf '  },\n'
  printf '  "netem": "%s",\n' "$(json_str "${NETEM_DESC}")"
  printf '  "parametros": {\n'
  printf '    "papel": "%s",\n' "$(json_str "${ROLE}")"
  printf '    "host": "%s",\n' "$(json_str "${HOST}")"
  printf '    "duracao_s": %s,\n' "${DURATION}"
  printf '    "repeticoes": %s,\n' "${REPEAT}"
  printf '    "bulk_mib": %s,\n' "${BULK_MIB}"
  printf '    "port_offset": %s,\n' "${PORT_OFFSET}"
  printf '    "captura_pcap": %s\n' "$([[ ${CAPTURE} -eq 1 ]] && echo true || echo false)"
  printf '  },\n'
  printf '  "canais": [%s],\n' "$(printf '"%s",' "${CHANNELS[@]}" | sed 's/,$//')"
  printf '  "chaves_fingerprint": {\n'
  key_fingerprints
  printf '  },\n'
  printf '  "resultado": {\n'
  first=1
  for c in "${CHANNELS[@]}"; do
    [[ ${first} -eq 1 ]] || printf ',\n'
    first=0
    printf '    "%s": { "ok": %s, "falhas": %s }' \
      "${c}" "${CHANNEL_OK[${c}]}" "${CHANNEL_FAIL[${c}]}"
  done
  printf '\n  }\n'
  printf '}\n'
} > "${OUT_DIR}/manifest.json"

# ---------------------------------------------------------------------------
# Relatório
# ---------------------------------------------------------------------------

log ""
log "=============================================================="
log " Resultado"
log "=============================================================="
TOTAL_FAIL=0
for c in "${CHANNELS[@]}"; do
  printf ' %-10s %d ok, %d falhas\n' "${c}" "${CHANNEL_OK[${c}]}" "${CHANNEL_FAIL[${c}]}"
  TOTAL_FAIL=$(( TOTAL_FAIL + ${CHANNEL_FAIL[${c}]} ))
done
log ""
log " Resultados em ${OUT_DIR}"
log " Manifesto   ${OUT_DIR}/manifest.json"

[[ "${TOTAL_FAIL}" -eq 0 ]]
