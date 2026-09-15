#!/usr/bin/env bash
#
# Emulação de enlace aéreo com tc qdisc netem (requisito R5).
#
# Uso:
#   ./netem.sh status [IFACE]        mostra o qdisc atual
#   ./netem.sh off [IFACE]           remove a emulação
#   ./netem.sh 3gpp-c2 [IFACE]       aplica o perfil nomeado
#   ./netem.sh handover [IFACE]
#   ./netem.sh custom "delay 30ms 5ms loss 1%" [IFACE]
#
# Sem IFACE, usa a interface da rota padrão.
#
# ATENÇÃO — o netem age só na SAÍDA da interface. Duas consequências:
#
#   1. Para atraso simétrico, aplique nas DUAS VMs. Aplicado só numa, o pacote
#      atrasa num sentido e volta sem atraso, e o RTT medido é metade do que a
#      leitura ingênua sugere.
#   2. Tráfego de loopback (127.0.0.1) não passa pela interface física. Testes
#      em loopback ignoram o netem silenciosamente — use as duas VMs.
#
# Exige CAP_NET_ADMIN. Ver README para o setcap que dispensa sudo.
set -uo pipefail

# ---------------------------------------------------------------------------
# Perfis
# ---------------------------------------------------------------------------
#
# 3gpp-c2: requisito de comando e controle de UAV lido como latência de plano
#   de usuário UNIDIRECIONAL de 50 ms e taxa de erro de pacote de 10^-3
#   [Zeng et al., arXiv 1903.05289, Tab. I; Fotouhi et al., arXiv 2005.00781,
#   Tab. 1]. Aplicado nas duas VMs, dá 50 ms em cada sentido.
#
# handover: troca de célula ou perda de linha de visada. A perda de 20% com
#   correlação de 25% aproxima o comportamento em RAJADA — o netem sem
#   correlação sortearia cada pacote de forma independente, o que distribui a
#   perda de modo uniforme e é justamente o que NÃO acontece num handover.
#   O valor de correlação é padrão do testbed, não vem da literatura.
#
# A linha de base é rodar sem netem nenhum ("off"): sem ela não há como
# atribuir uma degradação ao enlace em vez de ao perfil de segurança.
#
# Varreduras (P1), para as curvas do paper. Perfis paramétricos que variam UM
# eixo de cada vez, ancorados nos valores do 3gpp-c2:
#
#   loss-<X>   delay 50ms, perda X%  (X pode ter casa decimal, ex.: loss-0.1)
#   delay-<Y>  delay Yms, perda 0,1%  (Y inteiro, ex.: delay-100)
#
# A perda da varredura é INDEPENDENTE (sem correlação), diferente do handover,
# que usa rajada de propósito: aqui cada ponto deve diferir SÓ na taxa, para a
# curva ser limpa. A correlação em rajada é um segundo eixo, fora do escopo da
# varredura. Aplicar nas DUAS VMs, como os demais perfis.

profile_args() {
  local p="$1"

  case "${p}" in
    3gpp-c2)  printf 'delay 50ms loss 0.1%%'; return 0 ;;
    handover) printf 'delay 200ms 20ms loss 20%% 25%%'; return 0 ;;
  esac

  # Varredura de perda: delay fixo de 50 ms, taxa variável (aceita decimal).
  if [[ "${p}" =~ ^loss-([0-9]+(\.[0-9]+)?)$ ]]; then
    printf 'delay 50ms loss %s%%' "${BASH_REMATCH[1]}"
    return 0
  fi
  # Varredura de atraso: perda fixa de 0,1%, atraso variável (ms inteiros).
  if [[ "${p}" =~ ^delay-([0-9]+)$ ]]; then
    printf 'delay %sms loss 0.1%%' "${BASH_REMATCH[1]}"
    return 0
  fi

  return 1
}

default_iface() {
  ip route show default 2>/dev/null | awk '/default/ {print $5; exit}'
}

