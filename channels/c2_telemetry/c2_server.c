/*
 * Canal C2 - Telemetria periódica. Perfil fixo: OSCORE sobre CoAP, UDP/5002.
 *
 * Mensagens pequenas e periódicas, tolerantes a atraso, que podem atravessar
 * proxy ou sistema UTM. OSCORE (RFC 8613) mantém a segurança fim a fim através
 * de intermediários — TLS e DTLS não mantêm, porque são terminados no proxy e
 * a mensagem trafega em claro do outro lado.
 *
 * Não há handshake nem certificado: as duas pontas derivam o contexto de
 * segurança do Master Secret, que aqui é a chave do canal C2.
 *
 * Lado servidor: expõe o recurso de telemetria.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <coap3/coap.h>

#include "as_keys.h"
#include "as_metrics.h"
#include "as_profile.h"
#include "as_signal.h"
#include "c2_oscore.h"

#define AS_THIS_CHANNEL AS_CH_C2_TELEMETRY

/*
 * Payload de 2 bytes: é a leitura do experimento de referência
 * [Gündoğan et al., arXiv 2001.08023, §V-A]. O nome do recurso ("telemetry")
 * é padrão do testbed, não vem da literatura.
 */
#define C2_PAYLOAD_LEN 2
#define C2_RESOURCE    "telemetry"

static volatile sig_atomic_t g_stop;
static as_metrics_t *g_metrics;
static uint64_t g_seq;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida]\n"
        "  -p  porta de escuta (padrão %u)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n"
        "  -v  verbosidade do libcoap, 0 a 9 (padrão 4, WARN)\n",
        prog, as_profile_get(AS_THIS_CHANNEL)->port);
}

/*
 * Responde a leitura de telemetria com C2_PAYLOAD_LEN bytes.
 *
 * O conteúdo é um contador, não um sensor real: o que o testbed mede é o custo
 * de proteger e transportar a mensagem, não o valor lido.
 */
static void hnd_get_telemetry(coap_resource_t *resource, coap_session_t *session,
                              const coap_pdu_t *request,
                              const coap_string_t *query, coap_pdu_t *response)
{
    unsigned char payload[C2_PAYLOAD_LEN];
    uint16_t value = (uint16_t)(g_seq & 0xFFFF);

    (void)resource;
    (void)session;
    (void)request;
    (void)query;

    payload[0] = (unsigned char)(value >> 8);
    payload[1] = (unsigned char)value;

    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data(response, sizeof(payload), payload);

    /*
     * O libcoap já decifrou o pedido antes de chamar este handler, então a
     * janela de CPU da verificação não é isolável aqui. Fica 0; o custo do
     * OSCORE no C2 é medido do lado do cliente.
     */
    as_metrics_recv(g_metrics, g_seq, sizeof(payload), 0);
    g_seq++;
}

/*
 * Eventos do libcoap que interessam à instrumentação.
 *
 * COAP_EVENT_MSG_RETRANSMITTED é a retransmissão NATIVA do CoAP confirmável, e
 * é a única retransmissão de dados de aplicação em todo o testbed: o objeto já
 * protegido pelo OSCORE é reenviado sem ser cifrado de novo. É justamente aí
 * que OSCORE e DTLS divergem na literatura.
 *
 * As falhas de OSCORE entram como perda: R2 manda o canal falhar em voz alta,
 * e uma decifragem recusada não pode passar despercebida no log.
 */
