/*
 * Baseline C3 - mídia em tempo real SEM segurança (P1.3). RTP sobre UDP puro,
 * sem handshake DTLS e sem SRTP.
 *
 * Mesmo gerador do c3_media (fluxo RTP sintético, 4 Mbps em pacotes de 1200 B,
 * marca de tempo do emissor no payload para o atraso unidirecional) sem a
 * proteção SRTP nem o handshake DTLS que deriva a chave. A diferença entre este
 * baseline e o c3_media é o custo do DTLS-SRTP. Não é fallback (R2 preservado).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>

#include "adaptive_security.h"
#include "as_metrics.h"
#include "as_pack.h"
#include "as_profile.h"
#include "as_signal.h"

#define AS_THIS_CHANNEL AS_CH_C3_MEDIA
#define DEFAULT_HOST     "127.0.0.1"
#define DEFAULT_KBPS     4000
#define DEFAULT_PAYLOAD  1200
#define DEFAULT_SECONDS  60
#define DEFAULT_PT       96
#define DEFAULT_SSRC     0x0C3ACAFEu
#define RTP_HDR_LEN      12          /* RFC 3550, seção 5.1 (igual ao c3_media) */
#define C3_BUF_MAX       (RTP_HDR_LEN + 4096)

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-H host] [-p porta] [-b kbps] [-l bytes] [-d seg] [-o dir]\n"
        "  Baseline SEM segurança do canal C3 (RTP sobre UDP puro).\n", prog);
}

static void rtp_header(unsigned char *p, uint16_t seq, uint32_t ts,
                       uint32_t ssrc, unsigned pt)
{
    p[0] = 0x80;
    p[1] = (unsigned char)(pt & 0x7F);
    p[2] = (unsigned char)(seq >> 8);
    p[3] = (unsigned char)seq;
    as_put_u32(p + 4, ts);
    as_put_u32(p + 8, ssrc);
}

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST;
    const char *out_dir = "results";
    int port, opt, rc = 1, fd = -1;
    long kbps = DEFAULT_KBPS, payload = DEFAULT_PAYLOAD, seconds = DEFAULT_SECONDS;
    struct sockaddr_in peer;
    unsigned char pkt[C3_BUF_MAX];
    as_profile_t profile;
    as_metrics_t *m = NULL;
    uint64_t period_ns, deadline, next_ns;
    uint32_t seq = 0;
    struct timespec next;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c3_plain";
    profile.profile_name = "RTP sobre UDP puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "H:p:b:l:d:o:h")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'b': kbps = strtol(optarg, NULL, 10); break;
        case 'l': payload = strtol(optarg, NULL, 10); break;
        case 'd': seconds = strtol(optarg, NULL, 10); break;
        case 'o': out_dir = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "Porta inválida: %d\n", port); return 1; }
    if (payload < AS_HDR_LEN || payload > 4096) {
        fprintf(stderr, "Payload fora de [%d, 4096]: %ld\n", AS_HDR_LEN, payload); return 1;
    }
    if (kbps <= 0 || seconds <= 0) { fprintf(stderr, "Bitrate e duração positivos.\n"); return 1; }

    if (as_install_stop_handler(on_signal) != 0) { perror("sigaction"); return 1; }

    period_ns = (uint64_t)payload * 8ULL * 1000000ULL / (uint64_t)kbps;
    if (period_ns == 0) { fprintf(stderr, "Bitrate alto demais para o pacote.\n"); return 1; }

    printf("Baseline %s (%s)\nSem segurança: %s, UDP/%d\n"
           "Gerador: %ld kbps em pacotes de %ld B (%.1f pacotes/s) por %ld s\n",
           profile.slug, profile.app_class, profile.profile_name, port,
           kbps, payload, 1e9 / (double)period_ns, seconds);

    m = as_metrics_open(&profile, "client", out_dir);
    if (m == NULL)
        goto out;

    if (as_resolve_v4(host, port, SOCK_DGRAM, &peer) != 0)
        goto out;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); goto out; }
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) { perror("connect"); goto out; }

    /* Sem handshake DTLS: estabelecimento zero. */
    as_metrics_handshake(m, 0, 0);

    memset(pkt, 0x5A, sizeof(pkt));
    deadline = as_mono_ns() + (uint64_t)seconds * 1000000000ULL;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop && as_mono_ns() < deadline) {
        int len = RTP_HDR_LEN + (int)payload;
        uint64_t cpu0, now;
        ssize_t sent;

        now = as_mono_ns();
        rtp_header(pkt, (uint16_t)seq, (uint32_t)(now / 1000), DEFAULT_SSRC, DEFAULT_PT);
        as_hdr_put(pkt + RTP_HDR_LEN, seq, now);

        /* Janela de CPU vazia de cripto (sem SRTP): mede só a montagem do
         * pacote, para a coluna ser comparável com o srtp_protect do canal. */
        cpu0 = as_cpu_ns();
        sent = send(fd, pkt, (size_t)len, 0);
        if (sent < 0) { perror("send"); goto out; }
        as_metrics_msg(m, seq, 0, (size_t)payload, as_cpu_ns() - cpu0);
        as_metrics_wire_tx(m, (uint64_t)sent, 1);
        seq++;

        next.tv_nsec += (long)period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        next_ns = (uint64_t)next.tv_sec * 1000000000ULL + (uint64_t)next.tv_nsec;
        if (next_ns > as_mono_ns())
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    printf("Enviados %u pacotes RTP em claro.\n", seq);
    rc = 0;

out:
    if (m != NULL) as_metrics_close(m);
    if (fd >= 0) close(fd);
    return rc;
}
