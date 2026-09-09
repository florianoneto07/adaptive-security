#!/usr/bin/env bash
#
# Campanha completa: as três condições de enlace, em sequência, sem depender de
# o operador estar presente.
#
# Roda na VM SERVIDORA e comanda a cliente por SSH, porque o netem precisa valer
# nos dois lados e uma campanha de mais de uma hora não deve depender de alguém
# lembrar de aplicá-lo em cada máquina, na ordem certa, entre condições.
#
# Uso:
#   ./campaign.sh --client-ssh lab-client --host 192.168.218.128
#
# Opções:
#   --client-ssh ALVO     host SSH da VM cliente (alias do ~/.ssh/config)
#   --host ENDERECO       endereço DESTA máquina, como o cliente a enxerga
#   --client-dir CAMINHO  repositório na VM cliente (padrão ~/adaptive-security)
#   --conditions LISTA    condições, separadas por vírgula
#                         (padrão: clean,3gpp-c2,handover)
#   --duration SEGUNDOS   por execução (padrão 60)
#   --repeat N            repetições por condição (padrão 10)
#   --channels LISTA      canais (padrão: all)
#   --capture             grava pcap (necessário para bytes no fio do C2)
#   --out DIR             raiz da campanha (padrão results)
#   --dry-run             mostra o que faria, sem executar
#
# Saída, pensada para virar gráfico sem retrabalho:
#
#   results/campaign-<timestamp>/
#     clean/       servidor/ e cliente/ com CSVs brutos e manifestos
#     3gpp-c2/
#     handover/
#     resumo.csv   uma linha por (condição, canal) — é o arquivo do gráfico
#     campaign.log
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${REPO_ROOT}"

CLIENT_SSH=""
HOST=""
CLIENT_DIR="~/adaptive-security"
CONDITIONS="clean,3gpp-c2,handover"
DURATION=60
REPEAT=10
CHANNELS="all"
CAPTURE=0
OUT_ROOT="results"
DRY_RUN=0

usage() { sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --client-ssh) CLIENT_SSH="${2:-}"; shift 2 ;;
    --host)       HOST="${2:-}"; shift 2 ;;
    --client-dir) CLIENT_DIR="${2:-}"; shift 2 ;;
    --conditions) CONDITIONS="${2:-}"; shift 2 ;;
    --duration)   DURATION="${2:-}"; shift 2 ;;
    --repeat)     REPEAT="${2:-}"; shift 2 ;;
    --channels)   CHANNELS="${2:-}"; shift 2 ;;
    --out)        OUT_ROOT="${2:-}"; shift 2 ;;
    --capture)    CAPTURE=1; shift ;;
    --dry-run)    DRY_RUN=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    *) echo "Opção desconhecida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -z "${CLIENT_SSH}" || -z "${HOST}" ]]; then
  echo "--client-ssh e --host são obrigatórios." >&2
  echo >&2
  usage >&2
  exit 1
fi

log() { printf '%s\n' "$*" | tee -a "${CAMPAIGN_LOG:-/dev/null}"; }
warn() { printf '%s\n' "$*" >&2; [[ -n "${CAMPAIGN_LOG:-}" ]] && printf '%s\n' "$*" >> "${CAMPAIGN_LOG}"; }

run_remote() { ssh -o BatchMode=yes "${CLIENT_SSH}" "$@"; }

# ---------------------------------------------------------------------------
# Pré-condições
# ---------------------------------------------------------------------------

if ! ssh -o BatchMode=yes -o ConnectTimeout=8 "${CLIENT_SSH}" true 2>/dev/null; then
  echo "SSH sem senha para '${CLIENT_SSH}' não está funcionando." >&2
  exit 1
fi

# Binário desatualizado de um lado invalida a campanha inteira, e a checagem
# custa um segundo contra mais de uma hora de medição perdida. Foi assim que
# uma campanha rodou com binários de três horas antes.
log_pre() { printf '%s\n' "$*"; }
log_pre "Conferindo os dois lados..."

