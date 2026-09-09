#define _POSIX_C_SOURCE 200809L

#include "as_metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <wolfssl/wolfio.h>

/* ------------------------------------------------------------------------- */
/* Estado                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Contexto de I/O da instrumentação.
 *
 * Guarda os contextos que o wolfSSL havia instalado, porque cada rotina Embed
 * espera um tipo diferente ali: EmbedSend faz *(int*)ctx, enquanto EmbedSendTo
 * faz (WOLFSSL_DTLS_CTX*)ctx, que carrega o peer além dos descritores. Fabricar
 * um contexto próprio funciona em TLS e produz leitura fora dos limites em
 * DTLS. Repassar o original é o que vale para os dois.
 */
typedef struct {
    void         *orig_read_ctx;
    void         *orig_write_ctx;
    int           is_dtls;
    as_metrics_t *m;
} as_wire_ctx_t;

struct as_metrics {
    FILE      *csv;
    char       summary_path[512];

    const as_profile_t *profile;
    const char *role;

    /* Amostras de RTT, para os percentis. Cresce sob demanda. */
    uint64_t  *rtt;
    size_t     rtt_n;
    size_t     rtt_cap;

    uint64_t   t0;              /* origem monotônica da execução */

    uint64_t   handshake_wall_ns;
    uint64_t   handshake_cpu_ns;
    uint64_t   handshake_wire_tx;   /* fio consumido até o handshake fechar */
    uint64_t   handshake_wire_rx;

    uint64_t   msgs_sent;
    uint64_t   msgs_recv;
    uint64_t   msgs_retx;
    uint64_t   msgs_lost;

    uint64_t   app_bytes_tx;
    uint64_t   app_bytes_rx;

    uint64_t   cpu_first_ns;    /* CPU somada nos envios iniciais    */
    uint64_t   cpu_retx_ns;     /* CPU somada nas retransmissões     */
    uint64_t   cpu_verify_ns;   /* CPU somada ao verificar recebidas */

    uint64_t   wire_tx_bytes;
    uint64_t   wire_rx_bytes;
    uint64_t   wire_tx_pkts;
    uint64_t   wire_rx_pkts;

    as_wire_ctx_t wire;
    int        wire_instrumented;   /* as_wire_attach() foi chamado?          */
};

/* ------------------------------------------------------------------------- */
/* Relógios                                                                   */
/* ------------------------------------------------------------------------- */

