/*
 * Baseline C1 - controle SEM segurança (P1.3). UDP puro, lado receptor.
 *
 * Espelha o c1_server (eco dos primeiros AS_HDR_LEN bytes, contagem de entrega
 * e de perda por lacuna de sequência) sem a camada DTLS. Ver c1_plain_client.c
 * para o racional. Não é fallback do canal seguro.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "as_metrics.h"
#include "as_pack.h"
#include "as_profile.h"
#include "as_signal.h"

#define AS_THIS_CHANNEL AS_CH_C1_CONTROL
#define MAX_MSG 2048

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida] [-k]\n"
        "  Baseline SEM segurança do canal C1 (UDP puro).\n"
        "  -k  atende repetições sucessivas sem sair\n", prog);
}

int main(int argc, char **argv)
{
    const char *out_dir = "results";
    int port, keep_running = 0, opt, rc = 1, fd = -1, one = 1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len;
    unsigned char buf[MAX_MSG];
    as_profile_t profile;
    as_metrics_t *m = NULL;
    uint32_t expected_seq = 0;
    int first = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c1_plain";
    profile.profile_name = "UDP puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "p:o:kh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'k': keep_running = 1; break;
        default:  usage(argv[0]); return 1;
        }
    }
    (void)keep_running;   /* um socket UDP atende todas as repetições */
    if (port <= 0 || port > 65535) { fprintf(stderr, "Porta inválida: %d\n", port); return 1; }

    if (as_install_stop_handler(on_signal) != 0) { perror("sigaction"); return 1; }

    printf("Baseline %s (%s)\nSem segurança: %s, UDP/%d\n",
           profile.slug, profile.app_class, profile.profile_name, port);

    m = as_metrics_open(&profile, "server", out_dir);
    if (m == NULL)
        goto out;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); goto out; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); goto out; }

    printf("Canal %s aguardando na porta %d...\n", profile.slug, port);

    while (!g_stop) {
        uint32_t seq;
        uint64_t cpu0, cpu_used;
        ssize_t n;

        peer_len = sizeof(peer);
        cpu0 = as_cpu_ns();
        n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
        cpu_used = as_cpu_ns() - cpu0;
        if (n < 0) {
            if (g_stop) break;
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        if (n < AS_HDR_LEN)
            continue;

        seq = as_get_u32(buf);
        if (first) {
            first = 0;
        } else if (seq > expected_seq) {
            uint32_t gap;
            for (gap = expected_seq; gap < seq; gap++)
                as_metrics_lost(m, gap);
        }
        expected_seq = seq + 1;

        as_metrics_recv(m, seq, (size_t)n, cpu_used);

        /* Eco dos primeiros AS_HDR_LEN bytes, como no canal seguro: instrumento
         * de RTT, não mecanismo de confiabilidade. */
        if (sendto(fd, buf, AS_HDR_LEN, 0, (struct sockaddr *)&peer, peer_len) != AS_HDR_LEN) {
            if (!g_stop) fprintf(stderr, "Falha ao ecoar %u.\n", seq);
        }
    }

    printf("Recepção encerrada.\n");
    rc = 0;

out:
    if (m != NULL) as_metrics_close(m);
    if (fd >= 0) close(fd);
    return rc;
}