LOCAL_COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo '?')"
REMOTE_COMMIT="$(run_remote "cd ${CLIENT_DIR} && git rev-parse --short HEAD" 2>/dev/null || echo '?')"

log_pre "  servidor: ${LOCAL_COMMIT}"
log_pre "  cliente:  ${REMOTE_COMMIT}"

if [[ "${LOCAL_COMMIT}" != "${REMOTE_COMMIT}" ]]; then
  warn ""
  warn "As duas VMs estão em commits diferentes. Os resultados seriam atribuídos"
  warn "a uma versão que só vale para um dos lados."
  warn ""
  warn "No cliente:  cd ${CLIENT_DIR} && git pull && make WOLFSSL_DIR=\"\$HOME/.local\""
  # Num ensaio a divergência é informação, não impedimento.
  [[ "${DRY_RUN}" -eq 1 ]] || exit 1
  warn ""
fi

for f in ./netem.sh ./run.sh ./scripts/summarize.py; do
  [[ -x "${f}" ]] || { echo "Faltando: ${f}" >&2; exit 1; }
done

# ---------------------------------------------------------------------------
# Estrutura da campanha
# ---------------------------------------------------------------------------

# Aplicar o netem exige CAP_NET_ADMIN nos DOIS lados. Descobrir que falta no
# cliente só ao chegar na segunda condição custa a primeira hora de campanha —
# foi o que aconteceu. Um teste de um segundo evita isso.
needs_netem=0
for c in $(tr ',' ' ' <<< "${CONDITIONS}"); do
  [[ "${c}" != "clean" ]] && needs_netem=1
done

if [[ "${needs_netem}" -eq 1 && "${DRY_RUN}" -eq 0 ]]; then
  log_pre "Conferindo permissão do tc nas duas VMs..."
  netem_blocked=""

  ./netem.sh 3gpp-c2 >/dev/null 2>&1 || netem_blocked="servidor"
  ./netem.sh off >/dev/null 2>&1

  run_remote "cd ${CLIENT_DIR} && ./netem.sh 3gpp-c2" >/dev/null 2>&1 ||
    netem_blocked="${netem_blocked:+${netem_blocked} e }cliente"
  run_remote "cd ${CLIENT_DIR} && ./netem.sh off" >/dev/null 2>&1

  if [[ -n "${netem_blocked}" ]]; then
    warn ""
    warn "O tc não pode aplicar netem em: ${netem_blocked}."
    warn "As condições degradadas seriam puladas, e só a linha de base seria colhida."
    warn ""
    warn "Rode na(s) máquina(s) afetada(s), uma vez:"
    warn "  sudo setcap cap_net_admin+eip /usr/sbin/tc"
    warn ""
    warn "Para colher só a linha de base agora: --conditions clean"
    exit 1
  fi
  log_pre "  ok nas duas"
fi

CAMPAIGN_ID="campaign-$(date -u +%Y%m%dT%H%M%SZ)-${LOCAL_COMMIT}"
CAMPAIGN_DIR="${OUT_ROOT}/${CAMPAIGN_ID}"
CAMPAIGN_LOG="${CAMPAIGN_DIR}/campaign.log"
SUMMARY_CSV="${CAMPAIGN_DIR}/resumo.csv"

if [[ "${DRY_RUN}" -eq 0 ]]; then
  mkdir -p "${CAMPAIGN_DIR}"
  : > "${CAMPAIGN_LOG}"
else
  # Num ensaio nada é criado em disco, nem o log.
  CAMPAIGN_LOG="/dev/null"
fi

IFS=',' read -r -a COND_LIST <<< "${CONDITIONS}"

