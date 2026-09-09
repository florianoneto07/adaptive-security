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
#   --allow-stale              mede mesmo com binário mais antigo que o código
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
ALLOW_STALE=0

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

# Canais em que o CLIENTE mede ida e volta. O de mídia fica de fora: o fluxo é
# unidirecional e quem registra o atraso é o receptor.
CHANNELS_WITH_CLIENT_RTT="control telemetry bulk"

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
    --allow-stale) ALLOW_STALE=1; shift ;;
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

# Binário mais antigo que o código que o gera: a campanha rodaria com uma versão
# diferente da que o manifesto vai registrar, e o resultado seria atribuído ao
# commit errado. Aconteceu numa campanha de 35 minutos, cujo único sintoma foi
# uma correção que "não funcionou".
STALE=()
for c in "${CHANNELS[@]}"; do
  IFS=: read -r _ slug prefix _ <<<"$(spec_for "${c}")"
  for role_bin in server client; do
    [[ "${ROLE}" == "server" && "${role_bin}" == "client" ]] && continue
    [[ "${ROLE}" == "client" && "${role_bin}" == "server" ]] && continue

    bin="build/${prefix}_${role_bin}"
    [[ -f "${bin}" ]] || continue

    while IFS= read -r src; do
      [[ -n "${src}" && "${src}" -nt "${bin}" ]] && { STALE+=("${bin}"); break; }
    done < <(printf '%s\n' "channels/${slug}/${prefix}_${role_bin}.c" \
                            channels/"${slug}"/*.h common/*.c common/*.h)
  done
done

if [[ "${#STALE[@]}" -gt 0 ]]; then
  warn ""
  warn "Estes binários são mais antigos que o código-fonte:"
  printf '  %s\n' "${STALE[@]}" >&2
  warn ""
  warn "Recompile antes de medir:"
  warn "  make WOLFSSL_DIR=\"${WOLFSSL_DIR:-\$HOME/.local}\""
  warn ""
  warn "Para medir assim mesmo, use --allow-stale."
  [[ "${ALLOW_STALE}" -eq 1 ]] || exit 1
  warn "Prosseguindo por --allow-stale."
fi

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
STOP=0

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
on_interrupt() {
  STOP=1
  warn ""
  warn "Interrompido. Encerrando os canais em andamento..."
}

trap cleanup EXIT
trap on_interrupt INT TERM

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

# Um canal pode sair com código de sucesso e não ter trocado mensagem nenhuma.
# Foi assim que uma campanha inteira de telemetria passou por boa, com nove de
# dez repetições sem uma única resposta: o binário retornava 0 e o orquestrador
# acreditava. Esta é a segunda rede: conferir o que o canal de fato produziu,
# em vez de confiar só no código de saída.
#
# Devolve 0 (verdadeiro) quando o canal enviou mensagens e não obteve resposta.
channel_produced_nothing() {
  local name="$1" dir="$2" slug f sent samples

  [[ " ${CHANNELS_WITH_CLIENT_RTT} " == *" ${name} "* ]] || return 1
  [[ "${ROLE}" == "server" ]] && return 1

  IFS=: read -r _ slug _ _ <<<"$(spec_for "${name}")"
  f="${dir}/${slug}_client.summary"
  [[ -f "${f}" ]] || return 1

  sent="$(awk -F= '/^msgs_sent=/{print $2}' "${f}")"
  samples="$(awk -F= '/^rtt_samples=/{print $2}' "${f}")"

  [[ "${sent:-0}" -gt 0 && "${samples:-0}" -eq 0 ]]
}

declare -A CHANNEL_OK CHANNEL_FAIL
for c in "${CHANNELS[@]}"; do CHANNEL_OK["${c}"]=0; CHANNEL_FAIL["${c}"]=0; done

# ---------------------------------------------------------------------------
# Papel de servidor: todos os canais no ar, em PARALELO
# ---------------------------------------------------------------------------
#
# Os servidores precisam coexistir, não se revezar. Um canal é independente do
# outro (R1), e o cliente percorre os quatro dentro de cada repetição — se os
# servidores subissem em fila, esperando cada um terminar, o canal de telemetria
# (que atende indefinidamente, por natureza) seguraria a fila para sempre e os
# servidores de mídia e volumoso nunca chegariam a escutar. O sintoma no cliente
# é "error state on socket" no UDP e "connection refused" no TCP.
#
# Ficam persistentes (-k) para atender as repetições sucessivas do cliente. O
# servidor de telemetria não tem essa opção porque já atende em laço contínuo.
#
# As métricas do servidor vão para <run>/server/, e não por repetição: o
# processo é um só para toda a campanha.
REPEAT_PEDIDO="${REPEAT}"

if [[ "${ROLE}" == "server" ]]; then
  SRV_DIR="${OUT_DIR}/server"
  mkdir -p "${SRV_DIR}"

  for c in "${CHANNELS[@]}"; do
    IFS=: read -r _ slug prefix port <<<"$(spec_for "${c}")"
    port=$((port + PORT_OFFSET))

    srv_args=(-p "${port}" -o "${SRV_DIR}")
    [[ "${c}" != "telemetry" ]] && srv_args+=(-k)

    if [[ "${CAPTURE}" -eq 1 ]]; then
      proto="udp"; [[ "${c}" == "bulk" ]] && proto="tcp"
      tcpdump -i any -w "${SRV_DIR}/${slug}.pcap" -U "${proto} port ${port}" \
        >"${SRV_DIR}/${slug}_tcpdump.log" 2>&1 &
      TCPDUMP_PIDS+=($!)
    fi

    "./build/${prefix}_server" "${srv_args[@]}" \
      >"${SRV_DIR}/${slug}_server.log" 2>&1 &
    SERVER_PIDS+=($!)
    CHANNEL_OK["${c}"]=1
  done

  sleep 1
  log "Servidores no ar:"
  ok_count=0
  for c in "${CHANNELS[@]}"; do
    IFS=: read -r _ slug prefix port <<<"$(spec_for "${c}")"
    port=$((port + PORT_OFFSET))
    if grep -q "aguardando" "${SRV_DIR}/${slug}_server.log" 2>/dev/null; then
      printf '  %-10s porta %s\n' "${c}" "${port}"
      ok_count=$((ok_count + 1))
    else
      printf '  %-10s porta %s  NAO SUBIU\n' "${c}" "${port}"
      sed 's/^/      /' "${SRV_DIR}/${slug}_server.log" >&2
      CHANNEL_OK["${c}"]=0
      CHANNEL_FAIL["${c}"]=1
    fi
  done

  if [[ "${ok_count}" -eq 0 ]]; then
    warn "Nenhum servidor subiu."
    exit 1
  fi

  log ""
  log "Rode a campanha na VM cliente. Ctrl+C encerra os servidores."
  while [[ "${STOP}" -eq 0 ]]; do
    sleep 1
  done
  log ""
  log "Encerrando. Métricas do servidor em ${SRV_DIR}"

  REPEAT=0   # não há repetições do lado servidor; pula o laço abaixo
             # (REPEAT_PEDIDO preserva o valor para o manifesto)
fi

for rep in $(seq 1 "${REPEAT}"); do
  [[ "${STOP}" -eq 1 ]] && break
  REP_DIR="$(printf '%s/rep%02d' "${OUT_DIR}" "${rep}")"
  mkdir -p "${REP_DIR}"
  log "--- repetição ${rep}/${REPEAT} ---"

  for c in "${CHANNELS[@]}"; do
    [[ "${STOP}" -eq 1 ]] && break
    printf '  %-10s ' "${c}"
    if run_channel "${c}" "${REP_DIR}"; then
      if channel_produced_nothing "${c}" "${REP_DIR}"; then
        printf 'SEM RESPOSTA\n'
        CHANNEL_FAIL["${c}"]=$(( ${CHANNEL_FAIL["${c}"]} + 1 ))
      else
        printf 'ok\n'
        CHANNEL_OK["${c}"]=$(( ${CHANNEL_OK["${c}"]} + 1 ))
      fi
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

# O netem efetivamente ativo na interface, e não o que este script aplicou.
#
# Entre duas VMs o perfil precisa ser aplicado nos dois lados, e o caminho
# natural é chamar ./netem.sh à mão em cada uma — nesse caso o run.sh não
# aplicou nada e registraria "nenhum", fazendo o manifesto afirmar enlace limpo
# numa campanha que teve 50 ms de atraso e perda induzida. Ler o qdisc responde
# o que de fato valia, independentemente de quem o aplicou.
netem_effective() {
  local iface qdisc

  iface="$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')"
  if [[ -z "${iface}" ]]; then
    printf 'desconhecido'
    return
  fi

  qdisc="$(tc qdisc show dev "${iface}" 2>/dev/null | grep -m1 netem)"
  if [[ -z "${qdisc}" ]]; then
    printf 'nenhum'
    return
  fi

  # Descarta handle, refcnt e seed: mudam a cada aplicação do mesmo perfil e
  # fariam duas execuções idênticas parecerem diferentes.
  printf '%s' "${qdisc}" |
    sed -E 's/^qdisc netem [0-9a-f]+: root refcnt [0-9]+ //; s/ seed [0-9]+//; s/limit [0-9]+ //'
}

netem_iface() {
  ip route show default 2>/dev/null | awk '/default/ {print $5; exit}'
}

NETEM_DESC="$(netem_effective)"
NETEM_REQUESTED="nenhum"
[[ -n "${NETEM_PROFILE}" ]] && NETEM_REQUESTED="${NETEM_PROFILE}"

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
  printf '  "netem": {\n'
  printf '    "efetivo": "%s",\n' "$(json_str "${NETEM_DESC}")"
  printf '    "solicitado": "%s",\n' "$(json_str "${NETEM_REQUESTED}")"
  printf '    "interface": "%s"\n' "$(json_str "$(netem_iface)")"
  printf '  },\n'
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
