# Canal C3 - Vídeo e áudio em tempo real.
# Perfil fixo: handshake DTLS 1.3 + SRTP na mídia, UDP/5003.
#
# Depende do libsrtp2 além do wolfSSL. O wolfSSL precisa de --enable-srtp para
# expor a extensão use_srtp e a exportação de material de chave; sem isso o
# erro só apareceria no link. scripts/check_wolfssl.sh confere antes.
EXTRA_CFLAGS := $(shell pkg-config --cflags libsrtp2 2>/dev/null)
EXTRA_LIBS   := $(shell pkg-config --libs libsrtp2 2>/dev/null || echo -lsrtp2)

BINS := c3_server c3_client
include ../channel.mk
