#!/usr/bin/env bash
#
# Gera uma CA de teste e um certificado de servidor para o protótipo.
# Uso: ./scripts/generate_certs.sh [IP_DO_SERVIDOR]
#
# ATENÇÃO: material puramente de laboratório. A pasta certs/ é ignorada pelo
# git; nunca versione chaves privadas.
set -euo pipefail

SERVER_IP="${1:-192.168.237.128}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CERT_DIR="${REPO_ROOT}/certs"

mkdir -p "${CERT_DIR}"

openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout "${CERT_DIR}/ca.key" -out "${CERT_DIR}/ca.crt" \
  -subj "/CN=Adaptive Security Test CA"

openssl req -newkey rsa:2048 -nodes \
  -keyout "${CERT_DIR}/server.key" -out "${CERT_DIR}/server.csr" \
  -subj "/CN=${SERVER_IP}"

cat > "${CERT_DIR}/server.ext" <<EOT
subjectAltName=IP:${SERVER_IP}
extendedKeyUsage=serverAuth
EOT

openssl x509 -req -in "${CERT_DIR}/server.csr" \
  -CA "${CERT_DIR}/ca.crt" -CAkey "${CERT_DIR}/ca.key" -CAcreateserial \
  -out "${CERT_DIR}/server.crt" -days 825 -sha256 \
  -extfile "${CERT_DIR}/server.ext"

rm -f "${CERT_DIR}/server.csr" "${CERT_DIR}/server.ext"
chmod 600 "${CERT_DIR}/ca.key" "${CERT_DIR}/server.key"

printf 'Certificados gerados em %s (SAN IP: %s)\n' "${CERT_DIR}" "${SERVER_IP}"
