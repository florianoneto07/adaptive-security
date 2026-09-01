#!/usr/bin/env bash
#
# Teste E2E em loopback: sobe servidor e cliente na mesma máquina e confere
# tanto os caminhos felizes quanto os casos que DEVEM falhar.
#
# Uso: ./scripts/run_local_test.sh
#
# Os testes negativos são a parte que importa: um certificado assinado pela CA
# de confiança, mas emitido para outro endereço, precisa ser rejeitado. Se ele
# passar, a validação de identidade do peer não está funcionando.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

TLS_PORT="${TLS_PORT:-14433}"
DTLS_PORT="${DTLS_PORT:-14444}"
WORK_DIR="$(mktemp -d)"

PASS=0
FAIL=0

cleanup() {
  # Encerra qualquer servidor que tenha sobrado e apaga as chaves temporárias.
  jobs -p | xargs -r kill 2>/dev/null
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

report() {
  local status="$1" name="$2"
  if [[ "${status}" == "pass" ]]; then
    printf '  \033[32mPASS\033[0m  %s\n' "${name}"
    PASS=$((PASS + 1))
  else
    printf '  \033[31mFAIL\033[0m  %s\n' "${name}"
    FAIL=$((FAIL + 1))
  fi
}

# Executa um par servidor/cliente e devolve o código de saída do cliente.
# $1 = binário do servidor, $2 = binário do cliente, $3 = porta, $4 = CERT_DIR
run_pair() {
  local server_bin="$1" client_bin="$2" port="$3" cert_dir="$4"
  local server_log="${WORK_DIR}/server.log" client_log="${WORK_DIR}/client.log"
  local server_pid rc

  CERT_DIR="${cert_dir}" "./build/${server_bin}" "${port}" >"${server_log}" 2>&1 &
  server_pid=$!

  # Espera o servidor abrir a porta antes de disparar o cliente.
  for _ in $(seq 1 50); do
    if grep -q "aguardando" "${server_log}" 2>/dev/null; then break; fi
    sleep 0.1
  done

  CERT_DIR="${cert_dir}" "./build/${client_bin}" 127.0.0.1 "${port}" >"${client_log}" 2>&1
  rc=$?

  # O servidor é single-shot, mas num teste negativo pode ficar preso à espera
  # de um handshake que nunca fecha; encerra sem depender disso.
  kill "${server_pid}" 2>/dev/null
  wait "${server_pid}" 2>/dev/null
  LAST_SERVER_LOG="${server_log}"
  LAST_CLIENT_LOG="${client_log}"
  return "${rc}"
}

echo "== Preparando =="

# Certificado correto: válido para 127.0.0.1.
CERT_DIR="${WORK_DIR}/valid" ./scripts/generate_certs.sh 127.0.0.1 localhost >/dev/null 2>&1
# Certificado impostor: emitido para 198.51.100.7 (TEST-NET-3) por uma CA em
# que o cliente confia. Cadeia válida, identidade errada — exatamente o caso
# que a validação de cadeia sozinha deixa passar.
CERT_DIR="${WORK_DIR}/wrong" ./scripts/generate_certs.sh 198.51.100.7 >/dev/null 2>&1

echo "Certificados temporários em ${WORK_DIR}"

if [[ ! -x ./build/tls_server ]]; then
  echo "Binários ausentes. Rode 'make' antes." >&2
  exit 1
fi

echo
echo "== Caminho feliz =="

if run_pair tls_server tls_client "${TLS_PORT}" "${WORK_DIR}/valid" &&
   grep -q "Handshake TLS concluído" "${LAST_CLIENT_LOG}"; then
  report pass "TLS 1.3 handshake + troca de mensagens"
  grep -E "^(Versão|Cipher):" "${LAST_CLIENT_LOG}" | sed 's/^/        /'
else
  report fail "TLS 1.3 handshake + troca de mensagens"
  sed 's/^/        /' "${LAST_CLIENT_LOG}"
fi

if run_pair dtls_server dtls_client "${DTLS_PORT}" "${WORK_DIR}/valid" &&
   grep -q "Handshake DTLS concluído" "${LAST_CLIENT_LOG}"; then
  report pass "DTLS 1.3 handshake + troca de mensagens"
  grep -E "^(Versão|Cipher):" "${LAST_CLIENT_LOG}" | sed 's/^/        /'
else
  report fail "DTLS 1.3 handshake + troca de mensagens"
  sed 's/^/        /' "${LAST_CLIENT_LOG}"
fi

echo
echo "== Identidade do peer (devem ser REJEITADOS) =="

# Servidor usa o certificado de 198.51.100.7; cliente conecta em 127.0.0.1 e
# confia na CA que o assinou. Só a checagem de identidade pode barrar isso.
mkdir -p "${WORK_DIR}/impostor"
cp "${WORK_DIR}/wrong/server.crt" "${WORK_DIR}/wrong/server.key" "${WORK_DIR}/impostor/"
cp "${WORK_DIR}/wrong/ca.crt" "${WORK_DIR}/impostor/ca.crt"

if run_pair tls_server tls_client "${TLS_PORT}" "${WORK_DIR}/impostor"; then
  report fail "TLS rejeita certificado emitido para outro endereço"
  sed 's/^/        /' "${LAST_CLIENT_LOG}"
else
  report pass "TLS rejeita certificado emitido para outro endereço"
fi

if run_pair dtls_server dtls_client "${DTLS_PORT}" "${WORK_DIR}/impostor"; then
  report fail "DTLS rejeita certificado emitido para outro endereço"
  sed 's/^/        /' "${LAST_CLIENT_LOG}"
else
  report pass "DTLS rejeita certificado emitido para outro endereço"
fi

echo
echo "== Resultado =="
printf '  %d passaram, %d falharam\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
