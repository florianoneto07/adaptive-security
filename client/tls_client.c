/*
 * Cliente TLS 1.3 sobre TCP.
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
#define DEFAULT_PORT        4433

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : DEFAULT_SERVER_HOST;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    int sockfd = -1;
    int ret;
    struct sockaddr_in server_addr;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    char ca_path[AS_PATH_MAX];
    char buffer[1024];
    const char *msg = "Mensagem enviada pelo cliente em C usando TLS 1.3.";

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

    ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (ctx == NULL) {
        fprintf(stderr, "Erro ao criar contexto TLS 1.3.\n");
        goto fail;
    }

    if (as_cert_path(ca_path, sizeof(ca_path), "ca.crt") == NULL)
        goto fail;
    if (wolfSSL_CTX_load_verify_locations(ctx, ca_path, NULL) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar a CA em %s\n", ca_path);
        goto fail;
    }

    /* Explícito de propósito: não depender do padrão da build do wolfSSL. */
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);

    if (as_resolve_v4(host, port, SOCK_STREAM, &server_addr) != 0)
        goto fail;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        goto fail;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        goto fail;
    }
    printf("Conexão TCP estabelecida com %s:%d\n", host, port);

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão TLS.\n");
        goto fail;
    }

    /* Amarra o certificado à identidade do servidor antes do handshake. */
    if (as_require_peer_identity(ssl, host) != 0)
        goto fail;

    wolfSSL_set_fd(ssl, sockfd);

    ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Falha no handshake TLS: %s\n", as_ssl_error(ssl, ret));
        goto fail;
    }

    printf("Handshake TLS concluído.\n");
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
    close(sockfd);
    wolfSSL_Cleanup();
    return 0;

fail:
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    if (sockfd >= 0)
        close(sockfd);
    wolfSSL_Cleanup();
    return 1;
}
