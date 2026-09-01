/*
 * DTLS 1.3 server - estrutura preparada.
 * Ainda precisa de validação E2E no ambiente das duas VMs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#define DEFAULT_PORT 4444

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;

    if (fd < 0) { perror("socket"); return 1; }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    wolfSSL_Init();
    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_server_method());
    if (!ctx) { fprintf(stderr, "DTLS 1.3 não disponível nesta build.\n"); return 1; }

    if (wolfSSL_CTX_use_certificate_file(ctx, "certs/server.crt", SSL_FILETYPE_PEM) != SSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_file(ctx, "certs/server.key", SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar certificado/chave.\n"); return 1;
    }

    ssl = wolfSSL_new(ctx);
    wolfSSL_set_fd(ssl, fd);
    printf("Servidor DTLS 1.3 aguardando na porta %d...\n", port);

    if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
        fprintf(stderr, "Handshake DTLS ainda não validado neste pacote.\n");
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); close(fd); wolfSSL_Cleanup();
        return 1;
    }

    char buf[512] = {0};
    int n = wolfSSL_read(ssl, buf, sizeof(buf)-1);
    if (n > 0) {
        printf("Mensagem recebida: %s\n", buf);
        const char *reply = "Mensagem recebida com sucesso via DTLS 1.3.";
        wolfSSL_write(ssl, reply, (int)strlen(reply));
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); close(fd); wolfSSL_Cleanup();
    return 0;
}
