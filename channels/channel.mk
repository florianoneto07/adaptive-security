# Regras comuns aos Makefiles dos quatro canais.
#
# Cada canal define, ANTES de incluir este arquivo:
#   BINS          binários a produzir (ex.: c4_server c4_client)
#   EXTRA_CFLAGS  opcional, flags específicas do canal
#   EXTRA_LIBS    opcional, bibliotecas específicas (libcoap no C2, libsrtp2 no C3)
#
# Os canais compartilham código (chaves, PSK, métricas) mas NÃO compartilham
# binário: cada par servidor/cliente sobe, roda e é derrubado sem afetar os
# outros (R1).

CC ?= cc

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD     := $(REPO_ROOT)/build
COMMON    := $(REPO_ROOT)/common

# Localização do wolfSSL. WOLFSSL_DIR tem precedência; senão pkg-config; senão
# o prefixo padrão de "make install". WOLFSSL_PREFIX é o que a checagem de
# opções recebe, e precisa apontar para a MESMA build que o link vai usar —
# ter duas instalações divergentes na máquina é um erro difícil de perceber.
WOLFSSL_DIR ?=
ifneq ($(WOLFSSL_DIR),)
  WOLFSSL_CFLAGS := -I$(WOLFSSL_DIR)/include
  WOLFSSL_LIBS   := -L$(WOLFSSL_DIR)/lib -lwolfssl
  WOLFSSL_PREFIX := $(WOLFSSL_DIR)
else
  WOLFSSL_CFLAGS := $(shell pkg-config --cflags wolfssl 2>/dev/null || echo -I/usr/local/include)
  WOLFSSL_LIBS   := $(shell pkg-config --libs   wolfssl 2>/dev/null || echo -L/usr/local/lib -lwolfssl)
  WOLFSSL_PREFIX := $(shell pkg-config --variable=prefix wolfssl 2>/dev/null || echo /usr/local)
endif

# CFLAGS pertence ao usuário e pode ser sobrescrito na linha de comando; o que
# é obrigatório vive em ALL_CFLAGS, porque variável de linha de comando ignora
# "+=" e as flags essenciais seriam perdidas.
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic
LDFLAGS ?=

# _GNU_SOURCE: os geradores de C1 e C3 usam ppoll(), que espera o próximo prazo
# de envio com resolução de nanossegundos. Com poll(), cuja granularidade é de
# 1 ms, a cadência de 64 Hz acumularia desvio ao longo de uma execução inteira.
ALL_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE -I$(COMMON) \
              $(WOLFSSL_CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)
ALL_LDLIBS := $(WOLFSSL_LIBS) $(EXTRA_LIBS) $(LDLIBS)

COMMON_SRC := $(COMMON)/as_keys.c $(COMMON)/as_psk.c $(COMMON)/as_metrics.c
COMMON_HDR := $(wildcard $(COMMON)/*.h)

# Headers do próprio canal (ex.: c2_oscore.h). Sem eles nas dependências, mexer
# num header local não recompila nada, e servidor e cliente ficam com versões
# diferentes da mesma lógica — o que aparece como falha de decifragem OSCORE,
# bem longe da causa.
CHANNEL_HDR := $(wildcard *.h)

TARGETS := $(addprefix $(BUILD)/,$(BINS))

.PHONY: all clean check $(BINS)

all: check $(TARGETS)

# Atalhos: "make c4_server" em vez de "make ../../build/c4_server".
$(BINS): %: $(BUILD)/%

# A checagem roda antes de qualquer compilação, de propósito. Uma build de
# wolfSSL sem PSK compila sem erro e só falha no handshake, em execução, onde
# o sintoma é indistinguível de chave divergente entre as VMs.
check:
	@$(REPO_ROOT)/scripts/check_wolfssl.sh "$(WOLFSSL_PREFIX)"

$(BUILD)/%: %.c $(COMMON_SRC) $(COMMON_HDR) $(CHANNEL_HDR) | $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(COMMON_SRC) $(LDFLAGS) $(ALL_LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -f $(TARGETS)
