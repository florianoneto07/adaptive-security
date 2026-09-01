#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#define DEFAULT_PORT 4433

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    int listenfd = -1, connfd = -1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len = sizeof(peer);
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    char buffer[1024] = {0};
    const char *reply = "Mensagem recebida com sucesso via TLS 1.3.";

    wolfSSL_Init();
    ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (!ctx) goto fail;

    if (wolfSSL_CTX_use_certificate_file(ctx, "certs/server.crt", SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar certs/server.crt\n");
        goto fail;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, "certs/server.key", SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar certs/server.key\n");
        goto fail;
    }

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); goto fail; }
    int one = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); goto fail; }
    if (listen(listenfd, 5) < 0) { perror("listen"); goto fail; }

    printf("Servidor TLS aguardando conexão na porta %d...\n", port);
    connfd = accept(listenfd, (struct sockaddr*)&peer, &peer_len);
    if (connfd < 0) { perror("accept"); goto fail; }

    char peer_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    printf("Conexão TCP recebida de %s:%d\n", peer_ip, ntohs(peer.sin_port));

    ssl = wolfSSL_new(ctx);
    if (!ssl) goto fail;
    wolfSSL_set_fd(ssl, connfd);

    if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
        fprintf(stderr, "Falha no handshake TLS.\n");
        goto fail;
    }

    printf("Handshake TLS concluído.\n");
    printf("Versão: %s\n", wolfSSL_get_version(ssl));
    printf("Cipher: %s\n", wolfSSL_get_cipher(ssl));

    int n = wolfSSL_read(ssl, buffer, sizeof(buffer)-1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("Mensagem recebida (%d bytes): %s\n", n, buffer);
        int sent = wolfSSL_write(ssl, reply, (int)strlen(reply));
        printf("Confirmação enviada (%d bytes).\n", sent);
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    close(connfd);
    close(listenfd);
    wolfSSL_Cleanup();
    return 0;

fail:
    if (ssl) wolfSSL_free(ssl);
    if (ctx) wolfSSL_CTX_free(ctx);
    if (connfd >= 0) close(connfd);
    if (listenfd >= 0) close(listenfd);
    wolfSSL_Cleanup();
    return 1;
}
