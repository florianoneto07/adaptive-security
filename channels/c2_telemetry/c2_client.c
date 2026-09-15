/*
 * Canal C2 - Telemetria periódica. Perfil fixo: OSCORE sobre CoAP, UDP/5002.
 *
 * Lado cliente. Gerador de tráfego da classe: requisição/resposta CoAP com
 * payload pequeno e período configurável.
 *
 * Fonte: o experimento de referência usou uma leitura de 2 bytes requisitada a
 * intervalos de 2 +- 0,5 s [Gündoğan et al., arXiv 2001.08023, §V-A]. São
 * esses os padrões aqui; ambos são opção de linha de comando.
 *
 * As requisições são confirmáveis (CON) de propósito: é o mecanismo de
 * retransmissão NATIVO do CoAP, e a única retransmissão de dados de aplicação
 * em todo o testbed. Ela reenvia um objeto que o OSCORE já protegeu, sem
 * cifrar de novo — o ponto em que OSCORE e DTLS divergem na literatura.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <coap3/coap.h>

#include "as_keys.h"
#include "as_metrics.h"
#include "as_profile.h"
#include "as_signal.h"
#include "c2_oscore.h"

#define AS_THIS_CHANNEL AS_CH_C2_TELEMETRY

#define C2_RESOURCE    "telemetry"
#define C2_PAYLOAD_LEN 2

/* Da literatura: leitura pequena a cada 2 +- 0,5 s. */
#define DEFAULT_PERIOD_MS 2000
#define DEFAULT_JITTER_MS 500

/* Padrões do testbed, não vêm da literatura. */
#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_SECONDS 60
#define DEFAULT_SEED    1

/*
 * Espera máxima por resposta. Padrão do testbed, não vem da literatura:
 * cobre com folga as retransmissões do CoAP confirmável (ACK_TIMEOUT de 2 s
 * dobrando por até 4 tentativas, RFC 7252 §4.8) sem prender o gerador.
 */
#define DEFAULT_RECV_TIMEOUT_MS 30000

static volatile sig_atomic_t g_stop;
static as_metrics_t *g_metrics;

static uint64_t g_seq;        /* leituras enviadas                          */
static uint64_t g_responses;  /* respostas recebidas                        */
static uint64_t g_retx;       /* retransmissões nativas do CoAP confirmável */
static uint64_t g_nacks;      /* requisições que falharam de vez            */
static uint64_t g_reestab;    /* sessões recriadas após o enlace derrubá-la */

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-H host] [-p porta] [-t ms] [-j ms] [-d seg] [-S seed]\n"
        "            [-w ms] [-o dir] [-v nivel]\n"
        "  -H  endereço do servidor (padrão %s)\n"
        "  -p  porta (padrão %u)\n"
        "  -t  período entre leituras em ms (padrão %d)\n"
        "  -j  jitter do período em ms, para mais ou para menos (padrão %d)\n"
        "  -d  duração em segundos (padrão %d)\n"
        "  -S  semente do sorteio do jitter (padrão %d, fixa para reprodutibilidade)\n"
        "  -w  espera máxima por resposta em ms (padrão %d)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n"
        "  -v  verbosidade do libcoap, 0 a 9 (padrão 4, WARN)\n"
        "\n"
        "  O padrão de %d +- %d ms com leitura de %d B vem de\n"
        "  [Gündoğan et al., arXiv 2001.08023, §V-A].\n",
        prog, DEFAULT_HOST, as_profile_get(AS_THIS_CHANNEL)->port,
        DEFAULT_PERIOD_MS, DEFAULT_JITTER_MS, DEFAULT_SECONDS, DEFAULT_SEED,
        DEFAULT_RECV_TIMEOUT_MS, DEFAULT_PERIOD_MS, DEFAULT_JITTER_MS,
        C2_PAYLOAD_LEN);
}

/*
 * Falha definitiva de uma requisição: o CoAP esgotou as retransmissões, ou a
 * resposta foi recusada. Conta como perda, e não como retransmissão.
 *
 * coap_send_recv() devolve -3 quando este handler dispara, de modo que a
 * contagem aqui e o código de retorno lá descrevem o mesmo evento.
 */
static void nack_handler(coap_session_t *session, const coap_pdu_t *sent,
                         const coap_nack_reason_t reason, const coap_mid_t mid)
{
    (void)session;
    (void)sent;
    (void)reason;
    (void)mid;

    g_nacks++;
}

