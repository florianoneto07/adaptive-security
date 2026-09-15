# Testbed de perfis de segurança fixos por classe de aplicação UAV.
#
# Uso:
#   make                        compila os quatro canais e os binários legados
#   make channels               só os quatro canais
#   make c1                     só o canal C1 (idem c2, c3, c4)
#   make legacy                 só os binários TLS/DTLS originais
#   make WOLFSSL_DIR=~/.local   usa um wolfSSL fora do prefixo padrão
#   make check                  confere as opções da build do wolfSSL
#   make keys                   gera uma chave por canal em keys/
#   make certs                  gera os certificados de teste em certs/
#   make clean                  remove os artefatos de compilação
#
# Os quatro canais autenticam por PSK e NÃO usam certificado. Os binários
# legados (tls_*, dtls_*) continuam com X.509 e seguem intocados; certs/ existe
# para eles.

CC    ?= cc
BUILD := build

# Localização do wolfSSL. WOLFSSL_DIR tem precedência; senão pkg-config; senão
# o prefixo padrão de "make install".
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

CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic
LDFLAGS ?=

ALL_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -Icommon $(WOLFSSL_CFLAGS) $(CFLAGS)
ALL_LDLIBS := $(WOLFSSL_LIBS) $(LDLIBS)

COMMON_HDR  := common/adaptive_security.h

SERVER_BINS := tls_server dtls_server
CLIENT_BINS := tls_client dtls_client
LEGACY_BINS := $(SERVER_BINS) $(CLIENT_BINS)
LEGACY_TGTS := $(addprefix $(BUILD)/,$(LEGACY_BINS))

CHANNELS := c1_control c2_telemetry c3_media c4_bulk

.PHONY: all channels legacy baseline check keys certs clean $(CHANNELS) \
        $(LEGACY_BINS) c1 c2 c3 c4

all: check legacy channels baseline

# ---------------------------------------------------------------------------
# Verificação da build do wolfSSL
# ---------------------------------------------------------------------------

# Roda antes de qualquer compilação. Uma build sem PSK ou sem SRTP compila os
# canais sem erro e só falha no link ou, pior, em execução — onde o sintoma é
# indistinguível de chave divergente entre as VMs.
check:
	@./scripts/check_wolfssl.sh "$(WOLFSSL_PREFIX)"

# ---------------------------------------------------------------------------
# Canais
# ---------------------------------------------------------------------------

channels: $(CHANNELS)

# Cada canal é um par de binários independente (R1): sobe, roda e é derrubado
# sem afetar os outros. O Makefile de cada um vive em channels/<canal>/.
$(CHANNELS):
	@$(MAKE) --no-print-directory -C channels/$@ WOLFSSL_DIR="$(WOLFSSL_DIR)"

# Baselines sem segurança (P1.3), para isolar o custo do perfil. Ficam num
# alvo próprio, mas entram em "all" para que um único "make" produza tudo.
baseline:
	@$(MAKE) --no-print-directory -C channels/baseline WOLFSSL_DIR="$(WOLFSSL_DIR)"

# Atalhos curtos.
c1: c1_control
c2: c2_telemetry
c3: c3_media
c4: c4_bulk

# ---------------------------------------------------------------------------
# Binários legados (TLS/DTLS com certificado, portas 4433 e 4444)
# ---------------------------------------------------------------------------

legacy: $(LEGACY_TGTS)

$(LEGACY_BINS): %: $(BUILD)/%

$(addprefix $(BUILD)/,$(SERVER_BINS)): $(BUILD)/%: server/%.c $(COMMON_HDR) | $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(LDFLAGS) $(ALL_LDLIBS)

$(addprefix $(BUILD)/,$(CLIENT_BINS)): $(BUILD)/%: client/%.c $(COMMON_HDR) | $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(LDFLAGS) $(ALL_LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

# ---------------------------------------------------------------------------
# Material criptográfico
# ---------------------------------------------------------------------------

keys:
	./scripts/generate_keys.sh

certs:
	./scripts/generate_certs.sh

clean:
	rm -rf $(BUILD)
	@for c in $(CHANNELS); do \
	  $(MAKE) --no-print-directory -C channels/$$c clean WOLFSSL_DIR="$(WOLFSSL_DIR)" 2>/dev/null || true; \
	done
