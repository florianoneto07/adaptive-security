#!/usr/bin/env bash
#
# Gera uma chave pré-compartilhada distinta para cada um dos quatro canais.
#
# Uso: ./scripts/generate_keys.sh [DIRETÓRIO_DE_SAÍDA]
#
#   ./scripts/generate_keys.sh                 # grava em keys/
#   ./scripts/generate_keys.sh /tmp/lab-keys   # noutro lugar
#
# Cada canal recebe SUA PRÓPRIA chave, sem relação com a dos outros. Isso simula
# a separação de chave por aplicação do AKMA: cada classe de aplicação
# corresponde a um Application Function com seu próprio K_AF, e uma aplicação
# não alcança a chave de outra. Derivar as quatro de um segredo comum
# destruiria justamente a propriedade que o testbed quer demonstrar.
#
# A derivação AKMA propriamente dita está fora de escopo nesta fase: as chaves
# são injetadas, não derivadas.
#
# ATENÇÃO: material de laboratório. keys/ é ignorado pelo git; nunca versione
# chave nenhuma. Leve cada arquivo à VM que precisa dele por canal seguro.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${REPO_ROOT}/keys}"

# 32 bytes: padrão do testbed, não vem da literatura. Escolhido para casar com
# a força do TLS_AES_256_GCM_SHA384 dos handshakes e do AES-256-GCM do C3.
KEY_BYTES=32

mkdir -p "${OUT_DIR}"

# slug:arquivo — precisa espelhar as_profile_table[] em common/as_profile.h.
CHANNELS=(
  "c1_control:c1.key"
  "c2_telemetry:c2.key"
  "c3_media:c3.key"
  "c4_bulk:c4.key"
)

printf 'Chaves geradas em %s\n\n' "${OUT_DIR}"
printf '%-14s %-10s %s\n' "CANAL" "ARQUIVO" "FINGERPRINT (SHA-256, 8 B)"

for entry in "${CHANNELS[@]}"; do
  slug="${entry%%:*}"
  file="${entry##*:}"
  path="${OUT_DIR}/${file}"

  openssl rand -hex "${KEY_BYTES}" > "${path}"
  chmod 600 "${path}"

  # Mesmo fingerprint que os binários imprimem (as_key_fingerprint): SHA-256 da
  # chave em BINÁRIO, truncado em 8 bytes. Confere que as duas VMs têm a mesma
  # chave, e que os quatro canais têm chaves diferentes, sem expor segredo.
  fp="$(xxd -r -p "${path}" | openssl dgst -sha256 -binary | xxd -p -l 8)"
  printf '%-14s %-10s %s\n' "${slug}" "${file}" "${fp}"
done

cat <<'NOTA'

Cada canal usa apenas a sua chave. Leve à VM cliente as que ela precisa:

  scp keys/c1.key [USUARIO]@[IP_DO_CLIENTE]:~/adaptive-security-prototype/keys/

Confira que as pontas batem comparando os fingerprints acima nas duas VMs.
Chaves diferentes se manifestam como falha de handshake genérica, não como
erro de configuração, e é o defeito mais penoso de diagnosticar nesta montagem.
NOTA
