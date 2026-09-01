#!/usr/bin/env bash
#
# Teste E2E de OSCORE (RFC 8613) em loopback, usando o coap-server e o
# coap-client do libcoap.
#
# Uso: ./scripts/run_oscore_test.sh
#
# Exige libcoap com OSCORE instalado (scripts/setup_libcoap.sh).
#
# O caso negativo é o que importa: um cliente com master_secret diferente
# precisa ser recusado na decifragem. Sem ele, o caminho feliz sozinho não
# distingue "OSCORE protegendo" de "CoAP em claro passando".
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

PREFIX="${PREFIX:-$HOME/.local}"
COAP_SERVER="${PREFIX}/bin/coap-server"
COAP_CLIENT="${PREFIX}/bin/coap-client"
export LD_LIBRARY_PATH="${PREFIX}/lib:${LD_LIBRARY_PATH:-}"

PORT="${COAP_PORT:-5683}"
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

for bin in "${COAP_SERVER}" "${COAP_CLIENT}"; do
  if [[ ! -x "${bin}" ]]; then
    echo "Não encontrado: ${bin}" >&2
    echo "Rode antes: ./scripts/setup_libcoap.sh \"${PREFIX}\"" >&2
    exit 1
  fi
done

# Captura antes de filtrar: o coap-client sem argumentos sai com código de
# erro de uso, e sob pipefail isso derrubaria o pipeline mesmo com o grep
# encontrando o texto.
caps="$("${COAP_CLIENT}" 2>&1 || true)"
if ! grep -q "Have OSCORE" <<<"${caps}"; then
  echo "O libcoap instalado não tem OSCORE. Recompile com --enable-oscore." >&2
  exit 1
fi

echo "== Preparando =="
CERT_DIR=/dev/null ./scripts/generate_oscore_conf.sh "${WORK_DIR}" >/dev/null 2>&1
# Contexto com segredo divergente, para o caso negativo.
sed 's/^master_secret.*/master_secret,hex,"ffffffffffffffffffffffffffffffff"/' \
  "${WORK_DIR}/client.conf" > "${WORK_DIR}/bad.conf"
echo "contextos em ${WORK_DIR}"

"${COAP_SERVER}" -p "${PORT}" -E "${WORK_DIR}/server.conf" >"${WORK_DIR}/server.log" 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
  echo "coap-server não subiu:" >&2
  cat "${WORK_DIR}/server.log" >&2
  exit 1
fi

echo
echo "== Caminho feliz =="
out="$("${COAP_CLIENT}" -E "${WORK_DIR}/client.conf" -m get "coap://127.0.0.1:${PORT}/time" 2>&1)"
if [[ -n "${out}" ]] && ! grep -qi "fail\|error" <<<"${out}"; then
  report pass "GET protegido por OSCORE"
  printf '        resposta: %s\n' "${out}"
else
  report fail "GET protegido por OSCORE"
  printf '        %s\n' "${out}"
fi

# Confirma que a requisição realmente trafegou dentro de um envelope OSCORE, e
# não como CoAP em claro que por acaso funcionou.
verbose="$("${COAP_CLIENT}" -E "${WORK_DIR}/client.conf" -m get "coap://127.0.0.1:${PORT}/time" -v 7 2>&1)"
if grep -q "Oscore:" <<<"${verbose}"; then
  report pass "Requisição encapsulada em envelope OSCORE"
  grep -m1 "Oscore:" <<<"${verbose}" | sed 's/^/        /'
else
  report fail "Requisição encapsulada em envelope OSCORE"
fi

kill "${SERVER_PID}" 2>/dev/null
wait "${SERVER_PID}" 2>/dev/null

echo
echo "== Contexto inválido (deve ser REJEITADO) =="

# Servidor novo, numa porta própria. Reaproveitar a instância anterior faria a
# janela anti-replay barrar a requisição antes da decifragem (o contexto ruim
# reusa sender_id e números de sequência já vistos), e o teste passaria por
# 4.01 Replay detected em vez de provar a falha criptográfica.
BAD_PORT=$((PORT + 1))
"${COAP_SERVER}" -p "${BAD_PORT}" -E "${WORK_DIR}/server.conf" >"${WORK_DIR}/server_bad.log" 2>&1 &
BAD_PID=$!
sleep 2

out="$("${COAP_CLIENT}" -E "${WORK_DIR}/bad.conf" -m get "coap://127.0.0.1:${BAD_PORT}/time" 2>&1)"
if grep -qi "decryption failed" <<<"${out}"; then
  report pass "master_secret divergente é recusado"
  printf '        %s\n' "${out}"
else
  report fail "master_secret divergente é recusado (resposta: ${out})"
fi

if grep -qi "Decryption Failure" "${WORK_DIR}/server_bad.log"; then
  report pass "Servidor registra a falha de decifragem"
  grep -m1 -i "Decryption Failure" "${WORK_DIR}/server_bad.log" | sed 's/^.*WARN/        WARN/'
else
  report fail "Servidor registra a falha de decifragem"
  tail -2 "${WORK_DIR}/server_bad.log" | sed 's/^/        /'
fi

kill "${BAD_PID}" 2>/dev/null
wait "${BAD_PID}" 2>/dev/null

echo
echo "== Resultado =="
printf '  %d passaram, %d falharam\n' "${PASS}" "${FAIL}"
[[ "${FAIL}" -eq 0 ]]
