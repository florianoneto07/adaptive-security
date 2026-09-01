/*
 * Cliente DTLS 1.3 sobre UDP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "adaptive_security.h"

#define DEFAULT_SERVER_HOST "127.0.0.1"
#define DEFAULT_PORT        4444

/* Retransmissão do handshake: dobra a cada tentativa, de 1s até 8s. */
#define DTLS_TIMEOUT_INIT 1
#define DTLS_TIMEOUT_MAX  8

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : DEFAULT_SERVER_HOST;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    int fd = -1;
    int ret;
    struct sockaddr_in peer;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    char ca_path[AS_PATH_MAX];
    char buffer[512];
    const char *msg = "Mensagem enviada pelo cliente via DTLS 1.3.";

    /*
     * Linha a linha: com stdout redirecionado para arquivo ou pipe, o buffer
     * padrão é por bloco e o progresso do handshake só apareceria no fim.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %s\n", argv[2]);
        return 1;
    }

    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_client_method());
    if (ctx == NULL) {
        fprintf(stderr, "DTLS 1.3 não disponível nesta build do wolfSSL.\n"
                        "Recompile com --enable-dtls --enable-dtls13.\n");
        goto fail;
    }

    if (as_cert_path(ca_path, sizeof(ca_path), "ca.crt") == NULL)
        goto fail;
    if (wolfSSL_CTX_load_verify_locations(ctx, ca_path, NULL) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar a CA em %s\n", ca_path);
        goto fail;
    }

    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);

    if (as_resolve_v4(host, port, SOCK_DGRAM, &peer) != 0)
        goto fail;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        goto fail;
    }

    /*
     * Socket UDP conectado: fixa o destino de send() e faz o kernel descartar
     * datagramas de outras origens, o que é o mínimo de sanidade para DTLS.
     */
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        perror("connect");
        goto fail;
    }

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão DTLS.\n");
        goto fail;
    }

    if (as_require_peer_identity(ssl, host) != 0)
        goto fail;

    /*
     * Sem timeout o cliente fica preso indefinidamente se o servidor não
     * responder, já que UDP não sinaliza a ausência do peer.
     */
    if (wolfSSL_dtls_set_timeout_init(ssl, DTLS_TIMEOUT_INIT) != WOLFSSL_SUCCESS ||
        wolfSSL_dtls_set_timeout_max(ssl, DTLS_TIMEOUT_MAX) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao configurar os timeouts do DTLS.\n");
        goto fail;
    }

    wolfSSL_set_fd(ssl, fd);
    if (wolfSSL_dtls_set_peer(ssl, &peer, sizeof(peer)) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao registrar o peer DTLS.\n");
        goto fail;
    }

    printf("Iniciando handshake DTLS 1.3 com %s:%d...\n", host, port);

    ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Falha no handshake DTLS: %s\n", as_ssl_error(ssl, ret));
        goto fail;
    }

    printf("Handshake DTLS concluído.\n");
    printf("Versão: %s\n", wolfSSL_get_version(ssl));
    printf("Cipher: %s\n", wolfSSL_get_cipher(ssl));
    printf("Certificado do servidor validado (cadeia + identidade %s).\n", host);

    ret = wolfSSL_write(ssl, msg, (int)strlen(msg));
    if (ret <= 0) {
        fprintf(stderr, "Falha ao enviar: %s\n", as_ssl_error(ssl, ret));
        goto fail;
    }
    printf("Mensagem enviada (%d bytes): %s\n", ret, msg);

    ret = wolfSSL_read(ssl, buffer, sizeof(buffer) - 1);
    if (ret > 0) {
        buffer[ret] = '\0';
        printf("Resposta recebida (%d bytes): %s\n", ret, buffer);
    } else {
        fprintf(stderr, "Falha ao receber: %s\n", as_ssl_error(ssl, ret));
        goto fail;
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    close(fd);
    wolfSSL_Cleanup();
    return 0;

fail:
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    if (fd >= 0)
        close(fd);
    wolfSSL_Cleanup();
    return 1;
}
