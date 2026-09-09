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
SRC_VAR="LIBCOAP_SRC"

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

# Commit fixo, e não um branch. Com "origin/HEAD" cada pessoa que clonasse este
# repositório compilaria contra uma revisão diferente do libcoap, e as métricas
# coletadas deixariam de ser comparáveis entre si — que é exatamente o que o
# testbed precisa garantir. Este é o commit contra o qual ele foi validado.
# Para testar outra revisão, sobrescreva LIBCOAP_COMMIT.
LIBCOAP_COMMIT="${LIBCOAP_COMMIT:-dbeedd59b4097b3587af2bda5a387f86df97a056}"

if [[ ! -d "${SRC_DIR}/.git" ]]; then
  echo "==> Preparando ${SRC_DIR}"
  git init -q "${SRC_DIR}"
  git -C "${SRC_DIR}" remote add origin "${LIBCOAP_REPO}"
fi

echo "==> Buscando o libcoap ${LIBCOAP_COMMIT:0:9}"
# Buscar um commit avulso depende de o servidor permitir; o GitHub permite. Se
# não permitir, cai para o histórico completo em vez de falhar.
if ! git -C "${SRC_DIR}" fetch --quiet --depth 1 origin "${LIBCOAP_COMMIT}" 2>/dev/null; then
  echo "==> Commit avulso recusado pelo servidor; baixando o histórico"
  git -C "${SRC_DIR}" fetch --quiet origin
fi
git -C "${SRC_DIR}" checkout --quiet --detach "${LIBCOAP_COMMIT}"

# Trocar de flags de configure sem limpar deixa objetos compilados sob as
# opções antigas, e o defeito aparece como símbolo ausente no link de outro
# projeto — longe da causa.
if [[ -f "${SRC_DIR}/Makefile" ]]; then
  echo "==> Limpando a árvore"
  make -C "${SRC_DIR}" -s distclean >/dev/null 2>&1 || true
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
