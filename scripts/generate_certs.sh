#!/usr/bin/env bash
#
# Gera uma CA de teste e um certificado de servidor para o protótipo.
#
# Uso: ./scripts/generate_certs.sh [IP_DO_SERVIDOR] [SAN_EXTRA...]
#
#   ./scripts/generate_certs.sh
#   ./scripts/generate_certs.sh 10.0.0.5
#   ./scripts/generate_certs.sh 192.168.237.128 127.0.0.1 localhost
#
# Cada SAN extra é classificado automaticamente como IP ou DNS. Os clientes
# exigem que o endereço usado na linha de comando apareça no subjectAltName,
# portanto todo endereço pelo qual o servidor for alcançado precisa estar aqui.
#
# ATENÇÃO: material puramente de laboratório. A pasta certs/ é ignorada pelo
# git; nunca versione chaves privadas.
set -euo pipefail

SERVER_IP="${1:-192.168.237.128}"
shift || true
EXTRA_SANS=("$@")

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Mesma variável usada pelos binários, para que testes possam isolar os
# certificados num diretório temporário.
CERT_DIR="${CERT_DIR:-${REPO_ROOT}/certs}"

mkdir -p "${CERT_DIR}"

# Classifica um SAN como "IP:x" ou "DNS:x" conforme seja um literal IPv4/IPv6.
san_entry() {
  local value="$1"
  if [[ "${value}" =~ ^[0-9]+(\.[0-9]+){3}$ || "${value}" == *:* ]]; then
    printf 'IP:%s' "${value}"
  else
    printf 'DNS:%s' "${value}"
  fi
}

SAN_LIST="$(san_entry "${SERVER_IP}")"
for san in ${EXTRA_SANS+"${EXTRA_SANS[@]}"}; do
  SAN_LIST="${SAN_LIST},$(san_entry "${san}")"
done

# --- CA de teste -------------------------------------------------------------
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout "${CERT_DIR}/ca.key" -out "${CERT_DIR}/ca.crt" \
  -subj "/CN=Adaptive Security Test CA" \
  -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
  -addext "keyUsage=critical,keyCertSign,cRLSign"

# --- Certificado do servidor -------------------------------------------------
openssl req -newkey rsa:2048 -nodes \
  -keyout "${CERT_DIR}/server.key" -out "${CERT_DIR}/server.csr" \
  -subj "/CN=${SERVER_IP}"

cat > "${CERT_DIR}/server.ext" <<EOT
subjectAltName=${SAN_LIST}
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
EOT

openssl x509 -req -in "${CERT_DIR}/server.csr" \
  -CA "${CERT_DIR}/ca.crt" -CAkey "${CERT_DIR}/ca.key" -CAcreateserial \
  -out "${CERT_DIR}/server.crt" -days 825 -sha256 \
  -extfile "${CERT_DIR}/server.ext"

rm -f "${CERT_DIR}/server.csr" "${CERT_DIR}/server.ext"
chmod 600 "${CERT_DIR}/ca.key" "${CERT_DIR}/server.key"

printf 'Certificados gerados em %s\n' "${CERT_DIR}"
printf 'subjectAltName: %s\n' "${SAN_LIST}"
