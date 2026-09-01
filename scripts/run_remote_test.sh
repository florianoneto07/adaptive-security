#!/usr/bin/env bash
#
# Teste E2E entre as duas VMs, disparado a partir do servidor.
#
# Uso: ./scripts/run_remote_test.sh <HOST_SSH_DO_CLIENTE> <IP_DO_SERVIDOR>
#
#   ./scripts/run_remote_test.sh lab-client 10.0.0.5
#
# HOST_SSH_DO_CLIENTE é um host alcançável por SSH sem senha (um alias do
# ~/.ssh/config ou usuario@endereco). IP_DO_SERVIDOR é o endereço desta
# máquina como o cliente a enxerga, e precisa constar do subjectAltName do
# certificado.
#
# Exige SSH sem senha para o cliente e o repositório já compilado dos dois
# lados. Os casos negativos usam um certificado de cadeia válida emitido para
# outro endereço: ele precisa ser recusado.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

# Sem defaults de propósito: valores de laboratório embutidos num repositório
# público envelhecem mal e vazam topologia.
if [[ $# -lt 2 ]]; then
  sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \?//' >&2
  exit 1
fi
CLIENT_HOST="$1"
SERVER_IP="$2"
TLS_PORT="${TLS_PORT:-4433}"
DTLS_PORT="${DTLS_PORT:-4444}"
REMOTE_REPO="${REMOTE_REPO:-\$HOME/adaptive-security}"
REMOTE_LIB="${REMOTE_LIB:-\$HOME/.local/lib}"

WORK_DIR="$(mktemp -d)"
PASS=0
FAIL=0

cleanup() {
  jobs -p | xargs -r kill 2>/dev/null
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

report() {
  if [[ "$1" == "pass" ]]; then
    printf '  \033[32mPASS\033[0m  %s\n' "$2"; PASS=$((PASS + 1))
  else
    printf '  \033[31mFAIL\033[0m  %s\n' "$2"; FAIL=$((FAIL + 1))
  fi
}

# Sobe o servidor local, roda o cliente remoto, devolve o rc do cliente.
# rc 99 = o servidor não chegou a escutar, então o resultado não diz nada.
# $1 = protocolo (tls|dtls), $2 = porta, $3 = CERT_DIR local, $4 = CERT_DIR remoto
run_pair() {
  local proto="$1" port="$2" local_certs="$3" remote_certs="$4"
  local server_log="${WORK_DIR}/${proto}_server.log"
  local server_pid rc

  CERT_DIR="${local_certs}" "./build/${proto}_server" "${port}" >"${server_log}" 2>&1 &
  server_pid=$!

  local up=0
  for _ in $(seq 1 50); do
    grep -q "aguardando" "${server_log}" 2>/dev/null && { up=1; break; }
    kill -0 "${server_pid}" 2>/dev/null || break
    sleep 0.1
  done

  if [[ "${up}" -eq 0 ]]; then
    kill "${server_pid}" 2>/dev/null; wait "${server_pid}" 2>/dev/null
    LAST_LOG="${server_log}"
    return 99
  fi

  ssh -o BatchMode=yes "${CLIENT_HOST}" \
    "cd ${REMOTE_REPO} && CERT_DIR=${remote_certs} LD_LIBRARY_PATH=${REMOTE_LIB} ./build/${proto}_client ${SERVER_IP} ${port}" \
    >"${WORK_DIR}/${proto}_client.log" 2>&1
  rc=$?

  kill "${server_pid}" 2>/dev/null
  wait "${server_pid}" 2>/dev/null
  LAST_LOG="${WORK_DIR}/${proto}_client.log"
  return "${rc}"
}

echo "== Preparando =="
echo "servidor ${SERVER_IP}, cliente via SSH '${CLIENT_HOST}'"

if ! ssh -o BatchMode=yes -o ConnectTimeout=8 "${CLIENT_HOST}" true 2>/dev/null; then
  echo "SSH sem senha para '${CLIENT_HOST}' não está funcionando." >&2
  exit 1
fi
if [[ ! -x ./build/tls_server ]]; then
  echo "Binários ausentes. Rode 'make' antes." >&2
  exit 1
fi

# Certificado legítimo, válido para o IP do servidor.
CERT_DIR="${WORK_DIR}/valid" ./scripts/generate_certs.sh "${SERVER_IP}" >/dev/null 2>&1
ssh -o BatchMode=yes "${CLIENT_HOST}" "mkdir -p \$HOME/.cache/as_test/valid"
scp -q "${WORK_DIR}/valid/ca.crt" "${CLIENT_HOST}:~/.cache/as_test/valid/ca.crt"

# Certificado de cadeia válida, emitido para outro endereço.
CERT_DIR="${WORK_DIR}/wrong" ./scripts/generate_certs.sh 198.51.100.7 >/dev/null 2>&1
ssh -o BatchMode=yes "${CLIENT_HOST}" "mkdir -p \$HOME/.cache/as_test/wrong"
scp -q "${WORK_DIR}/wrong/ca.crt" "${CLIENT_HOST}:~/.cache/as_test/wrong/ca.crt"

echo
echo "== Caminho feliz =="
for proto in tls dtls; do
  port=$([[ "${proto}" == tls ]] && echo "${TLS_PORT}" || echo "${DTLS_PORT}")
  if run_pair "${proto}" "${port}" "${WORK_DIR}/valid" "\$HOME/.cache/as_test/valid" &&
     grep -q "Handshake ${proto^^} concluído" "${LAST_LOG}"; then
    report pass "${proto^^} 1.3 entre as VMs"
    grep -E "^(Versão|Cipher):" "${LAST_LOG}" | sed 's/^/        /'
  else
    report fail "${proto^^} 1.3 entre as VMs"
    sed 's/^/        /' "${LAST_LOG}"
  fi
done

echo
echo "== Identidade do peer (devem ser REJEITADOS) =="
for proto in tls dtls; do
  port=$([[ "${proto}" == tls ]] && echo "${TLS_PORT}" || echo "${DTLS_PORT}")
  run_pair "${proto}" "${port}" "${WORK_DIR}/wrong" "\$HOME/.cache/as_test/wrong"
  rc=$?
  name="${proto^^} rejeita certificado emitido para outro endereço"
  if [[ "${rc}" -eq 99 ]]; then
    report fail "${name} (servidor não subiu)"
    sed 's/^/        /' "${LAST_LOG}"
  elif [[ "${rc}" -eq 0 ]]; then
    report fail "${name} (certificado ACEITO indevidamente)"
    sed 's/^/        /' "${LAST_LOG}"
  else
    report pass "${name}"
    grep -m1 "Falha no handshake" "${LAST_LOG}" | sed 's/^/        /'
  fi
done

ssh -o BatchMode=yes "${CLIENT_HOST}" "rm -rf \$HOME/.cache/as_test"

echo
echo "== Resultado =="
printf '  %d passaram, %d falharam\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
