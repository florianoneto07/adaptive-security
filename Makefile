# Adaptive Security Prototype - build TLS 1.3 / DTLS 1.3 (wolfSSL)
#
# Uso:
#   make                        compila todos os binários em build/
#   make tls_server             compila um alvo específico
#   make WOLFSSL_DIR=/opt/ws    usa um wolfSSL fora do prefixo padrão
#   make certs                  gera os certificados de teste em certs/
#   make clean                  remove os artefatos de compilação

CC    ?= cc
BUILD := build

# Localização do wolfSSL. Se WOLFSSL_DIR for informado, ele tem precedência;
# caso contrário tenta-se pkg-config e, por fim, o prefixo padrão de
# "make install" do wolfSSL (/usr/local).
WOLFSSL_DIR ?=
ifneq ($(WOLFSSL_DIR),)
  WOLFSSL_CFLAGS := -I$(WOLFSSL_DIR)/include
  WOLFSSL_LIBS   := -L$(WOLFSSL_DIR)/lib -lwolfssl
else
  WOLFSSL_CFLAGS := $(shell pkg-config --cflags wolfssl 2>/dev/null || echo -I/usr/local/include)
  WOLFSSL_LIBS   := $(shell pkg-config --libs   wolfssl 2>/dev/null || echo -L/usr/local/lib -lwolfssl)
endif

# CFLAGS/LDFLAGS pertencem ao usuário e podem ser sobrescritos na linha de
# comando. As flags obrigatórias ficam em ALL_CFLAGS para não serem perdidas
# quando o make recebe CFLAGS=... (variável de linha de comando ignora "+=").
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic
LDFLAGS ?=

ALL_CFLAGS := -std=c11 -D_DEFAULT_SOURCE $(WOLFSSL_CFLAGS) $(CFLAGS)
ALL_LDLIBS := $(WOLFSSL_LIBS) $(LDLIBS)

SERVER_BINS := tls_server dtls_server
CLIENT_BINS := tls_client dtls_client
BINS        := $(SERVER_BINS) $(CLIENT_BINS)
TARGETS     := $(addprefix $(BUILD)/,$(BINS))

.PHONY: all clean certs $(BINS)

all: $(TARGETS)

# Atalhos: "make tls_server" em vez de "make build/tls_server".
$(BINS): %: $(BUILD)/%

$(addprefix $(BUILD)/,$(SERVER_BINS)): $(BUILD)/%: server/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(LDFLAGS) $(ALL_LDLIBS)

$(addprefix $(BUILD)/,$(CLIENT_BINS)): $(BUILD)/%: client/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(LDFLAGS) $(ALL_LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

certs:
	./scripts/generate_certs.sh

clean:
	rm -rf $(BUILD)
