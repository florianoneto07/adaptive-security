/*
 * Servidor DTLS 1.3 sobre UDP.
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

#define DEFAULT_PORT      4444
#define DTLS_TIMEOUT_INIT 1
#define DTLS_TIMEOUT_MAX  8

/* Tipo de registro DTLS "handshake" (RFC 9147). O ClientHello inicial trafega
 * em texto claro com este tipo, mesmo em DTLS 1.3. */
#define DTLS_CONTENT_HANDSHAKE 22

static void usage(const char *prog)
{
    fprintf(stderr, "Uso: %s [-k] [porta]\n"
                    "  -k  atende associações indefinidamente (Ctrl+C encerra)\n"
                    "      sem -k, atende uma associação e sai\n", prog);
}

/*
 * Atende uma associação DTLS na porta indicada, do bind ao encerramento.
 *
 * O socket é criado e destruído a cada associação de propósito: como ele é
 * conectado ao peer descoberto, atender outro cliente exige um socket novo.
 * Um servidor de produção demultiplexaria várias associações sobre o mesmo
 * socket; para o protótipo, uma de cada vez basta e mantém o código legível.
 *
 * Retorna 0 em sucesso.
 */
static int serve_association(WOLFSSL_CTX *ctx, int port)
{
    int fd = -1;
    int one = 1;
    int ret, rc = -1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len = sizeof(peer);
    char peer_ip[INET_ADDRSTRLEN];
    unsigned char probe[1];
    WOLFSSL *ssl = NULL;
    char buffer[512];
    const char *reply = "Mensagem recebida com sucesso via DTLS 1.3.";

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto done;
    }

    printf("Servidor DTLS 1.3 aguardando na porta %d...\n", port);

    /*
     * UDP não tem accept(): antes do handshake é preciso descobrir quem está
     * falando. MSG_PEEK espia o primeiro datagrama (o ClientHello) sem
     * consumi-lo, para que o wolfSSL ainda o encontre no socket.
     *
     * Era esta a causa do handshake não fechar: wolfSSL_accept() era chamado
     * num socket sem peer definido, então as respostas do servidor não tinham
     * para onde ir.
     */
    /*
     * Só um ClientHello inicia associação. Em modo -k, o close_notify da
     * associação anterior costuma chegar depois de o socket ser recriado; sem
     * este filtro ele seria tomado como início de uma nova associação, com o
     * servidor conectando-se a um peer que já foi embora.
     */
    for (;;) {
        peer_len = sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        if (recvfrom(fd, probe, sizeof(probe), MSG_PEEK,
                     (struct sockaddr *)&peer, &peer_len) < 0) {
            perror("recvfrom");
            goto done;
        }
        if (probe[0] == DTLS_CONTENT_HANDSHAKE)
            break;

        /* Consome e descarta o datagrama residual. */
        if (recvfrom(fd, probe, sizeof(probe), 0, NULL, NULL) < 0) {
            perror("recvfrom");
            goto done;
        }
        printf("Datagrama descartado: tipo de registro %u, não é handshake.\n",
               probe[0]);
    }

    inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    printf("Datagrama inicial recebido de %s:%d\n", peer_ip, ntohs(peer.sin_port));

    /*
     * Conecta o socket ao peer descoberto: send() passa a ter destino e o
     * kernel filtra datagramas de outras origens durante a sessão.
     */
    if (connect(fd, (struct sockaddr *)&peer, peer_len) < 0) {
        perror("connect");
        goto done;
    }

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão DTLS.\n");
        goto done;
    }

    if (wolfSSL_dtls_set_timeout_init(ssl, DTLS_TIMEOUT_INIT) != WOLFSSL_SUCCESS ||
        wolfSSL_dtls_set_timeout_max(ssl, DTLS_TIMEOUT_MAX) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao configurar os timeouts do DTLS.\n");
        goto done;
    }

    wolfSSL_set_fd(ssl, fd);
    if (wolfSSL_dtls_set_peer(ssl, &peer, peer_len) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao registrar o peer DTLS.\n");
        goto done;
    }

#ifdef WOLFSSL_DTLS13
    /*
     * Cookie no HelloRetryRequest (RFC 9147, seção 5.1). Obriga o cliente a
     * provar que recebe no endereço de origem que alega ter, antes de o
     * servidor gastar CPU com o handshake. Sem isso, um ClientHello com IP
     * forjado transforma o servidor em amplificador de tráfego.
     * Com secret NULL/0 o wolfSSL gera um segredo aleatório próprio.
     */
    if (wolfSSL_send_hrr_cookie(ssl, NULL, 0) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao habilitar o cookie de HelloRetryRequest.\n");
        goto done;
    }
#endif

    ret = wolfSSL_accept(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Falha no handshake DTLS: %s\n", as_ssl_error(ssl, ret));
        goto done;
    }

    printf("Handshake DTLS concluído.\n");
    printf("Versão: %s\n", wolfSSL_get_version(ssl));
    printf("Cipher: %s\n", wolfSSL_get_cipher(ssl));

    ret = wolfSSL_read(ssl, buffer, sizeof(buffer) - 1);
    if (ret <= 0) {
        fprintf(stderr, "Falha ao receber: %s\n", as_ssl_error(ssl, ret));
        goto done;
    }
    buffer[ret] = '\0';
    printf("Mensagem recebida (%d bytes): %s\n", ret, buffer);

    ret = wolfSSL_write(ssl, reply, (int)strlen(reply));
    if (ret <= 0) {
        fprintf(stderr, "Falha ao responder: %s\n", as_ssl_error(ssl, ret));
        goto done;
    }
    printf("Confirmação enviada (%d bytes).\n", ret);

    wolfSSL_shutdown(ssl);
    rc = 0;

done:
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (fd >= 0)
        close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    int keep_running = 0;
    int i;
    WOLFSSL_CTX *ctx = NULL;
    char crt_path[AS_PATH_MAX], key_path[AS_PATH_MAX];

    /*
     * Linha a linha: com stdout redirecionado para arquivo ou pipe, o buffer
     * padrão é por bloco e o progresso do handshake só apareceria no fim.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            keep_running = 1;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 1;
        } else {
            port = atoi(argv[i]);
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %d\n", port);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_server_method());
    if (ctx == NULL) {
        fprintf(stderr, "DTLS 1.3 não disponível nesta build do wolfSSL.\n"
                        "Recompile com --enable-dtls --enable-dtls13.\n");
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
    /* Pega cert/chave trocados na hora de subir, não no meio do handshake. */
    if (wolfSSL_CTX_check_private_key(ctx) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "A chave privada não corresponde ao certificado.\n");
        goto fail;
    }

    do {
        if (serve_association(ctx, port) != 0 && !keep_running)
            goto fail;
        if (keep_running)
            printf("\n");
    } while (keep_running);

    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;

fail:
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 1;
}
