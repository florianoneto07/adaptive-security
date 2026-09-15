/*
 * Baseline C2 - telemetria SEM segurança (P1.3). CoAP puro, sem OSCORE.
 *
 * Espelha o c2_server (recurso /telemetry que responde 2 B, retransmissão
 * nativa do CoAP contabilizada) sem o contexto OSCORE. Ver c2_plain_client.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <coap3/coap.h>

#include "as_metrics.h"
#include "as_profile.h"
#include "as_signal.h"

#define AS_THIS_CHANNEL AS_CH_C2_TELEMETRY
#define C2_PAYLOAD_LEN 2
#define C2_RESOURCE    "telemetry"

static volatile sig_atomic_t g_stop;
static as_metrics_t *g_metrics;
static uint64_t g_seq;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida] [-v nivel]\n"
        "  Baseline SEM segurança do canal C2 (CoAP puro).\n", prog);
}

static void hnd_get_telemetry(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *request,
                              const coap_string_t *query, coap_pdu_t *response)
{
    unsigned char payload[C2_PAYLOAD_LEN];
    uint16_t value = (uint16_t)(g_seq & 0xFFFF);

    (void)resource; (void)session; (void)request; (void)query;
    payload[0] = (unsigned char)(value >> 8);
    payload[1] = (unsigned char)value;
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data(response, sizeof(payload), payload);
    as_metrics_recv(g_metrics, g_seq, sizeof(payload), 0);
    g_seq++;
}

static int event_handler(coap_session_t *session, const coap_event_t event)
{
    (void)session;
    if (event == COAP_EVENT_MSG_RETRANSMITTED)
        as_metrics_retx(g_metrics, g_seq, C2_PAYLOAD_LEN, 0);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out_dir = "results";
    coap_log_t log_level = COAP_LOG_WARN;
    int port, opt, rc = 1;
    char port_str[16];
    coap_context_t *ctx = NULL;
    coap_resource_t *resource = NULL;
    coap_addr_info_t *info = NULL;
    coap_str_const_t bind_addr;
    as_profile_t profile;

    setvbuf(stdout, NULL, _IOLBF, 0);
    profile = *as_profile_get(AS_THIS_CHANNEL);
    profile.slug = "c2_plain";
    profile.profile_name = "CoAP confirmável puro (sem segurança)";
    port = profile.port;

    while ((opt = getopt(argc, argv, "p:o:v:h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'v': log_level = (coap_log_t)atoi(optarg); break;
        default:  usage(argv[0]); return 1;
        }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "Porta inválida: %d\n", port); return 1; }
    if (as_install_stop_handler(on_signal) != 0) { perror("sigaction"); return 1; }

    printf("Baseline %s (%s)\nSem segurança: %s, UDP/%d\n",
           profile.slug, profile.app_class, profile.profile_name, port);

    coap_startup();
    coap_set_log_level(log_level);

    g_metrics = as_metrics_open(&profile, "server", out_dir);
    if (g_metrics == NULL)
        goto out;

    ctx = coap_new_context(NULL);
    if (ctx == NULL) { fprintf(stderr, "Erro ao criar o contexto CoAP.\n"); goto out; }
    coap_register_event_handler(ctx, event_handler);

    snprintf(port_str, sizeof(port_str), "%d", port);
    bind_addr.s = (const uint8_t *)"0.0.0.0";
    bind_addr.length = strlen("0.0.0.0");
    info = coap_resolve_address_info(&bind_addr, (uint16_t)port, (uint16_t)port,
                                     (uint16_t)port, (uint16_t)port,
                                     0, 1 << COAP_URI_SCHEME_COAP,
                                     COAP_RESOLVE_TYPE_LOCAL);
    if (info == NULL) { fprintf(stderr, "Não foi possível resolver o endereço local.\n"); goto out; }
    if (coap_new_endpoint(ctx, &info->addr, COAP_PROTO_UDP) == NULL) {
        fprintf(stderr, "Erro ao criar o endpoint UDP na porta %d.\n", port); goto out;
    }

    resource = coap_resource_init(coap_make_str_const(C2_RESOURCE), 0);
    if (resource == NULL) { fprintf(stderr, "Erro ao criar o recurso /%s.\n", C2_RESOURCE); goto out; }
    coap_register_request_handler(resource, COAP_REQUEST_GET, hnd_get_telemetry);
    coap_add_resource(ctx, resource);

    printf("Canal %s aguardando na porta %d, recurso /%s\n",
           profile.slug, port, C2_RESOURCE);

    while (!g_stop) {
        int res = coap_io_process(ctx, 1000);
        if (res < 0)
            break;
    }

    printf("Servidor encerrado após %llu leituras.\n", (unsigned long long)g_seq);
    rc = 0;

out:
    if (g_metrics != NULL) as_metrics_close(g_metrics);
    if (info != NULL) coap_free_address_info(info);
    if (ctx != NULL) coap_free_context(ctx);
    coap_cleanup();
    return rc;
}
