#!/usr/bin/env bash
#
# Compila e instala o wolfSSL com as opções exigidas por este protótipo.
#
# Uso: ./scripts/setup_wolfssl.sh [PREFIX]
#
#   ./scripts/setup_wolfssl.sh                 # /usr/local, usa sudo
#   ./scripts/setup_wolfssl.sh "$HOME/.local"  # prefixo do usuário, sem sudo
#
# Com prefixo de usuário, compile o protótipo apontando para ele:
#   make WOLFSSL_DIR="$HOME/.local"
set -euo pipefail

PREFIX="${1:-/usr/local}"
SRC_DIR="${WOLFSSL_SRC:-${HOME}/wolfssl}"
WOLFSSL_REPO="https://github.com/wolfSSL/wolfssl.git"

# Só usa sudo quando o destino não pertence ao usuário.
if [[ -w "$(dirname "${PREFIX}")" || -w "${PREFIX}" ]]; then
  SUDO=""
else
  SUDO="sudo"
fi

for tool in git gcc make autoconf automake libtool; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Faltando: ${tool}" >&2
    echo "Instale as dependências antes:" >&2
    echo "  sudo apt update && sudo apt install -y build-essential autoconf automake libtool pkg-config git" >&2
    exit 1
  fi
done

if [[ -d "${SRC_DIR}/.git" ]]; then
  echo "==> Reutilizando ${SRC_DIR}"
  git -C "${SRC_DIR}" fetch --depth 1 origin
  git -C "${SRC_DIR}" reset --hard origin/HEAD
else
  echo "==> Clonando o wolfSSL em ${SRC_DIR}"
  git clone --depth 1 "${WOLFSSL_REPO}" "${SRC_DIR}"
fi

cd "${SRC_DIR}"

echo "==> autogen"
./autogen.sh

# As duas primeiras não são opcionais: --enable-opensslextra expõe
# X509_VERIFY_PARAM_set1_ip_asc() e WOLFSSL_IP_ALT_NAME faz o wolfSSL comparar
# subjectAltName do tipo iPAddress. Sem elas a verificação de identidade do
# peer não funciona e os clientes se recusam a subir.
echo "==> configure (prefixo: ${PREFIX})"
./configure \
  --prefix="${PREFIX}" \
  --enable-opensslextra \
  --enable-tls13 \
  --enable-dtls \
  --enable-dtls13 \
  --enable-sni \
  --enable-keylog-export \
  C_EXTRA_FLAGS="-DWOLFSSL_IP_ALT_NAME -DWOLFSSL_DTLS_CID"

echo "==> build"
make -j"$(nproc)"

echo "==> install"
${SUDO} make install
if [[ "${PREFIX}" == "/usr/local" ]]; then
  ${SUDO} ldconfig
fi

echo
echo "wolfSSL instalado em ${PREFIX}"
grep -E "define (WOLFSSL_DTLS13|WOLFSSL_IP_ALT_NAME|OPENSSL_EXTRA)$" \
  "${PREFIX}/include/wolfssl/options.h" || true

if [[ "${PREFIX}" != "/usr/local" ]]; then
  echo
  echo "Compile o protótipo com:"
  echo "  make WOLFSSL_DIR=${PREFIX}"
  echo "E exporte antes de executar:"
  echo "  export LD_LIBRARY_PATH=${PREFIX}/lib:\${LD_LIBRARY_PATH:-}"
fi
