# Canal C2 - Telemetria periódica.
# Perfil fixo: OSCORE sobre CoAP, UDP/5002.
#
# Depende do libcoap com OSCORE, além do wolfSSL. LIBCOAP_DIR aponta o prefixo
# quando ele não está num lugar conhecido pelo pkg-config; por padrão usa o
# mesmo prefixo do wolfSSL, que é como scripts/setup_libcoap.sh instala.
LIBCOAP_DIR ?= $(WOLFSSL_DIR)

ifneq ($(LIBCOAP_DIR),)
  EXTRA_CFLAGS := -I$(LIBCOAP_DIR)/include
  EXTRA_LIBS   := -L$(LIBCOAP_DIR)/lib -lcoap-3-wolfssl
else
  EXTRA_CFLAGS := $(shell pkg-config --cflags libcoap-3-wolfssl 2>/dev/null)
  EXTRA_LIBS   := $(shell pkg-config --libs   libcoap-3-wolfssl 2>/dev/null || echo -lcoap-3-wolfssl)
endif

BINS := c2_server c2_client
include ../channel.mk
