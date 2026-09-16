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
#   ./campaign.sh --client-ssh lab-client --host 192.168.218.130
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
#   --bulk-size MiB       tamanho da transferência do C4 (padrão do run.sh: 32).
#                         Reduza nas varreduras de perda alta, onde 32 MiB leva
#                         dezenas de minutos por repetição.
#   --capture             grava pcap (necessário para bytes no fio do C2)
#   --capture-channels L  captura só estes canais. O pcap só é indispensável no
#                         C2 (~0 MB); capturar o C3 custa ~153 MB por condição e
#                         já encheu o disco da VM.
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
BULK_MIB=""          # vazio: usa o padrão do run.sh (32 MiB)
CAPTURE=0
CAPTURE_CHANNELS=""   # vazio: captura todos os canais da campanha
OUT_ROOT="results"
DRY_RUN=0

# Imprime o cabeçalho de comentário até a primeira linha de código, sem depender
# de um número de linha fixo (que já ficou defasado ao editar o cabeçalho).
usage() { awk 'NR>=2 && /^#/{sub(/^# ?/,""); print; next} NR>=2{exit}' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --client-ssh) CLIENT_SSH="${2:-}"; shift 2 ;;
    --host)       HOST="${2:-}"; shift 2 ;;
    --client-dir) CLIENT_DIR="${2:-}"; shift 2 ;;
    --conditions) CONDITIONS="${2:-}"; shift 2 ;;
    --duration)   DURATION="${2:-}"; shift 2 ;;
    --repeat)     REPEAT="${2:-}"; shift 2 ;;
    --channels)   CHANNELS="${2:-}"; shift 2 ;;
    --bulk-size)  BULK_MIB="${2:-}"; shift 2 ;;
    --out)        OUT_ROOT="${2:-}"; shift 2 ;;
    --capture)    CAPTURE=1; shift ;;
    --capture-channels) CAPTURE_CHANNELS="${2:-}"; shift 2 ;;
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

run_remote() { ssh -o BatchMode=yes -o ConnectTimeout=20 "${CLIENT_SSH}" "$@"; }

# Sondagem do cliente enquanto ele roda. O netem vale para a interface inteira,
# então cada ssh atravessa o enlace degradado que está sendo medido: sob
# handover (20% de perda em rajada) uma sessão longa não sobrevive. Uma
# sondagem, ao contrário, pode falhar à vontade — a seguinte repete do mesmo
# ponto. O intervalo é escolha de engenharia do testbed, não vem da literatura.
POLL_INTERVAL=15
poll_remote() {
  timeout 90 ssh -o BatchMode=yes -o ConnectTimeout=15 \
    -o ServerAliveInterval=10 -o ServerAliveCountMax=3 "${CLIENT_SSH}" "$@"
}

# Comando de uma sondagem: código de saída (se já houver), se o processo vive,
# tamanho do log e o trecho do log ainda não mostrado — tudo num ssh só, para
# que uma falha de rede não deixe os quatro dados inconsistentes entre si.
# Os '$' ficam para o shell REMOTO expandir.
poll_cmd() {
  local ctl="$1" shown="$2"
  printf 'cd %s; ' "${CLIENT_DIR}"
  printf 'if [ -f %s.rc ]; then echo RC=$(cat %s.rc); else echo RC=-; fi; ' "${ctl}" "${ctl}"
  printf 'if kill -0 $(cat %s.pid 2>/dev/null) 2>/dev/null; then echo ALIVE=1; else echo ALIVE=0; fi; ' "${ctl}"
  printf 'n=$(wc -c < %s.log 2>/dev/null || echo 0); echo SIZE=$n; ' "${ctl}"
  printf 'tail -c +%d %s.log 2>/dev/null | head -c $((n - %d))' "$((shown + 1))" "${ctl}" "${shown}"
}

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
[[ -n "${BULK_MIB}" ]] && log " bulk C4     ${BULK_MIB} MiB (padrão 32)"
log " cliente     ${CLIENT_SSH}:${CLIENT_DIR}"
log " servidor    ${HOST}"
[[ "${CAPTURE}" -eq 1 ]] && log " captura     pcap habilitado${CAPTURE_CHANNELS:+ (só: ${CAPTURE_CHANNELS})}"
log ""

