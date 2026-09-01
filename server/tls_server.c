/*
 * Servidor TLS 1.3 sobre TCP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "adaptive_security.h"

#define DEFAULT_PORT 4433

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    int listenfd = -1, connfd = -1;
    int ret;
    int one = 1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len = sizeof(peer);
    char peer_ip[INET_ADDRSTRLEN];
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    char crt_path[AS_PATH_MAX], key_path[AS_PATH_MAX];
    char buffer[1024];
    const char *reply = "Mensagem recebida com sucesso via TLS 1.3.";

    /*
     * Linha a linha: com stdout redirecionado para arquivo ou pipe, o buffer
     * padrão é por bloco e o progresso do handshake só apareceria no fim.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %s\n", argv[1]);
        return 1;
    }

    /* Sem isso, escrever num socket já fechado pelo cliente mata o processo. */
    signal(SIGPIPE, SIG_IGN);
    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (ctx == NULL) {
        fprintf(stderr, "Erro ao criar contexto TLS 1.3.\n");
        goto fail;
    }

    if (as_cert_path(crt_path, sizeof(crt_path), "server.crt") == NULL ||
        as_cert_path(key_path, sizeof(key_path), "server.key") == NULL)
        goto fail;

    if (wolfSSL_CTX_use_certificate_file(ctx, crt_path, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar %s\n", crt_path);
        goto fail;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, key_path, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar %s\n", key_path);
        goto fail;
    }
    if (wolfSSL_CTX_check_private_key(ctx) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "A chave privada não corresponde ao certificado.\n");
        goto fail;
    }

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        goto fail;
    }
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto fail;
    }
    if (listen(listenfd, 5) < 0) {
        perror("listen");
        goto fail;
    }

    printf("Servidor TLS aguardando conexão na porta %d...\n", port);
    connfd = accept(listenfd, (struct sockaddr *)&peer, &peer_len);
    if (connfd < 0) {
        perror("accept");
        goto fail;
    }

    inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    printf("Conexão TCP recebida de %s:%d\n", peer_ip, ntohs(peer.sin_port));

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão TLS.\n");
        goto fail;
    }
    wolfSSL_set_fd(ssl, connfd);

    ret = wolfSSL_accept(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Falha no handshake TLS: %s\n", as_ssl_error(ssl, ret));
        goto fail;
    }

    printf("Handshake TLS concluído.\n");
    printf("Versão: %s\n", wolfSSL_get_version(ssl));
    printf("Cipher: %s\n", wolfSSL_get_cipher(ssl));

    ret = wolfSSL_read(ssl, buffer, sizeof(buffer) - 1);
    if (ret > 0) {
        buffer[ret] = '\0';
        printf("Mensagem recebida (%d bytes): %s\n", ret, buffer);

        ret = wolfSSL_write(ssl, reply, (int)strlen(reply));
        if (ret <= 0) {
            fprintf(stderr, "Falha ao responder: %s\n", as_ssl_error(ssl, ret));
            goto fail;
        }
        printf("Confirmação enviada (%d bytes).\n", ret);
    } else {
        fprintf(stderr, "Falha ao receber: %s\n", as_ssl_error(ssl, ret));
        goto fail;
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    close(connfd);
    close(listenfd);
    wolfSSL_Cleanup();
    return 0;

fail:
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    if (connfd >= 0)
        close(connfd);
    if (listenfd >= 0)
        close(listenfd);
    wolfSSL_Cleanup();
    return 1;
}
