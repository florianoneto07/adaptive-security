# Baseline P1.3 - versões SEM segurança dos quatro canais, para isolar o custo
# do perfil. NÃO fazem parte dos perfis fixos (R2): são binários de medição
# separados, sem caminho de código que troque um canal seguro por estes.
#
# O C2 baseline usa libcoap (CoAP puro); os demais usam só sockets. Todos linkam
# wolfSSL porque common/as_metrics.c depende dele, ainda que estes binários não
# usem TLS/DTLS. LIBCOAP_DIR segue o mesmo padrão do canal C2.
LIBCOAP_DIR ?= $(WOLFSSL_DIR)

ifneq ($(LIBCOAP_DIR),)
  EXTRA_CFLAGS := -I$(LIBCOAP_DIR)/include
  EXTRA_LIBS   := -L$(LIBCOAP_DIR)/lib -lcoap-3-wolfssl
else
  EXTRA_CFLAGS := $(shell pkg-config --cflags libcoap-3-wolfssl 2>/dev/null)
  EXTRA_LIBS   := $(shell pkg-config --libs   libcoap-3-wolfssl 2>/dev/null || echo -lcoap-3-wolfssl)
endif

BINS := c1_plain_server c1_plain_client \
        c2_plain_server c2_plain_client \
        c3_plain_server c3_plain_client \
        c4_plain_server c4_plain_client

include ../channel.mk
