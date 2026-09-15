/*
 * Baseline C4 - transferência volumosa SEM segurança (P1.3). TCP puro, receptor.
 *
 * Espelha o c4_server (lê o cabeçalho de tamanho, recebe os blocos, confirma o
 * total) sem TLS. Ver c4_plain_client.c para o racional.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "as_metrics.h"
#include "as_profile.h"
#include "as_signal.h"

#define AS_THIS_CHANNEL AS_CH_C4_BULK
#define DEFAULT_BLOCK 16384

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida] [-b bloco] [-k]\n"
        "  Baseline SEM segurança do canal C4 (TCP puro).\n", prog);
}

static int send_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* Recebe uma transferência: cabeçalho de 8 B, blocos, confirmação de 8 B. */
static int serve_transfer(int connfd, as_metrics_t *m, size_t block)
{
    unsigned char *buf, hdr[8];
    uint64_t expected = 0, received = 0, seq = 0;
    int rc = -1;

    buf = malloc(block);
    if (buf == NULL) { fprintf(stderr, "Sem memória.\n"); return -1; }

    { size_t got = 0;
      while (got < sizeof(hdr)) {
          ssize_t r = recv(connfd, hdr + got, sizeof(hdr) - got, 0);
          if (r <= 0) { fprintf(stderr, "Cabeçalho não recebido.\n"); goto done; }
          got += (size_t)r;
      } }
    for (int i = 0; i < 8; i++) expected = (expected << 8) | hdr[i];

    printf("Recebendo %llu bytes em blocos de %zu...\n",
           (unsigned long long)expected, block);

    while (received < expected && !g_stop) {
        size_t want = expected - received;
        uint64_t cpu0;
        ssize_t r;
        if (want > block) want = block;
        cpu0 = as_cpu_ns();
        r = recv(connfd, buf, want, 0);
        if (r <= 0) {
            if (g_stop) goto done;
            fprintf(stderr, "Falha ao receber após %llu bytes.\n",
                    (unsigned long long)received);
            goto done;
        }
        received += (uint64_t)r;
        as_metrics_recv(m, seq++, (size_t)r, as_cpu_ns() - cpu0);
    }

    { uint64_t v = received;
      for (int i = 7; i >= 0; i--) { hdr[i] = (unsigned char)(v & 0xFF); v >>= 8; } }
    if (send_all(connfd, hdr, sizeof(hdr)) != 0) {
        fprintf(stderr, "Falha ao confirmar.\n"); goto done;
    }
    printf("Transferência completa: %llu bytes.\n", (unsigned long long)received);
    rc = 0;
done:
    free(buf);
    return rc;
}

int main(int argc, char **argv)
{
    const char *out_dir = "results";
    int port, keep_running = 0, opt, rc = 1;
    size_t block = DEFAULT_BLOCK;
    int listenfd = -1, connfd = -1, one = 1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len;
    char peer_ip[INET_ADDRSTRLEN];
    as_profile_t profile;
    as_metrics_t *m = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c4_plain";
    profile.profile_name = "TCP puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "p:o:b:kh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'b': block = (size_t)strtoul(optarg, NULL, 10); break;
        case 'k': keep_running = 1; break;
        default:  usage(argv[0]); return 1;
        }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "Porta inválida: %d\n", port); return 1; }
    if (block == 0) { fprintf(stderr, "Bloco inválido: 0\n"); return 1; }

    if (as_install_stop_handler(on_signal) != 0) { perror("sigaction"); return 1; }

    printf("Baseline %s (%s)\nSem segurança: %s, TCP/%d\n",
           profile.slug, profile.app_class, profile.profile_name, port);

    m = as_metrics_open(&profile, "server", out_dir);
    if (m == NULL)
        goto out;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); goto out; }
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); goto out; }
    if (listen(listenfd, 5) < 0) { perror("listen"); goto out; }

    printf("Canal %s aguardando na porta %d...\n", profile.slug, port);

    do {
        peer_len = sizeof(peer);
        connfd = accept(listenfd, (struct sockaddr *)&peer, &peer_len);
        if (connfd < 0) {
            if (g_stop) break;
            perror("accept");
            goto out;
        }
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        printf("Conexão de %s:%d\n", peer_ip, ntohs(peer.sin_port));
        if (serve_transfer(connfd, m, block) != 0 && !keep_running)
            goto out;
        close(connfd);
        connfd = -1;
    } while (keep_running && !g_stop);
    rc = 0;

out:
    if (m != NULL) as_metrics_close(m);
    if (connfd >= 0) close(connfd);
    if (listenfd >= 0) close(listenfd);
    return rc;
}