static uint64_t clock_ns(clockid_t id)
{
    struct timespec ts;

    if (clock_gettime(id, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t as_mono_ns(void) { return clock_ns(CLOCK_MONOTONIC); }
uint64_t as_cpu_ns(void)  { return clock_ns(CLOCK_PROCESS_CPUTIME_ID); }

uint64_t as_rss_peak_kb(void)
{
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    uint64_t kb = 0;

    if (f == NULL)
        return 0;

    /*
     * VmHWM, e não VmRSS: o pico é o número comparável entre canais, porque
     * VmRSS depende de quando exatamente a leitura acontece.
     */
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            sscanf(line + 6, "%llu", (unsigned long long *)&kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

/* ------------------------------------------------------------------------- */
/* Ciclo de vida                                                              */
/* ------------------------------------------------------------------------- */

as_metrics_t *as_metrics_open(const as_profile_t *profile, const char *role,
                              const char *out_dir)
{
    as_metrics_t *m;
    char csv_path[512];

    if (profile == NULL || role == NULL) {
        fprintf(stderr, "as_metrics_open: argumento nulo.\n");
        return NULL;
    }
    if (out_dir == NULL || out_dir[0] == '\0')
        out_dir = ".";

    m = calloc(1, sizeof(*m));
    if (m == NULL) {
        fprintf(stderr, "as_metrics_open: sem memória.\n");
        return NULL;
    }

    m->profile = profile;
    m->role = role;
    m->t0 = as_mono_ns();

    if (snprintf(csv_path, sizeof(csv_path), "%s/%s_%s.csv",
                 out_dir, profile->slug, role) >= (int)sizeof(csv_path) ||
        snprintf(m->summary_path, sizeof(m->summary_path), "%s/%s_%s.summary",
                 out_dir, profile->slug, role) >= (int)sizeof(m->summary_path)) {
        fprintf(stderr, "Caminho de saída longo demais em %s\n", out_dir);
        free(m);
        return NULL;
    }

    m->csv = fopen(csv_path, "w");
    if (m->csv == NULL) {
        fprintf(stderr, "Não foi possível abrir %s: %s\n",
                csv_path, strerror(errno));
        free(m);
        return NULL;
    }

    /*
     * Linha a linha: uma execução interrompida no meio ainda deixa um CSV
     * legível até o ponto da interrupção, em vez de um arquivo truncado no
     * meio de um registro.
     */
    setvbuf(m->csv, NULL, _IOLBF, 0);

    fprintf(m->csv, "event,seq,t_rel_ns,rtt_ns,app_bytes,cpu_ns\n");
    return m;
}

/* Acrescenta uma amostra de RTT ao vetor, dobrando a capacidade quando enche. */
static void rtt_push(as_metrics_t *m, uint64_t v)
{
    if (m->rtt_n == m->rtt_cap) {
        size_t cap = (m->rtt_cap == 0) ? 4096 : m->rtt_cap * 2;
        uint64_t *p = realloc(m->rtt, cap * sizeof(*p));

        if (p == NULL)
            return;   /* perde a amostra, mas não derruba o experimento */
        m->rtt = p;
        m->rtt_cap = cap;
    }
    m->rtt[m->rtt_n++] = v;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

/*
 * Percentil por interpolação de posto mais próximo (nearest-rank), o método
 * usual em medição de latência: p95 é o menor valor que 95% das amostras não
 * ultrapassam. Exige o vetor já ordenado.
 */
static uint64_t percentile(const uint64_t *sorted, size_t n, double p)
{
    size_t idx;

    if (n == 0)
        return 0;
    idx = (size_t)((p / 100.0) * (double)n);
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

/* ------------------------------------------------------------------------- */
/* Eventos                                                                    */
/* ------------------------------------------------------------------------- */

static void row(as_metrics_t *m, const char *event, uint64_t seq,
                uint64_t rtt_ns, size_t app_bytes, uint64_t cpu_ns)
{
    fprintf(m->csv, "%s,%llu,%llu,%llu,%zu,%llu\n",
            event,
            (unsigned long long)seq,
            (unsigned long long)(as_mono_ns() - m->t0),
            (unsigned long long)rtt_ns,
            app_bytes,
            (unsigned long long)cpu_ns);
}

void as_metrics_handshake(as_metrics_t *m, uint64_t wall_ns, uint64_t cpu_ns)
{
    if (m == NULL)
        return;

    m->handshake_wall_ns = wall_ns;
    m->handshake_cpu_ns  = cpu_ns;
    m->handshake_wire_tx = m->wire_tx_bytes;
    m->handshake_wire_rx = m->wire_rx_bytes;

    row(m, "handshake", 0, wall_ns, 0, cpu_ns);
}

void as_metrics_msg(as_metrics_t *m, uint64_t seq, uint64_t rtt_ns,
                    size_t app_bytes, uint64_t cpu_ns)
{
    if (m == NULL)
        return;

    m->msgs_sent++;
    m->app_bytes_tx += app_bytes;
    m->cpu_first_ns += cpu_ns;
    if (rtt_ns > 0)
        rtt_push(m, rtt_ns);

    row(m, "msg", seq, rtt_ns, app_bytes, cpu_ns);
}

void as_metrics_retx(as_metrics_t *m, uint64_t seq, size_t app_bytes,
                     uint64_t cpu_ns)
{
    if (m == NULL)
        return;

    m->msgs_retx++;
    m->cpu_retx_ns += cpu_ns;

    row(m, "retx", seq, 0, app_bytes, cpu_ns);
}

void as_metrics_rtt(as_metrics_t *m, uint64_t seq, uint64_t rtt_ns)
{
    if (m == NULL || rtt_ns == 0)
        return;

    rtt_push(m, rtt_ns);
    row(m, "rtt", seq, rtt_ns, 0, 0);
}

void as_metrics_lost(as_metrics_t *m, uint64_t seq)
{
    if (m == NULL)
        return;

    m->msgs_lost++;
    row(m, "lost", seq, 0, 0, 0);
}

void as_metrics_recv(as_metrics_t *m, uint64_t seq, size_t app_bytes,
                     uint64_t cpu_ns)
{
    if (m == NULL)
        return;

    m->msgs_recv++;
    m->app_bytes_rx += app_bytes;
    m->cpu_verify_ns += cpu_ns;
    row(m, "recv", seq, 0, app_bytes, cpu_ns);
}

/* ------------------------------------------------------------------------- */
/* Bytes no fio                                                               */
/* ------------------------------------------------------------------------- */

static int wire_send(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    as_wire_ctx_t *w = (as_wire_ctx_t *)ctx;
    int n;

    /*
     * O wolfSSL usa EmbedSendTo para DTLS e EmbedSend para TLS. Delegamos ao
     * mesmo que ele usaria e só contamos o resultado, para que interceptar não
     * mude o comportamento do transporte.
     */
    n = w->is_dtls ? EmbedSendTo(ssl, buf, sz, w->orig_write_ctx)
                   : EmbedSend(ssl, buf, sz, w->orig_write_ctx);
    if (n > 0) {
        w->m->wire_tx_bytes += (uint64_t)n;
        w->m->wire_tx_pkts++;
    }
    return n;
}

static int wire_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    as_wire_ctx_t *w = (as_wire_ctx_t *)ctx;
    int n;

    n = w->is_dtls ? EmbedReceiveFrom(ssl, buf, sz, w->orig_read_ctx)
                   : EmbedReceive(ssl, buf, sz, w->orig_read_ctx);
    if (n > 0) {
        w->m->wire_rx_bytes += (uint64_t)n;
        w->m->wire_rx_pkts++;
    }
    return n;
}

void as_metrics_wire_tx(as_metrics_t *m, uint64_t bytes, uint64_t pkts)
{
    if (m == NULL)
        return;
    m->wire_tx_bytes += bytes;
    m->wire_tx_pkts  += pkts;
    m->wire_instrumented = 1;
}

void as_metrics_wire_rx(as_metrics_t *m, uint64_t bytes, uint64_t pkts)
{
    if (m == NULL)
        return;
    m->wire_rx_bytes += bytes;
    m->wire_rx_pkts  += pkts;
    m->wire_instrumented = 1;
}

int as_wire_attach(WOLFSSL *ssl, as_metrics_t *m, int is_dtls)
{
    if (ssl == NULL || m == NULL) {
        fprintf(stderr, "as_wire_attach: argumento nulo.\n");
        return -1;
    }

    /*
     * Captura o que o wolfSSL instalou. Em DTLS isso só existe depois de
     * wolfSSL_dtls_set_peer(), que é quem preenche o WOLFSSL_DTLS_CTX; chamar
     * antes guardaria um contexto sem peer e o envio não teria destino.
     */
    m->wire.orig_read_ctx  = wolfSSL_GetIOReadCtx(ssl);
    m->wire.orig_write_ctx = wolfSSL_GetIOWriteCtx(ssl);
    m->wire.is_dtls = is_dtls;
    m->wire.m = m;
    m->wire_instrumented = 1;

    if (m->wire.orig_read_ctx == NULL || m->wire.orig_write_ctx == NULL) {
        fprintf(stderr,
                "as_wire_attach: contexto de I/O ainda não definido. Chame "
                "depois de wolfSSL_set_fd() e, em DTLS, de "
                "wolfSSL_dtls_set_peer().\n");
        return -1;
    }

    /* Na sessão: wolfSSL_new() já copiou os callbacks do contexto. */
    wolfSSL_SSLSetIOSend(ssl, wire_send);
    wolfSSL_SSLSetIORecv(ssl, wire_recv);

    wolfSSL_SetIOWriteCtx(ssl, &m->wire);
    wolfSSL_SetIOReadCtx(ssl, &m->wire);

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Fechamento                                                                 */
/* ------------------------------------------------------------------------- */

int as_metrics_close(as_metrics_t *m)
{
    FILE *s;
    uint64_t ip_overhead;
    int rc = 0;

    if (m == NULL)
        return -1;

    if (m->rtt_n > 0)
        qsort(m->rtt, m->rtt_n, sizeof(m->rtt[0]), cmp_u64);

    s = fopen(m->summary_path, "w");
    if (s == NULL) {
        fprintf(stderr, "Não foi possível escrever %s: %s\n",
                m->summary_path, strerror(errno));
        rc = -1;
    } else {
        /*
         * Cabeçalhos de rede que o processo não enxerga: 20 B de IPv4 mais
         * 8 B de UDP ou 20 B de TCP. Somados por pacote, e apresentados à
         * parte para que quem ler saiba que são calculados, e não medidos —
         * a captura em pcap é a medida direta.
         */
        ip_overhead = (m->profile->socktype == SOCK_DGRAM ? 28 : 40) *
                      (m->wire_tx_pkts + m->wire_rx_pkts);

        fprintf(s,
            "channel=%s\n"
            "app_class=%s\n"
            "profile=%s\n"
            "role=%s\n"
            "port=%u\n"
            "duration_ns=%llu\n"
            "handshake_wall_ns=%llu\n"
            "handshake_cpu_ns=%llu\n"
            "handshake_wire_tx_bytes=%llu\n"
            "handshake_wire_rx_bytes=%llu\n"
            "msgs_sent=%llu\n"
            "msgs_recv=%llu\n"
            "msgs_retx=%llu\n"
            "msgs_lost=%llu\n"
            "app_bytes_tx=%llu\n"
            "app_bytes_rx=%llu\n"
            "wire_tx_bytes=%llu\n"
            "wire_rx_bytes=%llu\n"
            "wire_tx_pkts=%llu\n"
            "wire_rx_pkts=%llu\n"
            "ip_udp_tcp_overhead_bytes=%llu\n"
            "wire_instrumented=%d\n"
            "cpu_first_send_ns=%llu\n"
            "cpu_retransmit_ns=%llu\n"
            "cpu_verify_ns=%llu\n"
            "rtt_samples=%zu\n"
            "rtt_p50_ns=%llu\n"
            "rtt_p95_ns=%llu\n"
            "rtt_p99_ns=%llu\n"
            "rss_peak_kb=%llu\n",
            m->profile->slug,
            m->profile->app_class,
            m->profile->profile_name,
            m->role,
            (unsigned)m->profile->port,
            (unsigned long long)(as_mono_ns() - m->t0),
            (unsigned long long)m->handshake_wall_ns,
            (unsigned long long)m->handshake_cpu_ns,
            (unsigned long long)m->handshake_wire_tx,
            (unsigned long long)m->handshake_wire_rx,
            (unsigned long long)m->msgs_sent,
            (unsigned long long)m->msgs_recv,
            (unsigned long long)m->msgs_retx,
            (unsigned long long)m->msgs_lost,
            (unsigned long long)m->app_bytes_tx,
            (unsigned long long)m->app_bytes_rx,
            (unsigned long long)m->wire_tx_bytes,
            (unsigned long long)m->wire_rx_bytes,
            (unsigned long long)m->wire_tx_pkts,
            (unsigned long long)m->wire_rx_pkts,
            (unsigned long long)ip_overhead,
            m->wire_instrumented,
            (unsigned long long)m->cpu_first_ns,
            (unsigned long long)m->cpu_retx_ns,
            (unsigned long long)m->cpu_verify_ns,
            m->rtt_n,
            (unsigned long long)percentile(m->rtt, m->rtt_n, 50.0),
            (unsigned long long)percentile(m->rtt, m->rtt_n, 95.0),
            (unsigned long long)percentile(m->rtt, m->rtt_n, 99.0),
            (unsigned long long)as_rss_peak_kb());

        fclose(s);
    }

    if (m->csv != NULL)
        fclose(m->csv);
    free(m->rtt);
    free(m);
    return rc;
}
