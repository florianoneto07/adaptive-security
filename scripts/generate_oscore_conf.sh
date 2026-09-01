#!/usr/bin/env bash
#
# Gera o par de contextos de segurança OSCORE (RFC 8613) para cliente e
# servidor, no formato lido pelo libcoap (-E oscore_conf_file).
#
# Uso: ./scripts/generate_oscore_conf.sh [DIRETÓRIO_DE_SAÍDA]
#
# Produz:
#   oscore/server.conf   usado no coap-server
#   oscore/client.conf   usado no coap-client
#
# O master_secret é gerado aleatoriamente e é SEGREDO COMPARTILHADO: os dois
# arquivos precisam carregar o mesmo valor, e o do cliente tem de ser levado à
# outra VM por um canal seguro. O diretório de saída é ignorado pelo git.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${REPO_ROOT}/oscore}"

mkdir -p "${OUT_DIR}"

# RFC 8613 seção 3.1: Master Secret de 16 bytes e Master Salt de 8 bytes.
MASTER_SECRET="$(openssl rand -hex 16)"
MASTER_SALT="$(openssl rand -hex 8)"

# Sender ID de um lado é o Recipient ID do outro. Trocar os dois é o erro mais
# comum de configuração e se manifesta como falha de decifragem, não como erro
# de configuração, o que torna o diagnóstico bem mais penoso.
SERVER_ID="01"
CLIENT_ID="02"

# aead_alg 10 = AES-CCM-16-64-128, o obrigatório da RFC 8613.
# hkdf_alg -10 = direct+HKDF-SHA-256.
write_conf() {
  local path="$1" sender="$2" recipient="$3"
  cat > "${path}" <<EOT
# Contexto de segurança OSCORE (RFC 8613)
# Gerado por scripts/generate_oscore_conf.sh
master_secret,hex,"${MASTER_SECRET}"
master_salt,hex,"${MASTER_SALT}"
sender_id,hex,"${sender}"
recipient_id,hex,"${recipient}"
replay_window,integer,32
aead_alg,integer,10
hkdf_alg,integer,-10
EOT
  chmod 600 "${path}"
}

write_conf "${OUT_DIR}/server.conf" "${SERVER_ID}" "${CLIENT_ID}"
write_conf "${OUT_DIR}/client.conf" "${CLIENT_ID}" "${SERVER_ID}"

printf 'Contextos OSCORE gerados em %s\n' "${OUT_DIR}"
printf '  server.conf  sender_id=%s  recipient_id=%s\n' "${SERVER_ID}" "${CLIENT_ID}"
printf '  client.conf  sender_id=%s  recipient_id=%s\n' "${CLIENT_ID}" "${SERVER_ID}"
printf '\nLeve client.conf para a VM cliente por canal seguro:\n'
printf '  scp %s/client.conf [USUARIO]@[IP_DO_CLIENTE]:~/adaptive-security/oscore/\n' "${OUT_DIR}"
