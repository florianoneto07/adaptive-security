#!/usr/bin/env bash
#
# Compila e instala o libcoap com CoAP + OSCORE apoiados no wolfSSL.
#
# Uso: ./scripts/setup_libcoap.sh [PREFIX]
#
#   ./scripts/setup_libcoap.sh                 # /usr/local, usa sudo
#   ./scripts/setup_libcoap.sh "$HOME/.local"  # prefixo do usuário, sem sudo
#
# Requer o wolfSSL já instalado (scripts/setup_wolfssl.sh). Por padrão procura
# o wolfSSL no mesmo PREFIX; use WOLFSSL_DIR para apontar outro lugar.
#
# Usar --with-wolfssl mantém DTLS e OSCORE sobre a mesma pilha criptográfica
# que o resto do protótipo, em vez de arrastar um segundo backend TLS.
set -euo pipefail

PREFIX="${1:-/usr/local}"
WOLFSSL_DIR="${WOLFSSL_DIR:-${PREFIX}}"
SRC_DIR="${LIBCOAP_SRC:-${HOME}/libcoap}"
LIBCOAP_REPO="https://github.com/obgm/libcoap.git"

if [[ -w "$(dirname "${PREFIX}")" || -w "${PREFIX}" ]]; then
  SUDO=""
else
  SUDO="sudo"
fi

for tool in git gcc make autoconf automake libtoolize pkg-config; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Faltando: ${tool}" >&2
    echo "  sudo apt update && sudo apt install -y build-essential autoconf automake libtool pkg-config git" >&2
    exit 1
  fi
done

# O configure do libcoap localiza o wolfSSL por pkg-config.
export PKG_CONFIG_PATH="${WOLFSSL_DIR}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
if ! pkg-config --exists wolfssl; then
  echo "wolfSSL não encontrado em ${WOLFSSL_DIR}." >&2
  echo "Rode antes: ./scripts/setup_wolfssl.sh ${WOLFSSL_DIR}" >&2
  exit 1
fi
echo "==> wolfSSL $(pkg-config --modversion wolfssl) encontrado em ${WOLFSSL_DIR}"

if [[ -d "${SRC_DIR}/.git" ]]; then
  echo "==> Reutilizando ${SRC_DIR}"
  git -C "${SRC_DIR}" fetch --depth 1 origin
  git -C "${SRC_DIR}" reset --hard origin/HEAD
else
  echo "==> Clonando o libcoap em ${SRC_DIR}"
  git clone --depth 1 "${LIBCOAP_REPO}" "${SRC_DIR}"
fi

cd "${SRC_DIR}"

echo "==> autogen"
./autogen.sh

# --disable-documentation evita exigir asciidoc/doxygen só para gerar manpages.
# Os exemplos ficam habilitados de propósito: coap-client e coap-server são as
# ferramentas usadas para validar o OSCORE antes de escrever código próprio.
echo "==> configure (prefixo: ${PREFIX})"
./configure \
  --prefix="${PREFIX}" \
  --with-wolfssl \
  --enable-oscore \
  --enable-examples \
  --disable-documentation

echo "==> build"
make -j"$(nproc)"

echo "==> install"
${SUDO} make install
if [[ "${PREFIX}" == "/usr/local" ]]; then
  ${SUDO} ldconfig
fi

echo
echo "libcoap instalado em ${PREFIX}"
PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH}" \
  pkg-config --modversion libcoap-3 2>/dev/null | sed 's/^/versão: /' || true
echo "binários de exemplo:"
ls "${PREFIX}/bin" | grep -E "^coap-" | sed 's/^/  /' || true
