#!/usr/bin/env bash
#
# Confere que a build do wolfSSL apontada tem as opções que este testbed exige.
#
# Uso: ./scripts/check_wolfssl.sh <PREFIXO_DO_WOLFSSL>
#
# Existe porque a alternativa é pior: sem esta checagem, uma build incompleta
# não falha na compilação — falha no LINK, com "undefined reference" a um
# símbolo cujo nome não diz qual flag de configure está faltando. Pior ainda,
# a falta de PSK só apareceria em tempo de execução, como handshake que não
# fecha, e seria confundida com chave divergente entre as VMs.
#
# É também o que impede o erro de haver duas instalações de wolfSSL na mesma
# máquina e o build pegar a errada por pkg-config.
set -uo pipefail

PREFIX="${1:-}"
if [[ -z "${PREFIX}" ]]; then
  echo "Uso: $0 <PREFIXO_DO_WOLFSSL>" >&2
  exit 2
fi

OPTIONS="${PREFIX}/include/wolfssl/options.h"
if [[ ! -f "${OPTIONS}" ]]; then
  cat >&2 <<MSG
wolfSSL não encontrado em ${PREFIX} (esperado ${OPTIONS}).

Instale com:
  ./scripts/setup_wolfssl.sh "${PREFIX}"
MSG
  exit 1
fi

MISSING=()

# define_presente <MACRO> <o que depende dela>
need_define() {
  grep -qE "^#define +$1( |\$)" "${OPTIONS}" || MISSING+=("$1 — $2")
}

# Ausência de macro: NO_PSK presente significa PSK compilado FORA.
need_absent() {
  grep -qE "^#define +$1( |\$)" "${OPTIONS}" && MISSING+=("!$1 — $2")
}

need_define WOLFSSL_TLS13       "TLS 1.3, usado pelo canal C4 (--enable-tls13)"
need_define WOLFSSL_DTLS13      "DTLS 1.3, canais C1 e C3 (--enable-dtls13)"
need_define WOLFSSL_SRTP        "DTLS-SRTP, canal C3 (--enable-srtp)"
need_define HAVE_AESCCM         "AES-CCM-16-64-128 do OSCORE, canal C2 (--enable-aesccm)"
need_define OPENSSL_EXTRA       "validação de SAN de IP nos binários legados (--enable-opensslextra)"
need_define OPENSSL_ALL         "símbolos que o libcoap referencia (--enable-opensslall)"
need_define WOLFSSL_IP_ALT_NAME "comparação de subjectAltName iPAddress (-DWOLFSSL_IP_ALT_NAME)"
need_define WOLFSSL_DTLS_CID    "Connection ID do DTLS 1.3, RFC 9146 (--enable-dtlscid)"
need_absent NO_PSK              "PSK por canal em C1/C3/C4 (--enable-psk)"

if [[ "${#MISSING[@]}" -gt 0 ]]; then
  {
    echo
    echo "A build do wolfSSL em ${PREFIX} não serve para este testbed."
    echo "Faltando:"
    printf '  %s\n' "${MISSING[@]}"
    echo
    echo "Recompile com as flags corretas:"
    echo "  ./scripts/setup_wolfssl.sh \"${PREFIX}\""
    echo
    echo "Se houver mais de uma instalação de wolfSSL na máquina, aponte a"
    echo "certa explicitamente: make WOLFSSL_DIR=${PREFIX}"
    echo
  } >&2
  exit 1
fi

exit 0