log "=============================================================="
log " Campanha: ${CAMPAIGN_ID}"
log "=============================================================="
log " commit      ${LOCAL_COMMIT} (as duas VMs)"
log " condições   ${COND_LIST[*]}"
log " canais      ${CHANNELS}"
log " duração     ${DURATION}s x ${REPEAT} repetições por condição"
log " cliente     ${CLIENT_SSH}:${CLIENT_DIR}"
log " servidor    ${HOST}"
[[ "${CAPTURE}" -eq 1 ]] && log " captura     pcap habilitado"
log ""

# Estimativa: ajuda a decidir se dá para acompanhar ou se é caso de deixar
# rodando. Cada canal consome DURATION, exceto o volumoso, que é mais rápido.
n_ch=4
[[ "${CHANNELS}" != "all" ]] && n_ch=$(wc -w <<< "${CHANNELS}")
est_min=$(( ${#COND_LIST[@]} * REPEAT * n_ch * DURATION / 60 ))
log " estimativa  ~${est_min} min"
log ""

# ---------------------------------------------------------------------------
# netem nas duas pontas
# ---------------------------------------------------------------------------

apply_netem() {
  local profile="$1" rc=0

  if [[ "${profile}" == "clean" ]]; then
    ./netem.sh off >/dev/null 2>&1 || rc=1
    run_remote "cd ${CLIENT_DIR} && ./netem.sh off" >/dev/null 2>&1 || rc=1
    return "${rc}"
  fi

  ./netem.sh "${profile}" >/dev/null 2>&1 || rc=1
  run_remote "cd ${CLIENT_DIR} && ./netem.sh ${profile}" >/dev/null 2>&1 || rc=1
  return "${rc}"
}

clear_netem() {
  ./netem.sh off >/dev/null 2>&1 || true
  run_remote "cd ${CLIENT_DIR} && ./netem.sh off" >/dev/null 2>&1 || true
}

SERVER_PID=""
cleanup() {
  [[ -n "${SERVER_PID}" ]] && kill -TERM "${SERVER_PID}" 2>/dev/null
  clear_netem
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Uma condição
# ---------------------------------------------------------------------------

run_condition() {
  local cond="$1"
  local cond_dir="${CAMPAIGN_DIR}/${cond}"
  local srv_out="${cond_dir}/servidor"
  local cli_out="${cond_dir}/cliente"
  local srv_args=() cli_args=()

  log "--------------------------------------------------------------"
  log " Condição: ${cond}"
  log "--------------------------------------------------------------"

  if ! apply_netem "${cond}"; then
    warn " netem '${cond}' não pôde ser aplicado nas duas VMs; pulando."
    return 1
  fi
  log " netem aplicado nas duas VMs: $( [[ ${cond} == clean ]] && echo 'removido (linha de base)' || echo "${cond}" )"

  mkdir -p "${srv_out}" "${cli_out}"

  srv_args=(--role server --out "${srv_out}" --duration "${DURATION}" --repeat "${REPEAT}")
  [[ "${CAPTURE}" -eq 1 ]] && srv_args+=(--capture)
  # shellcheck disable=SC2206
  srv_args+=(${CHANNELS})

  ./run.sh "${srv_args[@]}" > "${cond_dir}/servidor.log" 2>&1 &
  SERVER_PID=$!

  # Espera os servidores anunciarem que escutam, em vez de dormir um tempo fixo.
  local up=0 i
  for i in $(seq 1 100); do
    grep -q "Servidores no ar" "${cond_dir}/servidor.log" 2>/dev/null && { up=1; break; }
    kill -0 "${SERVER_PID}" 2>/dev/null || break
    sleep 0.2
  done
  if [[ "${up}" -eq 0 ]]; then
    warn " servidores não subiram:"
    sed 's/^/   /' "${cond_dir}/servidor.log" >&2
    kill -INT "${SERVER_PID}" 2>/dev/null; SERVER_PID=""
    return 1
  fi
  if grep -q "NAO SUBIU" "${cond_dir}/servidor.log" 2>/dev/null; then
    warn " algum servidor não subiu:"
    grep -A4 "NAO SUBIU" "${cond_dir}/servidor.log" | sed 's/^/   /' >&2
  fi
  log " servidores no ar"

  # --- cliente, remoto ---
  cli_args="--role client --host ${HOST} --duration ${DURATION} --repeat ${REPEAT} --out results/${CAMPAIGN_ID}-${cond}"
  log " rodando o cliente (${REPEAT} repetições)..."

  # Em tee, não redirecionado: sem isso a campanha fica quarenta minutos por
  # condição sem imprimir nada, e não há como saber se avançou ou travou.
  local cli_rc=0
  run_remote "cd ${CLIENT_DIR} && ./run.sh ${cli_args} ${CHANNELS}" 2>&1 \
    | sed 's/^/   | /' | tee -a "${cond_dir}/cliente.log"
  cli_rc="${PIPESTATUS[0]}"

  if [[ "${cli_rc}" -ne 0 ]]; then
    warn " cliente terminou com rc=${cli_rc}; veja ${cond_dir}/cliente.log"
    grep -E "FALHOU|SEM RESPOSTA" "${cond_dir}/cliente.log" | head -5 | sed 's/^/   /' >&2
  fi

  # --- derruba os servidores para que gravem os resumos ---
  #
  # SIGTERM, e não SIGINT: um script iniciado em background herda SIGINT
  # ignorado, e o kill -INT não tinha efeito nenhum — a campanha ficava presa
  # em wait() indefinidamente depois de o cliente já ter terminado. Custou 40
  # minutos de espera numa condição que já estava pronta.
  kill -TERM "${SERVER_PID}" 2>/dev/null
  wait "${SERVER_PID}" 2>/dev/null
  SERVER_PID=""
  sleep 1

  # --- traz os resultados do cliente ---
  log " coletando resultados do cliente..."
  if ! scp -q -r "${CLIENT_SSH}:${CLIENT_DIR}/results/${CAMPAIGN_ID}-${cond}" \
       "${cli_out}/" 2>/dev/null; then
    warn " não foi possível copiar os resultados do cliente."
  fi

  # --- agrega esta condição ---
  local srv_run cli_run
  srv_run="$(find "${srv_out}" -maxdepth 1 -mindepth 1 -type d | head -1)"
  cli_run="$(find "${cli_out}" -maxdepth 1 -mindepth 1 -type d | head -1)"

  if [[ -n "${srv_run}" && -n "${cli_run}" ]]; then
    ./scripts/summarize.py "${cli_run}" "${srv_run}" \
      --csv "${SUMMARY_CSV}" --label "${cond}" --append \
      | tee -a "${CAMPAIGN_LOG}"
  else
    warn " faltam dados para agregar a condição ${cond}"
  fi

  log ""
  return 0
}

# ---------------------------------------------------------------------------
# Execução
# ---------------------------------------------------------------------------

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "Faria, em sequência:"
  for cond in "${COND_LIST[@]}"; do
    echo "  ${cond}: netem nas duas VMs, ${REPEAT}x${DURATION}s por canal, agregar"
  done
  echo "Saída em ${CAMPAIGN_DIR}"
  exit 0
fi

FAILED=()
for cond in "${COND_LIST[@]}"; do
  run_condition "${cond}" || FAILED+=("${cond}")
done

clear_netem

log "=============================================================="
log " Campanha concluída"
log "=============================================================="
if [[ "${#FAILED[@]}" -gt 0 ]]; then
  log " condições com problema: ${FAILED[*]}"
fi
log " resultados  ${CAMPAIGN_DIR}"
log " tabela      ${SUMMARY_CSV}"
log ""
log " Para os bytes no fio do C2 (sem contadores internos):"
log "   ./scripts/pcap_bytes.py ${CAMPAIGN_DIR}/*/servidor/*/server/c2_telemetry.pcap"

[[ "${#FAILED[@]}" -eq 0 ]]