static int event_handler(coap_session_t *session, const coap_event_t event)
{
    (void)session;

    switch (event) {
    case COAP_EVENT_MSG_RETRANSMITTED:
        as_metrics_retx(g_metrics, g_seq, C2_PAYLOAD_LEN, 0);
        break;
    case COAP_EVENT_OSCORE_DECRYPTION_FAILURE:
        fprintf(stderr, "OSCORE: falha de decifragem. Chave do canal C2 "
                        "divergente entre as pontas?\n");
        break;
    case COAP_EVENT_OSCORE_NOT_ENABLED:
    case COAP_EVENT_OSCORE_NO_PROTECTED_PAYLOAD:
    case COAP_EVENT_OSCORE_NO_SECURITY:
        fprintf(stderr, "OSCORE: mensagem sem proteção recusada.\n");
        break;
    default:
        break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const as_profile_t *profile = as_profile_get(AS_THIS_CHANNEL);
    const char *out_dir = "results";
    coap_log_t log_level = COAP_LOG_WARN;
    int port, opt, rc = 1;
    unsigned char key[AS_PSK_LEN];
    char fp[AS_KEY_FP_LEN], port_str[16];
    coap_context_t *ctx = NULL;
    coap_oscore_conf_t *oscore_conf = NULL;
    coap_resource_t *resource = NULL;
    coap_addr_info_t *info = NULL;
    coap_str_const_t bind_addr;

    setvbuf(stdout, NULL, _IOLBF, 0);
    port = profile->port;

    while ((opt = getopt(argc, argv, "p:o:v:h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'v': log_level = (coap_log_t)atoi(optarg); break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %d\n", port);
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

    printf("Canal %s (%s)\nPerfil fixo: %s, UDP/%d\nChave do canal: %s\n",
           profile->slug, profile->app_class, profile->profile_name, port, fp);

    coap_startup();
    coap_set_log_level(log_level);

    g_metrics = as_metrics_open(profile, "server", out_dir);
    if (g_metrics == NULL)
        goto out;

    ctx = coap_new_context(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Erro ao criar o contexto CoAP.\n");
        goto out;
    }

    /*
     * R2: o contexto OSCORE é obrigatório. Se ele não puder ser montado, o
     * canal falha aqui — não sobe em CoAP claro.
     */
    oscore_conf = c2_oscore_conf(key, 1);
    memset(key, 0, sizeof(key));
    if (oscore_conf == NULL)
        goto out;

    if (coap_context_oscore_server(ctx, oscore_conf) != 1) {
        fprintf(stderr, "Erro ao instalar o contexto OSCORE no servidor.\n");
        goto out;
    }
    oscore_conf = NULL;   /* a posse passou para o contexto */

    coap_register_event_handler(ctx, event_handler);

    snprintf(port_str, sizeof(port_str), "%d", port);
    bind_addr.s = (const uint8_t *)"0.0.0.0";
    bind_addr.length = strlen("0.0.0.0");

    info = coap_resolve_address_info(&bind_addr, (uint16_t)port, (uint16_t)port,
                                     (uint16_t)port, (uint16_t)port,
                                     0, 1 << COAP_URI_SCHEME_COAP,
                                     COAP_RESOLVE_TYPE_LOCAL);
    if (info == NULL) {
        fprintf(stderr, "Não foi possível resolver o endereço local.\n");
        goto out;
    }
    if (coap_new_endpoint(ctx, &info->addr, COAP_PROTO_UDP) == NULL) {
        fprintf(stderr, "Erro ao criar o endpoint UDP na porta %d.\n", port);
        goto out;
    }

    resource = coap_resource_init(coap_make_str_const(C2_RESOURCE), 0);
    if (resource == NULL) {
        fprintf(stderr, "Erro ao criar o recurso /%s.\n", C2_RESOURCE);
        goto out;
    }
    coap_register_request_handler(resource, COAP_REQUEST_GET, hnd_get_telemetry);
    coap_add_resource(ctx, resource);

    printf("Canal %s aguardando na porta %d, recurso /%s\n",
           profile->slug, port, C2_RESOURCE);

    while (!g_stop) {
        int res = coap_io_process(ctx, 1000);

        if (res < 0)
            break;
    }

    printf("Servidor encerrado após %llu leituras.\n",
           (unsigned long long)g_seq);
    rc = 0;

out:
    if (g_metrics != NULL)
        as_metrics_close(g_metrics);
    if (info != NULL)
        coap_free_address_info(info);
    if (oscore_conf != NULL)
        coap_delete_oscore_conf(oscore_conf);
    if (ctx != NULL)
        coap_free_context(ctx);
    coap_cleanup();
    return rc;
}