static int event_handler(coap_session_t *session, const coap_event_t event)
{
    (void)session;

    switch (event) {
    case COAP_EVENT_MSG_RETRANSMITTED:
        /*
         * Retransmissão nativa do CoAP CON. O custo de CPU registrado é zero
         * porque nada é cifrado de novo: o objeto protegido pelo OSCORE é
         * reenviado como está. Esse zero é o resultado, não uma medição que
         * faltou — comparar com cpu_first_send_ns é o ponto do experimento.
         */
        as_metrics_retx(g_metrics, g_seq, 0, 0);
        g_retx++;
        break;
    case COAP_EVENT_OSCORE_DECRYPTION_FAILURE:
        fprintf(stderr, "OSCORE: falha de decifragem. Chave do canal C2 "
                        "divergente entre as pontas?\n");
        break;
    default:
        break;
    }
    return 0;
}

/*
 * Cria contexto + sessão OSCORE do cliente, prontos para transacionar.
 *
 * Cada chamada monta um contexto novo, de propósito: é o que permite recriar a
 * sessão no meio da execução quando o enlace a derruba (ver o laço principal).
 * O número de sequência do OSCORE é persistido em arquivo a cada envio
 * (ssn_freq=1, padrão do libcoap), então um contexto reconstruído retoma a
 * numeração de onde o anterior parou — sem reusar nonce e sem ser recusado
 * pelo servidor como repetição.
 *
 * Devolve 0 e preenche ctx_out e sess_out; -1 em erro, com a mensagem já em
 * stderr. A posse do coap_oscore_conf_t passa para a sessão.
 */
static int c2_open_session(const unsigned char key[AS_PSK_LEN],
                           const coap_address_t *addr,
                           coap_context_t **ctx_out,
                           coap_session_t **sess_out)
{
    coap_context_t *ctx;
    coap_session_t *sess;
    coap_oscore_conf_t *conf;

    ctx = coap_new_context(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Erro ao criar o contexto CoAP.\n");
        return -1;
    }
    coap_register_nack_handler(ctx, nack_handler);
    coap_register_event_handler(ctx, event_handler);

    /* R2: sem contexto OSCORE o canal não sobe — não há CoAP em claro aqui. */
    conf = c2_oscore_conf(key, 0);
    if (conf == NULL) {
        coap_free_context(ctx);
        return -1;
    }

    sess = coap_new_client_session_oscore3(ctx, NULL, addr, COAP_PROTO_UDP,
                                           conf, NULL, NULL, NULL);
    if (sess == NULL) {
        fprintf(stderr, "Erro ao criar a sessão OSCORE do canal C2.\n");
        coap_free_context(ctx);
        return -1;
    }

    *ctx_out = ctx;
    *sess_out = sess;
    return 0;
}

