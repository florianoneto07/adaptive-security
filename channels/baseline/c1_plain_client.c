/*
 * Baseline C1 - controle SEM segurança (P1.3). UDP puro, sem DTLS.
 *
 * Mede o mesmo gerador do canal C1 (128 B a 64 Hz, cabeçalho seq+timestamp,
 * eco para RTT) trocando a camada DTLS por sendto/recvfrom crus. Serve para
 * isolar o custo do perfil de segurança: a diferença entre este baseline e o
 * c1_control é o que o DTLS 1.3 acrescenta em estabelecimento, sobrecarga de
 * bytes, atraso e CPU.
 *
 * NÃO é um caminho de fallback do canal seguro (R2 preservado): é um binário
 * separado, de medição. O canal C1 continua DTLS-only, sem alternativa em
 * tempo de execução.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>

#include "adaptive_security.h"
#include "as_metrics.h"
#include "as_pack.h"
#include "as_profile.h"

#define AS_THIS_CHANNEL AS_CH_C1_CONTROL

#define DEFAULT_HOST     "127.0.0.1"
#define DEFAULT_MSG_LEN  128
#define DEFAULT_RATE_HZ  64
#define DEFAULT_SECONDS  60
#define MAX_MSG 2048

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-H host] [-p porta] [-l bytes] [-r hz] [-d seg] [-o dir]\n"
        "  Baseline SEM segurança do canal C1 (UDP puro). Mesmos parâmetros do\n"
        "  c1_control, para comparar o custo do DTLS 1.3.\n",
        prog);
}

/* Consome todos os ecos já disponíveis e registra o RTT de cada um. */
static void drain_echoes(int fd, as_metrics_t *m, unsigned char *buf,
                         size_t buf_len, uint64_t *rtt_count)
{
    for (;;) {
        ssize_t n = recv(fd, buf, buf_len, 0);

        if (n >= AS_HDR_LEN) {
            uint32_t seq = as_get_u32(buf);
            uint64_t sent_ns = as_get_u64(buf + 4);
            uint64_t now = as_mono_ns();

            as_metrics_wire_rx(m, (uint64_t)n, 1);
            if (now > sent_ns) {
                as_metrics_rtt(m, seq, now - sent_ns);
                (*rtt_count)++;
            }
            continue;
        }
        if (n > 0)
            continue;   /* eco truncado: ignora */
        return;         /* EAGAIN/EWOULDBLOCK ou 0: fila vazia */
    }
}

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST;
    const char *out_dir = "results";
    int port, opt, rc = 1, fd = -1;
    size_t msg_len = DEFAULT_MSG_LEN;
    long rate_hz = DEFAULT_RATE_HZ, seconds = DEFAULT_SECONDS;
    struct sockaddr_in peer;
    unsigned char msg[MAX_MSG], echo[MAX_MSG];
    as_profile_t profile;
    as_metrics_t *m = NULL;
    uint64_t t_start, period_ns, deadline, next_ns, rtt_count = 0;
    uint32_t seq = 0;
    struct timespec next;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c1_plain";
    profile.profile_name = "UDP puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "H:p:l:r:d:o:h")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'l': msg_len = (size_t)strtoul(optarg, NULL, 10); break;
        case 'r': rate_hz = strtol(optarg, NULL, 10); break;
        case 'd': seconds = strtol(optarg, NULL, 10); break;
        case 'o': out_dir = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (port <= 0 || port > 65535) { fprintf(stderr, "Porta inválida: %d\n", port); return 1; }
    if (msg_len < AS_HDR_LEN || msg_len > MAX_MSG) {
        fprintf(stderr, "Tamanho fora de [%d, %d]: %zu\n", AS_HDR_LEN, MAX_MSG, msg_len);
        return 1;
    }
    if (rate_hz <= 0 || seconds <= 0) { fprintf(stderr, "Taxa e duração positivas.\n"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("Baseline %s (%s)\nSem segurança: %s, UDP/%d\n"
           "Gerador: %zu B a %ld Hz por %ld s (%.1f kbps de payload)\n",
           profile.slug, profile.app_class, profile.profile_name, port,
           msg_len, rate_hz, seconds, (double)msg_len * rate_hz * 8 / 1000.0);

    m = as_metrics_open(&profile, "client", out_dir);
    if (m == NULL)
        goto out;

    if (as_resolve_v4(host, port, SOCK_DGRAM, &peer) != 0)
        goto out;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); goto out; }
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) { perror("connect"); goto out; }
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) { perror("fcntl"); goto out; }

    /* UDP puro não tem estabelecimento: registrar zero torna a coluna
     * comparável com o handshake do canal seguro — a ausência é o resultado. */
    as_metrics_handshake(m, 0, 0);

    memset(msg, 0xA5, sizeof(msg));
    period_ns = 1000000000ULL / (uint64_t)rate_hz;
    deadline = as_mono_ns() + (uint64_t)seconds * 1000000000ULL;
    (void)t_start;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop && as_mono_ns() < deadline) {
        uint64_t cpu0;
        ssize_t sent;

        as_hdr_put(msg, seq, as_mono_ns());

        /* Janela de CPU só em volta do envio, como no canal seguro. */
        cpu0 = as_cpu_ns();
        sent = send(fd, msg, msg_len, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "Falha ao enviar %u: %s\n", seq, strerror(errno));
            goto out;
        }
        as_metrics_msg(m, seq, 0, (size_t)sent, as_cpu_ns() - cpu0);
        as_metrics_wire_tx(m, (uint64_t)sent, 1);
        seq++;

        next.tv_nsec += (long)period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        next_ns = (uint64_t)next.tv_sec * 1000000000ULL + (uint64_t)next.tv_nsec;

        while (!g_stop) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            struct timespec wait;
            uint64_t now = as_mono_ns();
            int pr;

            if (now >= next_ns) break;
            wait.tv_sec  = (time_t)((next_ns - now) / 1000000000ULL);
            wait.tv_nsec = (long)((next_ns - now) % 1000000000ULL);
            pr = ppoll(&pfd, 1, &wait, NULL);
            if (pr > 0) drain_echoes(fd, m, echo, sizeof(echo), &rtt_count);
            else if (pr == 0) break;
            else if (errno != EINTR) break;
        }
    }

    { struct timespec grace = { 0, 200000000L };
      nanosleep(&grace, NULL);
      drain_echoes(fd, m, echo, sizeof(echo), &rtt_count); }

    printf("Enviadas %u mensagens, %llu ecos com RTT medido.\n",
           seq, (unsigned long long)rtt_count);
    rc = 0;

out:
    if (m != NULL) as_metrics_close(m);
    if (fd >= 0) close(fd);
    return rc;
}