usage() {
  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
  echo
  echo "Perfis disponíveis:"
  echo "  3gpp-c2   $(profile_args 3gpp-c2)"
  echo "  handover  $(profile_args handover)"
  echo "  loss-<X>  varredura de perda: $(profile_args loss-5) (ex.: loss-5)"
  echo "  delay-<Y> varredura de atraso: $(profile_args delay-100) (ex.: delay-100)"
  echo "  off       remove qualquer emulação"
}

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

ACTION="$1"; shift

if [[ "${ACTION}" == "custom" ]]; then
  if [[ $# -lt 1 ]]; then
    echo "custom exige os parâmetros do netem entre aspas." >&2
    echo "  ./netem.sh custom \"delay 30ms 5ms loss 1%\"" >&2
    exit 1
  fi
  NETEM_ARGS="$1"; shift
fi

IFACE="${1:-$(default_iface)}"

if [[ -z "${IFACE}" ]]; then
  echo "Não foi possível descobrir a interface de saída. Informe-a:" >&2
  echo "  ./netem.sh ${ACTION} eth0" >&2
  exit 1
fi
if [[ ! -d "/sys/class/net/${IFACE}" ]]; then
  echo "Interface inexistente: ${IFACE}" >&2
  exit 1
fi

# tc precisa de CAP_NET_ADMIN. Aviso cedo, com a saída exata para corrigir, em
# vez de deixar o comando falhar com "Operation not permitted" no meio.
run_tc() {
  local err rc

  # Uma execução só: repetir o comando para inspecionar a mensagem aplicaria a
  # regra duas vezes quando ela funciona.
  err="$(tc "$@" 2>&1)"
  rc=$?

  if [[ ${rc} -eq 0 ]]; then
    return 0
  fi
  if grep -qi "not permitted" <<<"${err}"; then
    echo "tc precisa de CAP_NET_ADMIN. Uma destas resolve:" >&2
    echo "  sudo setcap cap_net_admin+eip $(command -v tc)   (uma vez, permanente)" >&2
    echo "  sudo ./netem.sh ${ACTION} ${IFACE}               (a cada execução)" >&2
  else
    echo "${err}" >&2
  fi
  return 1
}

case "${ACTION}" in
  status)
    echo "Interface: ${IFACE}"
    tc qdisc show dev "${IFACE}"
    ;;

  off)
    # "|| true": remover um qdisc que não existe devolve erro, e isso não é
    # falha — deixar o script parar aí atrapalharia qualquer limpeza no final
    # de uma execução.
    tc qdisc del dev "${IFACE}" root 2>/dev/null || true
    echo "Emulação removida de ${IFACE}."
    tc qdisc show dev "${IFACE}"
    ;;

  custom)
    tc qdisc del dev "${IFACE}" root 2>/dev/null || true
    # shellcheck disable=SC2086
    if run_tc qdisc add dev "${IFACE}" root netem ${NETEM_ARGS}; then
      echo "Aplicado em ${IFACE}: netem ${NETEM_ARGS}"
      tc qdisc show dev "${IFACE}"
    else
      exit 1
    fi
    ;;

  *)
    if ! ARGS="$(profile_args "${ACTION}")"; then
      echo "Perfil desconhecido: ${ACTION}" >&2
      echo >&2
      usage >&2
      exit 1
    fi
    tc qdisc del dev "${IFACE}" root 2>/dev/null || true
    # shellcheck disable=SC2086
    if run_tc qdisc add dev "${IFACE}" root netem ${ARGS}; then
      echo "Perfil '${ACTION}' aplicado em ${IFACE}: netem ${ARGS}"
      echo
      echo "Lembre-se: o netem age só na saída. Para atraso simétrico, aplique"
      echo "o mesmo perfil na outra VM."
      tc qdisc show dev "${IFACE}"
    else
      exit 1
    fi
    ;;
esac
