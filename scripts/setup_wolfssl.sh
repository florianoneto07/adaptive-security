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
SRC_VAR="WOLFSSL_SRC"

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

# Um "sudo make install" anterior deixa objetos de root na árvore de build, e o
# rebuild seguinte como usuário comum falha com "Permission denied" no meio da
# compilação, longe da causa. Detecta isso antes de gastar o build inteiro.
if [[ -d "${SRC_DIR}" ]]; then
  root_files="$(find "${SRC_DIR}" ! -user "$(id -un)" -print -quit 2>/dev/null)"
  if [[ -n "${root_files}" ]]; then
    echo "A árvore ${SRC_DIR} contém arquivos de outro usuário (ex.: ${root_files})." >&2
    echo "Provavelmente sobra de um 'sudo make install' anterior." >&2
    echo "Compile numa árvore limpa:" >&2
    echo "  ${0##*/} usa a variável ${SRC_VAR}; aponte-a para um diretório novo," >&2
    echo "  por exemplo: ${SRC_VAR}=\$HOME/$(basename "${SRC_DIR}")-novo $0 $*" >&2
    exit 1
  fi
fi

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
#   --enable-dtlscid  Connection ID do DTLS 1.3 (RFC 9146), relevante quando o
#                     cliente muda de IP/porta — cenário comum em 5G
#   -DDTLS_CID_MAX_SIZE=8
#                     o wolfSSL define esse limite em internal.h, que é privado
#                     e não é instalado; o libcoap compara COAP_DTLS_CID_LENGTH
#                     contra ele e, sem enxergá-lo, o pré-processador o toma
#                     como 0 e aborta o build. Passar pelo C_EXTRA_FLAGS o
#                     grava no options.h, onde o libcoap consegue vê-lo.
#                     O valor 8 é o recomendado em src/coap_wolfssl.c
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
  C_EXTRA_FLAGS="-DWOLFSSL_IP_ALT_NAME -DDTLS_CID_MAX_SIZE=8"

echo "==> build"
make -j"$(nproc)"

echo "==> install"
${SUDO} make install
if [[ "${PREFIX}" == "/usr/local" ]]; then
  ${SUDO} ldconfig
fi

echo
echo "wolfSSL instalado em ${PREFIX}"
grep -E "define (WOLFSSL_DTLS13|WOLFSSL_IP_ALT_NAME|OPENSSL_EXTRA|HAVE_AESCCM|WOLFSSL_DTLS_CID|DTLS_CID_MAX_SIZE)" \
  "${PREFIX}/include/wolfssl/options.h" || true

if [[ "${PREFIX}" != "/usr/local" ]]; then
  echo
  echo "Compile o protótipo com:"
  echo "  make WOLFSSL_DIR=${PREFIX}"
  echo "E exporte antes de executar:"
  echo "  export LD_LIBRARY_PATH=${PREFIX}/lib:\${LD_LIBRARY_PATH:-}"
fi