int main(int argc, char **argv)
{
    const as_profile_t *profile = as_profile_get(AS_THIS_CHANNEL);
    const char *host = DEFAULT_HOST;
    const char *out_dir = "results";
    coap_log_t log_level = COAP_LOG_WARN;
    int port, opt, rc = 1;
    long period_ms = DEFAULT_PERIOD_MS, jitter_ms = DEFAULT_JITTER_MS;
    long seconds = DEFAULT_SECONDS;
    long recv_timeout_ms = DEFAULT_RECV_TIMEOUT_MS;
    unsigned seed = DEFAULT_SEED;
    unsigned char key[AS_PSK_LEN];
    char fp[AS_KEY_FP_LEN];
    coap_context_t *ctx = NULL;
    coap_session_t *session = NULL;
    coap_addr_info_t *info = NULL;
    coap_str_const_t server_str;
    uint64_t deadline;

    setvbuf(stdout, NULL, _IOLBF, 0);
    port = profile->port;

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

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %d\n", port);
        return 1;
    }
    if (period_ms <= 0 || seconds <= 0) {
        fprintf(stderr, "Período e duração precisam ser positivos.\n");
        return 1;
    }
    if (jitter_ms < 0 || jitter_ms >= period_ms) {
        fprintf(stderr, "Jitter precisa estar em [0, período).\n");
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

    srand(seed);

    printf("Canal %s (%s)\nPerfil fixo: %s, UDP/%d\nChave do canal: %s\n"
           "Gerador: leitura de 2 B a cada %ld +- %ld ms por %ld s\n",
           profile->slug, profile->app_class, profile->profile_name, port, fp,
           period_ms, jitter_ms, seconds);

    coap_startup();
    coap_set_log_level(log_level);

    g_metrics = as_metrics_open(profile, "client", out_dir);
    if (g_metrics == NULL)
        goto out;

    server_str.s = (const uint8_t *)host;
    server_str.length = strlen(host);
    info = coap_resolve_address_info(&server_str, (uint16_t)port, (uint16_t)port,
                                     (uint16_t)port, (uint16_t)port,
                                     0, 1 << COAP_URI_SCHEME_COAP,
                                     COAP_RESOLVE_TYPE_REMOTE);
    if (info == NULL) {
        fprintf(stderr, "Não foi possível resolver %s.\n", host);
        goto out;
    }

    /*
     * O número de sequência do OSCORE fica ao lado da chave do canal: cada
     * execução do cliente retoma de onde a anterior parou, em vez de recomeçar
     * do zero e ser recusada pelo servidor como repetição.
     */
    c2_seq_file_init(getenv("AS_KEY_DIR"), "c2.seq");

    if (c2_open_session(key, &info->addr, &ctx, &session) != 0)
        goto out;

    /*
     * OSCORE não tem handshake, então não há tempo de estabelecimento no
     * sentido de TLS/DTLS: a primeira mensagem já vai protegida. Registrar
     * zero aqui é o que torna a coluna comparável com os outros canais — e a
     * ausência de handshake é uma propriedade do perfil, não uma medição
     * faltando.
     */
    as_metrics_handshake(g_metrics, 0, 0);

    deadline = as_mono_ns() + (uint64_t)seconds * 1000000000ULL;

    while (!g_stop && as_mono_ns() < deadline) {
        coap_pdu_t *pdu, *response = NULL;
        uint64_t cpu0, t_send, rtt, cpu_used;
        long jitter, sleep_ms;
        int res, reopen = 0;

        pdu = coap_new_pdu(COAP_MESSAGE_CON, COAP_REQUEST_CODE_GET, session);
        if (pdu == NULL) {
            fprintf(stderr, "Erro ao montar a requisição.\n");
            goto out;
        }
        if (coap_add_option(pdu, COAP_OPTION_URI_PATH, strlen(C2_RESOURCE),
                            (const uint8_t *)C2_RESOURCE) == 0) {
            fprintf(stderr, "Erro ao acrescentar Uri-Path.\n");
            coap_delete_pdu(pdu);
            goto out;
        }

        /*
         * Transação síncrona, e não coap_send() com laço de I/O próprio.
         *
         * Na primeira requisição OSCORE o libcoap liga session->doing_first
         * (coap_net.c, no trecho de oscore_encryption) para segurar os envios
         * seguintes até saber se o peer responde com "4.01 + Echo" (RFC 9175).
         * Com coap_send() assíncrono o reenvio que carrega o Echo também cai
         * nessa fila, e nada é transmitido até o timeout interno de 5 s expirar.
         * coap_send_recv() conduz esse diálogo internamente.
         *
         * Consequência para as métricas: a janela de CPU do C2 cobre proteger
         * o pedido E verificar a resposta, enquanto em C1, C3 e C4 ela cobre só
         * o envio. A API síncrona não expõe ponto de medição entre as duas
         * metades. A diferença está registrada no README, junto da leitura dos
         * CSVs, para não ser tomada por um custo maior do OSCORE.
         */
        t_send = as_mono_ns();
        cpu0 = as_cpu_ns();
        res = coap_send_recv(session, pdu, &response, (uint32_t)recv_timeout_ms);
        cpu_used = as_cpu_ns() - cpu0;
        rtt = as_mono_ns() - t_send;

        as_metrics_msg(g_metrics, g_seq, 0, C2_PAYLOAD_LEN, cpu_used);

        /*
         * Um PDU de volta não significa leitura bem-sucedida: o servidor
         * responde 4.00 quando não consegue decifrar, e coap_send_recv()
         * devolve isso como transação concluída. Sem checar a classe do
         * código, um canal com chaves divergentes seria contabilizado como
         * dez leituras perfeitas — foi exatamente o que aconteceu aqui antes
         * desta verificação.
         */
        if (res >= 0 && response != NULL &&
            COAP_RESPONSE_CLASS(coap_pdu_get_code(response)) == 2) {
            size_t len = 0;
            const uint8_t *data = NULL;

            coap_get_data(response, &len, &data);
            as_metrics_rtt(g_metrics, g_seq, rtt);
            g_responses++;

            /*
             * A primeira transação carrega o desafio Echo, que é o custo de
             * estabelecimento do OSCORE — não há handshake, mas há um ida e
             * volta extra antes de a primeira leitura ser aceita. É o
             * equivalente comparável ao handshake dos outros canais.
             */
            if (g_responses == 1)
                as_metrics_handshake(g_metrics, rtt, cpu_used);
        } else {
            as_metrics_lost(g_metrics, g_seq);

            if (response != NULL) {
                /* R2: o canal falha em voz alta, não segue como se estivesse bem. */
                coap_pdu_code_t code = coap_pdu_get_code(response);
                const char *hint;

                /*
                 * As duas recusas têm causas distintas e confundi-las custa
                 * caro no diagnóstico:
                 *
                 *   4.00  o servidor não decifrou — chaves diferentes.
                 *   4.01  o contexto OSCORE recusou por número de sequência —
                 *         tipicamente um segundo cliente contra um servidor
                 *         que já atendeu outro, começando a sequência do zero.
                 *         A janela anti-replay está funcionando; o que falta é
                 *         um servidor novo por execução.
                 */
                if (COAP_RESPONSE_CLASS(code) == 4 && (code & 0x1F) == 1)
                    hint = "contexto OSCORE recusou a sequência. Um servidor "
                           "novo é necessário a cada execução do cliente";
                else
                    hint = "servidor não decifrou. Chave do canal C2 "
                           "divergente entre as pontas?";

                fprintf(stderr, "Leitura %llu recusada: %d.%02d — %s.\n",
                        (unsigned long long)g_seq,
                        COAP_RESPONSE_CLASS(code), code & 0x1F, hint);
            } else if (res == -5) {
                fprintf(stderr, "Leitura %llu sem resposta em %ld ms.\n",
                        (unsigned long long)g_seq, recv_timeout_ms);
            } else {
                fprintf(stderr, "Leitura %llu falhou (coap_send_recv=%d).\n",
                        (unsigned long long)g_seq, res);
                /*
                 * -2 (falha ao transmitir) e -4 (erro de I/O) deixam a sessão
                 * inutilizável: no libcoap 4.3.5 esta versão não a reconecta
                 * sozinha, e sem recriá-la TODAS as leituras seguintes falham
                 * em microssegundos até o fim da repetição — foi o que zerou o
                 * C2 sob handover. A leitura atual já conta como perda; a
                 * sessão é recriada abaixo para que as próximas voltem a valer.
                 * Um timeout puro (-5) não fecha a sessão: aí é só perda.
                 */
                if (res == -2 || res == -4)
                    reopen = 1;
            }
        }

        if (response != NULL)
            coap_delete_pdu(response);
        coap_delete_pdu(pdu);
        g_seq++;

        if (reopen) {
            coap_session_release(session);
            coap_free_context(ctx);
            session = NULL;
            ctx = NULL;
            if (c2_open_session(key, &info->addr, &ctx, &session) != 0) {
                fprintf(stderr, "Canal %s: não foi possível recriar a sessão "
                                "após o enlace derrubá-la; encerrando.\n",
                        profile->slug);
                goto out;
            }
            g_reestab++;
            fprintf(stderr, "Canal %s: sessão OSCORE recriada (%llu no total); "
                            "o número de sequência foi retomado do arquivo.\n",
                    profile->slug, (unsigned long long)g_reestab);
        }

        /*
         * Período com jitter uniforme em [-jitter, +jitter], descontado o
         * tempo que a transação já consumiu: é o intervalo ENTRE leituras que
         * a literatura fixa, não o intervalo entre o fim de uma e o início da
         * seguinte.
         */
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

    /*
     * R2: um canal que enviou tudo e não recebeu nada falhou, e precisa dizer
     * isso. Antes retornava sucesso, e uma campanha inteira era marcada como
     * "ok" com nove repetições de dez sem uma única resposta — o defeito só
     * apareceu ao agregar os resultados.
     */
    if (g_seq > 0 && g_responses == 0) {
        fprintf(stderr,
                "Canal %s: nenhuma das %llu leituras foi respondida.\n",
                profile->slug, (unsigned long long)g_seq);
        rc = 1;
    } else {
        rc = 0;
    }

out:
    /* A chave viveu na pilha toda a execução para permitir recriar a sessão. */
    memset(key, 0, sizeof(key));
    if (g_metrics != NULL)
        as_metrics_close(g_metrics);
    if (session != NULL)
        coap_session_release(session);
    if (info != NULL)
        coap_free_address_info(info);
    if (ctx != NULL)
        coap_free_context(ctx);
    coap_cleanup();
    return rc;
}
