/*
 * Baseline C4 - transferência volumosa SEM segurança (P1.3). TCP puro, sem TLS.
 *
 * Mesmo protocolo de aplicação do c4_bulk (cabeçalho de 8 B com o tamanho,
 * blocos, confirmação de 8 B) trocando TLS 1.3 por send/recv crus. A diferença
 * entre este baseline e o c4_bulk é o custo do TLS 1.3 em estabelecimento,
 * vazão e CPU. Não é fallback do canal seguro (R2 preservado).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include "adaptive_security.h"
#include "as_metrics.h"
#include "as_profile.h"

#define AS_THIS_CHANNEL AS_CH_C4_BULK
#define DEFAULT_HOST     "127.0.0.1"
#define DEFAULT_SIZE_MIB 32
#define DEFAULT_BLOCK    16384

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-H host] [-p porta] [-s tamanho_MiB] [-b bloco] [-o dir]\n"
        "  Baseline SEM segurança do canal C4 (TCP puro).\n", prog);
}

/* Envia exatamente n bytes, lidando com escritas parciais do TCP. */
static int send_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int recv_all(int fd, unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST;
    const char *out_dir = "results";
    int port, opt, rc = 1, sockfd = -1;
    size_t block = DEFAULT_BLOCK;
    uint64_t total = (uint64_t)DEFAULT_SIZE_MIB * 1024 * 1024;
    uint64_t sent = 0, seq = 0, t_first_byte;
    struct sockaddr_in server_addr;
    unsigned char *buf = NULL, hdr[8];
    as_profile_t profile;
    as_metrics_t *m = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c4_plain";
    profile.profile_name = "TCP puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "H:p:s:b:o:h")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 's': total = strtoull(optarg, NULL, 10) * 1024 * 1024; break;
        case 'b': block = (size_t)strtoul(optarg, NULL, 10); break;
        case 'o': out_dir = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "Porta inválida: %d\n", port); return 1; }
    if (block == 0 || total == 0) { fprintf(stderr, "Tamanho ou bloco inválido.\n"); return 1; }

    printf("Baseline %s (%s)\nSem segurança: %s, TCP/%d\n",
           profile.slug, profile.app_class, profile.profile_name, port);

    buf = malloc(block);
    if (buf == NULL) { fprintf(stderr, "Sem memória para o bloco.\n"); goto out; }
    for (size_t i = 0; i < block; i++)
        buf[i] = (unsigned char)(i * 31 + 7);

    m = as_metrics_open(&profile, "client", out_dir);
    if (m == NULL)
        goto out;

    if (as_resolve_v4(host, port, SOCK_STREAM, &server_addr) != 0)
        goto out;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); goto out; }
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect"); goto out;
    }

    /* TCP puro não tem handshake de segurança: estabelecimento zero. */
    as_metrics_handshake(m, 0, 0);
    t_first_byte = as_mono_ns();
    printf("Transferindo %llu bytes...\n", (unsigned long long)total);

    for (int i = 7; i >= 0; i--)
        hdr[i] = (unsigned char)((total >> ((7 - i) * 8)) & 0xFF);
    if (send_all(sockfd, hdr, sizeof(hdr)) != 0) {
        fprintf(stderr, "Falha ao enviar o cabeçalho.\n"); goto out;
    }
    as_metrics_wire_tx(m, sizeof(hdr), 1);

    while (sent < total) {
        size_t want = total - sent;
        uint64_t cpu0;
        if (want > block) want = block;
        cpu0 = as_cpu_ns();
        if (send_all(sockfd, buf, want) != 0) {
            fprintf(stderr, "Falha após %llu bytes: %s\n",
                    (unsigned long long)sent, strerror(errno));
            goto out;
        }
        as_metrics_msg(m, seq++, 0, want, as_cpu_ns() - cpu0);
        as_metrics_wire_tx(m, (uint64_t)want, 1);
        sent += want;
    }

    if (recv_all(sockfd, hdr, sizeof(hdr)) != 0) {
        fprintf(stderr, "Confirmação não recebida.\n"); goto out;
    }
    {
        uint64_t acked = 0, elapsed = as_mono_ns() - t_first_byte;
        for (int i = 0; i < 8; i++) acked = (acked << 8) | hdr[i];
        as_metrics_msg(m, seq, elapsed, 0, 0);
        as_metrics_wire_rx(m, sizeof(hdr), 1);
        printf("Confirmados %llu de %llu bytes em %.3f s (%.2f Mbps).\n",
               (unsigned long long)acked, (unsigned long long)total, elapsed / 1e9,
               elapsed > 0 ? (double)total * 8.0 / (double)elapsed * 1000.0 : 0.0);
        if (acked != total) { fprintf(stderr, "Volume confirmado difere.\n"); goto out; }
    }
    rc = 0;

out:
    if (m != NULL) as_metrics_close(m);
    free(buf);
    if (sockfd >= 0) close(sockfd);
    return rc;
}
