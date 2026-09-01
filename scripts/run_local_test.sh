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
# $1 = servidor, $2 = cliente, $3 = porta, $4 = CERT_DIR, $5 = host (opt)
#
# Define SERVER_UP=1/0. Distinguir "o cliente foi rejeitado" de "o servidor
# nunca subiu" é essencial: sem isso um servidor quebrado faz os testes
# negativos passarem pelo motivo errado, que foi exatamente o que aconteceu na
# primeira execução desta suíte.
run_pair() {
  local server_bin="$1" client_bin="$2" port="$3" cert_dir="$4"
  local host="${5:-127.0.0.1}"
  local server_log="${WORK_DIR}/server.log" client_log="${WORK_DIR}/client.log"
  local server_pid rc

  SERVER_UP=0
  CERT_DIR="${cert_dir}" "./build/${server_bin}" "${port}" >"${server_log}" 2>&1 &
  server_pid=$!

  # Espera o servidor anunciar que está escutando, ou desistir se ele morrer.
  for _ in $(seq 1 50); do
    if grep -q "aguardando" "${server_log}" 2>/dev/null; then
      SERVER_UP=1
      break
    fi
    if ! kill -0 "${server_pid}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done

  if [[ "${SERVER_UP}" -eq 0 ]]; then
    kill "${server_pid}" 2>/dev/null
    wait "${server_pid}" 2>/dev/null
    LAST_SERVER_LOG="${server_log}"
    LAST_CLIENT_LOG="${server_log}"
    return 99
  fi

  CERT_DIR="${cert_dir}" "./build/${client_bin}" "${host}" "${port}" >"${client_log}" 2>&1
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
# Certificado válido para o IP de loopback, mas sem SAN dNSName: conectar por
# "localhost" precisa ser recusado.
CERT_DIR="${WORK_DIR}/iponly" ./scripts/generate_certs.sh 127.0.0.1 >/dev/null 2>&1

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

if run_pair tls_server tls_client "${TLS_PORT}" "${WORK_DIR}/valid" localhost &&
   grep -q "Handshake TLS concluído" "${LAST_CLIENT_LOG}"; then
  report pass "TLS por hostname (SAN dNSName)"
else
  report fail "TLS por hostname (SAN dNSName)"
  sed 's/^/        /' "${LAST_CLIENT_LOG}"
fi

echo
echo "== Identidade do peer (devem ser REJEITADOS) =="

# Servidor usa o certificado de 198.51.100.7; cliente conecta em 127.0.0.1 e
# confia na CA que o assinou. Só a checagem de identidade pode barrar isso.
mkdir -p "${WORK_DIR}/impostor"
cp "${WORK_DIR}/wrong/server.crt" "${WORK_DIR}/wrong/server.key" "${WORK_DIR}/impostor/"
cp "${WORK_DIR}/wrong/ca.crt" "${WORK_DIR}/impostor/ca.crt"

check_rejected() {
  local name="$1"
  # rc 99 = servidor não subiu: a rejeição não prova nada.
  if [[ "$2" -eq 99 ]]; then
    report fail "${name} (servidor não subiu)"
    sed 's/^/        /' "${LAST_SERVER_LOG}"
  elif [[ "$2" -eq 0 ]]; then
    report fail "${name} (certificado ACEITO indevidamente)"
    sed 's/^/        /' "${LAST_CLIENT_LOG}"
  else
    report pass "${name}"
    grep -m1 "Falha no handshake" "${LAST_CLIENT_LOG}" | sed 's/^/        /'
  fi
}

run_pair tls_server tls_client "${TLS_PORT}" "${WORK_DIR}/impostor"
check_rejected "TLS rejeita certificado emitido para outro endereço" $?

run_pair dtls_server dtls_client "${DTLS_PORT}" "${WORK_DIR}/impostor"
check_rejected "DTLS rejeita certificado emitido para outro endereço" $?

run_pair tls_server tls_client "${TLS_PORT}" "${WORK_DIR}/iponly" localhost
check_rejected "TLS rejeita hostname ausente do subjectAltName" $?

echo
echo "== Resultado =="
printf '  %d passaram, %d falharam\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
