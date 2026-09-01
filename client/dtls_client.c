/*
 * DTLS 1.3 client - estrutura preparada.
 * Ainda precisa de validação E2E no ambiente das duas VMs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#define DEFAULT_SERVER_IP "192.168.237.128"
#define DEFAULT_PORT 4444

int main(int argc, char **argv) {
    const char *server_ip = (argc > 1) ? argv[1] : DEFAULT_SERVER_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in peer;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;

    if (fd < 0) { perror("socket"); return 1; }
    wolfSSL_Init();
    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_client_method());
    if (!ctx) { fprintf(stderr, "DTLS 1.3 não disponível nesta build.\n"); return 1; }

    if (wolfSSL_CTX_load_verify_locations(ctx, "certs/ca.crt", NULL) != SSL_SUCCESS) {
        fprintf(stderr, "Erro ao carregar CA.\n"); return 1;
    }

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &peer.sin_addr);
    connect(fd, (struct sockaddr*)&peer, sizeof(peer));

    ssl = wolfSSL_new(ctx);
    wolfSSL_set_fd(ssl, fd);

    printf("Tentando handshake DTLS 1.3 com %s:%d...\n", server_ip, port);
    if (wolfSSL_connect(ssl) != SSL_SUCCESS) {
        fprintf(stderr, "Handshake DTLS ainda não validado neste pacote.\n");
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); close(fd); wolfSSL_Cleanup();
        return 1;
    }

    const char *msg = "Mensagem enviada pelo cliente via DTLS 1.3.";
    wolfSSL_write(ssl, msg, (int)strlen(msg));
    char buf[512] = {0};
    int n = wolfSSL_read(ssl, buf, sizeof(buf)-1);
    if (n > 0) printf("Resposta: %s\n", buf);

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); close(fd); wolfSSL_Cleanup();
    return 0;
}