# Estimativa: ajuda a decidir se dá para acompanhar ou se é caso de deixar
# rodando. É um PISO: o volumoso transfere um tamanho fixo, não um tempo fixo,
# e sob perda alta demora muito mais (handover: ~17 min por repetição, contra
# 2 s em enlace limpo).
n_ch=4
[[ "${CHANNELS}" != "all" ]] && n_ch=$(wc -w <<< "${CHANNELS}")
est_min=$(( ${#COND_LIST[@]} * REPEAT * n_ch * DURATION / 60 ))
log " estimativa  >=${est_min} min (o volumoso pode estender muito sob perda)"
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
CLIENT_PIDFILE=""   # pid do cliente remoto em execução, para abortar junto
CLEANED=0

# SIGTERM, e não SIGINT: um script iniciado em background herda SIGINT
# ignorado, e o kill -INT não tinha efeito nenhum — a campanha ficava presa
# em wait() indefinidamente depois de o cliente já ter terminado. Custou 40
# minutos de espera numa condição que já estava pronta.
stop_server() {
  [[ -n "${SERVER_PID}" ]] || return 0
  kill -TERM "${SERVER_PID}" 2>/dev/null
  wait "${SERVER_PID}" 2>/dev/null
  SERVER_PID=""
  sleep 1
}

cleanup() {
  [[ "${CLEANED}" -eq 1 ]] && return 0
  CLEANED=1
  [[ -n "${SERVER_PID}" ]] && kill -TERM "${SERVER_PID}" 2>/dev/null
  # O cliente é líder de sessão (setsid): o sinal ao grupo alcança o run.sh e
  # os binários de canal abaixo dele.
  if [[ -n "${CLIENT_PIDFILE}" ]]; then
    run_remote "cd ${CLIENT_DIR} && kill -TERM -- -\$(cat ${CLIENT_PIDFILE}) 2>/dev/null" 2>/dev/null || true
  fi
  clear_netem
}
trap cleanup EXIT
# Sem o exit, o Ctrl-C limpava e a campanha seguia para a condição seguinte.
trap 'cleanup; exit 130' INT TERM

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
  [[ -n "${CAPTURE_CHANNELS}" ]] && srv_args+=(--capture-channels "${CAPTURE_CHANNELS}")
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

  # --- cliente, remoto e desacoplado da sessão SSH ---
  #
  # A sessão SSH que comandava o cliente atravessava o mesmo enlace degradado
  # que estava sendo medido, e sob handover caiu na terceira repetição levando
  # o cliente junto: rc=255 do ssh, 4 de 10 repetições. O cliente agora roda
  # em sessão própria na VM cliente (setsid), com a saída em arquivo, e o que
  # atravessa o enlace são sondagens curtas e descartáveis.
  local remote_out="results/${CAMPAIGN_ID}-${cond}"
  local remote_ctl="${remote_out}.cliente"   # .pid, .rc e .log, ao lado do diretório
  cli_args="--role client --host ${HOST} --duration ${DURATION} --repeat ${REPEAT} --out ${remote_out}"
  [[ -n "${BULK_MIB}" ]] && cli_args="${cli_args} --bulk-size ${BULK_MIB}"

  # Idempotente: se o ssh devolver 255 depois de já ter lançado o cliente,
  # repetir o comando não lança um segundo por cima do primeiro.
  local launch_cmd
  launch_cmd="cd ${CLIENT_DIR} && mkdir -p results && \
    if ! { [ -f ${remote_ctl}.pid ] && kill -0 \$(cat ${remote_ctl}.pid) 2>/dev/null; }; then \
      setsid bash -c 'echo \$\$ > ${remote_ctl}.pid; ./run.sh ${cli_args} ${CHANNELS}; echo \$? > ${remote_ctl}.rc' \
        > ${remote_ctl}.log 2>&1 < /dev/null & \
    fi"

  local attempt launched=0
  for attempt in 1 2 3; do
    if run_remote "${launch_cmd}"; then launched=1; break; fi
    warn " lançamento do cliente falhou (tentativa ${attempt}/3); repetindo em 10s"
    sleep 10
  done
  if [[ "${launched}" -eq 0 ]]; then
    warn " não foi possível lançar o cliente; pulando a condição."
    stop_server
    return 1
  fi
  CLIENT_PIDFILE="${remote_ctl}.pid"
  log " cliente lançado (${REPEAT} repetições); sondando a cada ${POLL_INTERVAL}s..."

  # Sondagem até o cliente registrar o código de saída. O trecho novo do log é
  # mostrado na hora — sem isso a campanha fica quarenta minutos por condição
  # sem imprimir nada — e acumulado em cliente.log por contagem de BYTES, não
  # de linhas, para que uma linha cortada ao meio não seja pulada na seguinte.
  local shown=0 out="" body="" cli_rc="" size=0 pad=0
  local rc_line alive_line size_line dead_polls=0 warned_slow=0 t0 elapsed
  local nominal=$(( REPEAT * n_ch * DURATION ))
  t0=$(date +%s)
  : > "${cond_dir}/cliente.log"
  while :; do
    sleep "${POLL_INTERVAL}"
    if ! out="$(poll_remote "$(poll_cmd "${remote_ctl}" "${shown}")")"; then
      warn " sondagem falhou (ssh); repetindo em ${POLL_INTERVAL}s"
      continue
    fi
    rc_line="$(sed -n 1p <<< "${out}")"
    alive_line="$(sed -n 2p <<< "${out}")"
    size_line="$(sed -n 3p <<< "${out}")"
    if [[ "${rc_line}" != RC=* || "${alive_line}" != ALIVE=* || "${size_line}" != SIZE=* ]]; then
      warn " sondagem devolveu resposta inesperada; repetindo"
      continue
    fi
    size="${size_line#SIZE=}"
    if (( size > shown )); then
      body="$(sed -n '4,$p' <<< "${out}")"
      # $(...) come as quebras de linha finais; o tamanho remoto diz quantas eram.
      printf '%s' "${body}" >> "${cond_dir}/cliente.log"
      pad=$(( size - shown - $(printf '%s' "${body}" | wc -c) ))
      (( pad > 0 )) && printf '%*s' "${pad}" '' | tr ' ' '\n' >> "${cond_dir}/cliente.log"
      printf '%s\n' "${body}" | sed 's/^/   | /'
      shown="${size}"
    fi

    if [[ "${rc_line}" != "RC=-" ]]; then
      cli_rc="${rc_line#RC=}"
      break
    fi
    # Sem rc e sem processo: morreu sem registrar (VM reiniciada, kill -9).
    # Duas sondagens seguidas, para não confundir com a janela entre o fim do
    # run.sh e a gravação do rc.
    if [[ "${alive_line}" == "ALIVE=0" ]]; then
      dead_polls=$(( dead_polls + 1 ))
      if (( dead_polls >= 2 )); then
        warn " o cliente sumiu sem registrar o código de saída"
        cli_rc=255
        break
      fi
    else
      dead_polls=0
    fi
    elapsed=$(( $(date +%s) - t0 ))
    if (( elapsed > 2 * nominal + 600 && warned_slow == 0 )); then
      warn " cliente já passou do dobro do tempo nominal (${elapsed}s); seguindo, o volumoso demora sob perda"
      warned_slow=1
    fi
  done
  CLIENT_PIDFILE=""

  if [[ "${cli_rc}" -ne 0 ]]; then
    warn " cliente terminou com rc=${cli_rc}; veja ${cond_dir}/cliente.log"
    grep -E "FALHOU|SEM RESPOSTA" "${cond_dir}/cliente.log" | head -5 | sed 's/^/   /' >&2
  fi

  # --- derruba os servidores para que gravem os resumos ---
  stop_server

  # --- traz os resultados do cliente ---
  #
  # A medição desta condição já acabou; sem o netem a cópia não sofre a perda
  # que acabou de ser medida. A condição seguinte aplica o seu de novo.
  clear_netem
  log " coletando resultados do cliente..."
  local copied=0
  for attempt in 1 2 3; do
    if scp -q -r "${CLIENT_SSH}:${CLIENT_DIR}/${remote_out}" "${cli_out}/" 2>/dev/null; then
      copied=1; break
    fi
    sleep 5
  done
  [[ "${copied}" -eq 1 ]] || warn " não foi possível copiar os resultados do cliente."
  # O log íntegro, direto da VM cliente, substitui o reconstruído pelas sondagens.
  scp -q "${CLIENT_SSH}:${CLIENT_DIR}/${remote_ctl}.log" "${cond_dir}/cliente.log" 2>/dev/null || true

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
