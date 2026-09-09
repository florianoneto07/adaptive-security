/*
 * Canal C4 - Missão, firmware e logs. Perfil fixo: TLS 1.3 sobre TCP/5004.
 *
 * Transferência volumosa e diferida, em que confiabilidade e ordenação importam
 * mais que latência — daí TCP, e não UDP. O overhead de registro do TLS 1.3 é
 * de 14 bytes, contra 11 do DTLS 1.3, e se dilui num fluxo grande.
 *
 * Autenticação exclusivamente por PSK do canal (R2, sem fallback e sem
 * certificado). Lado receptor da transferência.
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

#include "as_keys.h"
#include "as_metrics.h"
#include "as_profile.h"
#include "as_signal.h"
#include "as_psk.h"

#define AS_THIS_CHANNEL AS_CH_C4_BULK

/* Padrão do testbed, não vem da literatura: casa com o registro máximo do TLS. */
#define DEFAULT_BLOCK 16384

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida] [-b bloco] [-k]\n"
        "  -p  porta de escuta (padrão %u)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n"
        "  -b  tamanho do bloco de leitura em bytes (padrão %d)\n"
        "  -k  atende conexões indefinidamente\n",
        prog, as_profile_get(AS_THIS_CHANNEL)->port, DEFAULT_BLOCK);
}

/*
 * Atende uma transferência. Retorna 0 em sucesso.
 *
 * Protocolo de aplicação: 8 bytes de tamanho total em ordem de rede, depois o
 * corpo, e por fim 8 bytes de confirmação com o total recebido. A confirmação
 * é o que fecha o RTT medido pelo cliente.
 */
static int serve_transfer(WOLFSSL_CTX *ctx, int connfd,
                          const as_profile_t *profile,
                          as_metrics_t *m, size_t block)
{
    WOLFSSL *ssl = NULL;
    unsigned char *buf = NULL;
    uint64_t t_start, cpu_start, expected = 0, received = 0, seq = 0;
    unsigned char hdr[8];
    int ret, rc = -1;

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão TLS.\n");
        return -1;
    }

    buf = malloc(block);
    if (buf == NULL) {
        fprintf(stderr, "Sem memória para o bloco de %zu bytes.\n", block);
        goto done;
    }

    wolfSSL_set_fd(ssl, connfd);
    if (as_wire_attach(ssl, m, 0) != 0)
        goto done;

    t_start = as_mono_ns();
    cpu_start = as_cpu_ns();

    ret = wolfSSL_accept(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        /*
         * R2: o canal falha, e não recai em outro perfil. A causa mais comum é
         * chave divergente entre as VMs; a segunda é cliente de outro canal.
         */
        fprintf(stderr, "Falha no handshake TLS do canal %s: %s\n",
                profile->slug, wolfSSL_ERR_reason_error_string(
                    (unsigned long)wolfSSL_get_error(ssl, ret)));
        goto done;
    }

    as_metrics_handshake(m, as_mono_ns() - t_start, as_cpu_ns() - cpu_start);

    printf("Handshake concluído: %s / %s / identidade PSK \"%s\"\n",
           wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl),
           profile->psk_identity);

    /* Cabeçalho de tamanho. */
    if (wolfSSL_read(ssl, hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        fprintf(stderr, "Cabeçalho de tamanho não recebido.\n");
        goto done;
    }
    for (int i = 0; i < 8; i++)
        expected = (expected << 8) | hdr[i];

    printf("Recebendo %llu bytes em blocos de %zu...\n",
           (unsigned long long)expected, block);

    while (received < expected && !g_stop) {
        size_t want = expected - received;
        uint64_t cpu0;

        if (want > block)
            want = block;

        cpu0 = as_cpu_ns();
        ret = wolfSSL_read(ssl, buf, (int)want);
        if (ret <= 0) {
            fprintf(stderr, "Falha ao receber após %llu bytes: %s\n",
                    (unsigned long long)received,
                    wolfSSL_ERR_reason_error_string(
                        (unsigned long)wolfSSL_get_error(ssl, ret)));
            goto done;
        }
        received += (uint64_t)ret;
        as_metrics_recv(m, seq++, (size_t)ret, as_cpu_ns() - cpu0);
    }

    /* Confirmação: total recebido, que fecha o RTT do lado do cliente. */
    for (int i = 7; i >= 0; i--) {
        hdr[i] = (unsigned char)(received & 0xFF);
        received >>= 8;
    }
    received = expected;   /* restaurado para o relatório */

    if (wolfSSL_write(ssl, hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        fprintf(stderr, "Falha ao confirmar a transferência.\n");
        goto done;
    }

    printf("Transferência completa: %llu bytes.\n",
           (unsigned long long)received);
    wolfSSL_shutdown(ssl);
    rc = 0;

done:
    free(buf);
    wolfSSL_free(ssl);
    return rc;
}

int main(int argc, char **argv)
{
    const as_profile_t *profile = as_profile_get(AS_THIS_CHANNEL);
    const char *out_dir = "results";
    int port, keep_running = 0, opt;
    size_t block = DEFAULT_BLOCK;
    int listenfd = -1, connfd = -1, one = 1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len;
    char peer_ip[INET_ADDRSTRLEN];
    char fp[AS_KEY_FP_LEN];
    unsigned char key[AS_PSK_LEN];
    WOLFSSL_CTX *ctx = NULL;
    as_metrics_t *m = NULL;
    int rc = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    port = profile->port;

    while ((opt = getopt(argc, argv, "p:o:b:kh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'b': block = (size_t)strtoul(optarg, NULL, 10); break;
        case 'k': keep_running = 1; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %d\n", port);
        return 1;
    }
    if (block == 0) {
        fprintf(stderr, "Bloco inválido: 0\n");
        return 1;
    }

    if (as_install_stop_handler(on_signal) != 0) {
        perror("sigaction");
        return 1;
    }

    if (as_key_load(profile, key) != 0)
        return 1;
    if (as_key_fingerprint(key, fp) != 0)
        return 1;

    printf("Canal %s (%s)\nPerfil fixo: %s, TCP/%d\nChave do canal: %s\n",
           profile->slug, profile->app_class, profile->profile_name, port, fp);

    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (ctx == NULL) {
        fprintf(stderr, "TLS 1.3 não disponível nesta build do wolfSSL.\n");
        goto out;
    }
    if (as_psk_install_server(ctx, profile, key) != 0)
        goto out;

    /* A chave já está no contexto; não há motivo para mantê-la no stack. */
    memset(key, 0, sizeof(key));

    m = as_metrics_open(profile, "server", out_dir);
    if (m == NULL)
        goto out;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        goto out;
    }
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto out;
    }
    if (listen(listenfd, 5) < 0) {
        perror("listen");
        goto out;
    }

    printf("Canal %s aguardando na porta %d...\n", profile->slug, port);

    do {
        peer_len = sizeof(peer);
        connfd = accept(listenfd, (struct sockaddr *)&peer, &peer_len);
        if (connfd < 0) {
            if (g_stop)
                break;
            perror("accept");
            goto out;
        }

        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        printf("Conexão de %s:%d\n", peer_ip, ntohs(peer.sin_port));

        if (serve_transfer(ctx, connfd, profile, m, block) != 0 && !keep_running)
            goto out;

        close(connfd);
        connfd = -1;
    } while (keep_running && !g_stop);

    rc = 0;

out:
    if (m != NULL)
        as_metrics_close(m);
    if (connfd >= 0)
        close(connfd);
    if (listenfd >= 0)
        close(listenfd);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return rc;
}
