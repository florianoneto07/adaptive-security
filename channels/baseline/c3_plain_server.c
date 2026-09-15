/*
 * Baseline C3 - mídia em tempo real SEM segurança (P1.3). RTP sobre UDP puro,
 * lado receptor. Sem DTLS, sem SRTP.
 *
 * Espelha o c3_server (atraso unidirecional pela marca de tempo no payload,
 * contagem de entrega e de perda por lacuna de sequência, fim do fluxo por
 * silêncio) sem a camada SRTP. Ver c3_plain_client.c para o racional.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "as_metrics.h"
#include "as_pack.h"
#include "as_profile.h"
#include "as_signal.h"

#define AS_THIS_CHANNEL AS_CH_C3_MEDIA
#define RTP_HDR_LEN     12
#define C3_BUF_MAX      (RTP_HDR_LEN + 4096)
#define IDLE_TIMEOUT_S  5

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida] [-k]\n"
        "  Baseline SEM segurança do canal C3 (RTP sobre UDP puro).\n", prog);
}

/* Um pacote RTP começa com V=2 (bits altos do 1o byte em 128..191). */
static int is_rtp(const unsigned char *p, size_t n)
{
    return n >= RTP_HDR_LEN && p[0] >= 128 && p[0] <= 191;
}

int main(int argc, char **argv)
{
    const char *out_dir = "results";
    int port, keep_running = 0, opt, rc = 1, fd = -1, one = 1;
    struct sockaddr_in addr;
    struct timeval tv;
    unsigned char pkt[C3_BUF_MAX];
    as_profile_t profile;
    as_metrics_t *m = NULL;
    uint32_t expected = 0, received = 0;
    int first = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c3_plain";
    profile.profile_name = "RTP sobre UDP puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "p:o:kh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'k': keep_running = 1; break;
        default:  usage(argv[0]); return 1;
        }
    }
    (void)keep_running;
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

    tv.tv_sec = IDLE_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!g_stop) {
        ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
        uint32_t seq;
        uint64_t sent_ns, now;

        if (n < 0)
            break;   /* silêncio por IDLE_TIMEOUT_S: fluxo terminou */
        if (!is_rtp(pkt, (size_t)n))
            continue;

        as_metrics_wire_rx(m, (uint64_t)n, 1);
        now = as_mono_ns();

        if (n < RTP_HDR_LEN + AS_HDR_LEN)
            continue;

        seq = as_get_u32(pkt + RTP_HDR_LEN);
        sent_ns = as_get_u64(pkt + RTP_HDR_LEN + 4);

        if (first) {
            first = 0;
        } else if (seq > expected) {
            uint32_t gap;
            for (gap = expected; gap < seq; gap++)
                as_metrics_lost(m, gap);
        }
        expected = seq + 1;
        received++;

        as_metrics_recv(m, seq, (size_t)(n - RTP_HDR_LEN), 0);
        if (now > sent_ns)
            as_metrics_rtt(m, seq, now - sent_ns);
    }

    printf("Fluxo encerrado: %u pacotes recebidos.\n", received);
    rc = 0;

out:
    if (m != NULL) as_metrics_close(m);
    if (fd >= 0) close(fd);
    return rc;
}
