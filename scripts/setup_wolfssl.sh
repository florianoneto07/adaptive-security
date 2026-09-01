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

# libtoolize, não "libtool": o autogen.sh do wolfSSL usa o primeiro, e o
# wrapper libtool vem noutro pacote (libtool-bin) que não é necessário.
for tool in git gcc make autoconf automake libtoolize; do
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

# Verificação de identidade do peer (obrigatório para os clientes subirem):
#   --enable-opensslextra   expõe X509_VERIFY_PARAM_set1_ip_asc()
#   -DWOLFSSL_IP_ALT_NAME   faz comparar subjectAltName do tipo iPAddress
#
# Exigido pelo libcoap/OSCORE:
#   --enable-aesccm   AES-CCM-16-64-128 é o AEAD obrigatório do OSCORE
#   --enable-psk      o backend wolfSSL do libcoap usa as callbacks de PSK
#   --enable-dtlscid  precisa da opção real, não do macro solto: o libcoap
#                     compara COAP_DTLS_CID_LENGTH com DTLS_CID_MAX_SIZE, que
#                     só é definido por esta opção, e falha o build sem ela
echo "==> configure (prefixo: ${PREFIX})"
./configure \
  --prefix="${PREFIX}" \
  --enable-opensslextra \
  --enable-tls13 \
  --enable-dtls \
  --enable-dtls13 \
  --enable-dtlscid \
  --enable-aesccm \
  --enable-psk \
  --enable-alpn \
  --enable-sni \
  --enable-keylog-export \
  C_EXTRA_FLAGS="-DWOLFSSL_IP_ALT_NAME"

echo "==> build"
make -j"$(nproc)"

echo "==> install"
${SUDO} make install
if [[ "${PREFIX}" == "/usr/local" ]]; then
  ${SUDO} ldconfig
fi

echo
echo "wolfSSL instalado em ${PREFIX}"
grep -E "define (WOLFSSL_DTLS13|WOLFSSL_IP_ALT_NAME|OPENSSL_EXTRA|HAVE_AESCCM|WOLFSSL_DTLS_CID)$" \
  "${PREFIX}/include/wolfssl/options.h" || true

if [[ "${PREFIX}" != "/usr/local" ]]; then
  echo
  echo "Compile o protótipo com:"
  echo "  make WOLFSSL_DIR=${PREFIX}"
  echo "E exporte antes de executar:"
  echo "  export LD_LIBRARY_PATH=${PREFIX}/lib:\${LD_LIBRARY_PATH:-}"
fi
