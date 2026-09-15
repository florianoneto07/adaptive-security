/*
 * Baseline C2 - telemetria SEM segurança (P1.3). CoAP confirmável puro, sem
 * OSCORE.
 *
 * Mesmo gerador do c2_telemetry (GET confirmável de uma leitura de 2 B a cada
 * 2 s, com as retransmissões nativas do CoAP) sem o contexto OSCORE que protege
 * o objeto. A diferença entre este baseline e o c2_telemetry é o custo do
 * OSCORE. Não é fallback do canal seguro (R2 preservado).
 *
 * Mantém a recuperação de sessão do canal seguro: sob perda alta o libcoap 4.3.5
 * pode deixar a sessão inutilizável após esgotar as retransmissões, e sem
 * recriá-la as leituras seguintes falhariam todas.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <coap3/coap.h>

#include "as_metrics.h"
#include "as_profile.h"
#include "as_signal.h"

#define AS_THIS_CHANNEL AS_CH_C2_TELEMETRY
#define C2_RESOURCE    "telemetry"
#define C2_PAYLOAD_LEN 2
#define DEFAULT_PERIOD_MS 2000
#define DEFAULT_JITTER_MS 500
#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_SECONDS 60
#define DEFAULT_SEED    1
#define DEFAULT_RECV_TIMEOUT_MS 30000

static volatile sig_atomic_t g_stop;
static as_metrics_t *g_metrics;
static uint64_t g_seq, g_responses, g_retx, g_nacks, g_reestab;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-H host] [-p porta] [-t ms] [-j ms] [-d seg] [-S seed]\n"
        "            [-w ms] [-o dir] [-v nivel]\n"
        "  Baseline SEM segurança do canal C2 (CoAP confirmável puro).\n", prog);
}

static void nack_handler(coap_session_t *s, const coap_pdu_t *sent,
                         const coap_nack_reason_t reason, const coap_mid_t mid)
{
    (void)s; (void)sent; (void)reason; (void)mid;
    g_nacks++;
}

static int event_handler(coap_session_t *s, const coap_event_t event)
{
    (void)s;
    if (event == COAP_EVENT_MSG_RETRANSMITTED) {
        as_metrics_retx(g_metrics, g_seq, 0, 0);
        g_retx++;
    }
    return 0;
}

/* Cria contexto + sessão CoAP em claro. 0 em sucesso; -1 em erro. */
static int open_session(const coap_address_t *addr,
                        coap_context_t **ctx_out, coap_session_t **sess_out)
{
    coap_context_t *ctx;
    coap_session_t *sess;

    ctx = coap_new_context(NULL);
    if (ctx == NULL) { fprintf(stderr, "Erro ao criar o contexto CoAP.\n"); return -1; }
    coap_register_nack_handler(ctx, nack_handler);
    coap_register_event_handler(ctx, event_handler);

    sess = coap_new_client_session(ctx, NULL, addr, COAP_PROTO_UDP);
    if (sess == NULL) {
        fprintf(stderr, "Erro ao criar a sessão CoAP.\n");
        coap_free_context(ctx);
        return -1;
    }
    *ctx_out = ctx;
    *sess_out = sess;
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = DEFAULT_HOST, *out_dir = "results";
    coap_log_t log_level = COAP_LOG_WARN;
    int port, opt, rc = 1;
    long period_ms = DEFAULT_PERIOD_MS, jitter_ms = DEFAULT_JITTER_MS;
    long seconds = DEFAULT_SECONDS, recv_timeout_ms = DEFAULT_RECV_TIMEOUT_MS;
    unsigned seed = DEFAULT_SEED;
    coap_context_t *ctx = NULL;
    coap_session_t *session = NULL;
    coap_addr_info_t *info = NULL;
    coap_str_const_t server_str;
    as_profile_t profile;
    uint64_t deadline;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c2_plain";
    profile.profile_name = "CoAP confirmável puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "H:p:t:j:d:S:w:o:v:h")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 't': period_ms = strtol(optarg, NULL, 10); break;
        case 'j': jitter_ms = strtol(optarg, NULL, 10); break;
        case 'd': seconds = strtol(optarg, NULL, 10); break;
        case 'S': seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'w': recv_timeout_ms = strtol(optarg, NULL, 10); break;
        case 'o': out_dir = optarg; break;
        case 'v': log_level = (coap_log_t)atoi(optarg); break;
        default:  usage(argv[0]); return 1;
        }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "Porta inválida: %d\n", port); return 1; }
    if (period_ms <= 0 || seconds <= 0) { fprintf(stderr, "Período e duração positivos.\n"); return 1; }
    if (jitter_ms < 0 || jitter_ms >= period_ms) { fprintf(stderr, "Jitter em [0, período).\n"); return 1; }

    if (as_install_stop_handler(on_signal) != 0) { perror("sigaction"); return 1; }
    srand(seed);

    printf("Baseline %s (%s)\nSem segurança: %s, UDP/%d\n"
           "Gerador: leitura de 2 B a cada %ld +- %ld ms por %ld s\n",
           profile.slug, profile.app_class, profile.profile_name, port,
           period_ms, jitter_ms, seconds);

    coap_startup();
    coap_set_log_level(log_level);

    g_metrics = as_metrics_open(&profile, "client", out_dir);
    if (g_metrics == NULL)
        goto out;

    server_str.s = (const uint8_t *)host;
    server_str.length = strlen(host);
    info = coap_resolve_address_info(&server_str, (uint16_t)port, (uint16_t)port,
                                     (uint16_t)port, (uint16_t)port,
                                     0, 1 << COAP_URI_SCHEME_COAP,
                                     COAP_RESOLVE_TYPE_REMOTE);
    if (info == NULL) { fprintf(stderr, "Não foi possível resolver %s.\n", host); goto out; }

    if (open_session(&info->addr, &ctx, &session) != 0)
        goto out;

    /* Sem OSCORE e sem handshake: estabelecimento zero, sobrescrito pela
     * primeira transação (equivalente comparável ao do c2_telemetry). */
    as_metrics_handshake(g_metrics, 0, 0);

    deadline = as_mono_ns() + (uint64_t)seconds * 1000000000ULL;

    while (!g_stop && as_mono_ns() < deadline) {
        coap_pdu_t *pdu, *response = NULL;
        uint64_t cpu0, t_send, rtt, cpu_used;
        long jitter, sleep_ms;
        int res, reopen = 0;

        pdu = coap_new_pdu(COAP_MESSAGE_CON, COAP_REQUEST_CODE_GET, session);
        if (pdu == NULL) { fprintf(stderr, "Erro ao montar a requisição.\n"); goto out; }
        if (coap_add_option(pdu, COAP_OPTION_URI_PATH, strlen(C2_RESOURCE),
                            (const uint8_t *)C2_RESOURCE) == 0) {
            fprintf(stderr, "Erro ao acrescentar Uri-Path.\n");
            coap_delete_pdu(pdu);
            goto out;
        }

        t_send = as_mono_ns();
        cpu0 = as_cpu_ns();
        res = coap_send_recv(session, pdu, &response, (uint32_t)recv_timeout_ms);
        cpu_used = as_cpu_ns() - cpu0;
        rtt = as_mono_ns() - t_send;

        as_metrics_msg(g_metrics, g_seq, 0, C2_PAYLOAD_LEN, cpu_used);

        if (res >= 0 && response != NULL &&
            COAP_RESPONSE_CLASS(coap_pdu_get_code(response)) == 2) {
            as_metrics_rtt(g_metrics, g_seq, rtt);
            g_responses++;
            if (g_responses == 1)
                as_metrics_handshake(g_metrics, rtt, cpu_used);
        } else {
            as_metrics_lost(g_metrics, g_seq);
            if (response != NULL) {
                coap_pdu_code_t code = coap_pdu_get_code(response);
                fprintf(stderr, "Leitura %llu recusada: %d.%02d.\n",
                        (unsigned long long)g_seq,
                        COAP_RESPONSE_CLASS(code), code & 0x1F);
            } else if (res == -5) {
                fprintf(stderr, "Leitura %llu sem resposta em %ld ms.\n",
                        (unsigned long long)g_seq, recv_timeout_ms);
            } else {
                fprintf(stderr, "Leitura %llu falhou (coap_send_recv=%d).\n",
                        (unsigned long long)g_seq, res);
                if (res == -2 || res == -4)
                    reopen = 1;
            }
        }

        if (response != NULL) coap_delete_pdu(response);
        coap_delete_pdu(pdu);
        g_seq++;

        if (reopen) {
            coap_session_release(session);
            coap_free_context(ctx);
            session = NULL; ctx = NULL;
            if (open_session(&info->addr, &ctx, &session) != 0) {
                fprintf(stderr, "Não foi possível recriar a sessão; encerrando.\n");
                goto out;
            }
            g_reestab++;
        }

        jitter = (jitter_ms > 0) ? (rand() % (2 * jitter_ms + 1)) - jitter_ms : 0;
        sleep_ms = period_ms + jitter - (long)(rtt / 1000000ULL);
        while (sleep_ms > 0 && !g_stop && as_mono_ns() < deadline) {
            long slice = sleep_ms > 200 ? 200 : sleep_ms;
            struct timespec ts = { slice / 1000, (slice % 1000) * 1000000L };
            nanosleep(&ts, NULL);
            sleep_ms -= slice;
        }
    }

    printf("Enviadas %llu leituras, %llu respostas, %llu retransmissões CoAP, "
           "%llu falhas, %llu sessões recriadas.\n",
           (unsigned long long)g_seq, (unsigned long long)g_responses,
           (unsigned long long)g_retx, (unsigned long long)g_nacks,
           (unsigned long long)g_reestab);

    if (g_seq > 0 && g_responses == 0) {
        fprintf(stderr, "Baseline %s: nenhuma das %llu leituras foi respondida.\n",
                profile.slug, (unsigned long long)g_seq);
        rc = 1;
    } else {
        rc = 0;
    }

out:
    if (g_metrics != NULL) as_metrics_close(g_metrics);
    if (session != NULL) coap_session_release(session);
    if (info != NULL) coap_free_address_info(info);
    if (ctx != NULL) coap_free_context(ctx);
    coap_cleanup();
    return rc;
}
