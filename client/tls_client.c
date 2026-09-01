#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#define DEFAULT_SERVER_IP "192.168.237.128"
#define DEFAULT_PORT 4433

int main(int argc, char **argv) {
    const char *server_ip = (argc > 1) ? argv[1] : DEFAULT_SERVER_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    int sockfd = -1;
    struct sockaddr_in server_addr;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    char buffer[1024] = {0};
    const char *msg = "Mensagem enviada pelo cliente em C usando TLS 1.3.";

    wolfSSL_Init();
    ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!ctx) {
        fprintf(stderr, "Erro ao criar contexto TLS.\n");
        goto fail;
    }

    if (wolfSSL_CTX_load_verify_locations(ctx, "certs/ca.crt", NULL) != SSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar CA em certs/ca.crt\n");
        goto fail;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); goto fail; }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "IP inválido: %s\n", server_ip);
        goto fail;
    }

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        goto fail;
    }
    printf("Conexão TCP estabelecida com %s:%d\n", server_ip, port);

    ssl = wolfSSL_new(ctx);
    if (!ssl) goto fail;
    wolfSSL_set_fd(ssl, sockfd);

    if (wolfSSL_connect(ssl) != SSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl, 0);
        fprintf(stderr, "Falha no handshake TLS. erro=%d\n", err);
        goto fail;
    }

    printf("Handshake TLS concluído.\n");
    printf("Versão: %s\n", wolfSSL_get_version(ssl));
    printf("Cipher: %s\n", wolfSSL_get_cipher(ssl));
    printf("Certificado do servidor validado.\n");

    int sent = wolfSSL_write(ssl, msg, (int)strlen(msg));
    if (sent <= 0) goto fail;
    printf("Mensagem enviada (%d bytes): %s\n", sent, msg);

    int n = wolfSSL_read(ssl, buffer, sizeof(buffer)-1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("Resposta recebida (%d bytes): %s\n", n, buffer);
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    close(sockfd);
    wolfSSL_Cleanup();
    return 0;

fail:
    if (ssl) wolfSSL_free(ssl);
    if (ctx) wolfSSL_CTX_free(ctx);
    if (sockfd >= 0) close(sockfd);
    wolfSSL_Cleanup();
    return 1;
}
