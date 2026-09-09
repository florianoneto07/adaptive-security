/*
 * Canal C1 - Comando e controle. Perfil fixo: DTLS 1.3 sobre UDP/5001.
 *
 * Lado emissor. Gerador de tráfego da classe: mensagens pequenas em cadência
 * fixa, dimensionadas para cair na faixa de 60-100 kbps.
 *
 * Fontes: 60-100 kbps, PER 10^-3 e latência de 50 ms são o requisito 3GPP de
 * comando e controle [Zeng et al., arXiv 1903.05289, Tab. I; Fotouhi et al.,
 * arXiv 2005.00781, Tab. 1]. Mensagens de comando MAVLink padrão são bem
 * menores que 256 bytes [arXiv 2404.07557].
 *
 * O par 128 B a 64 Hz (~65,5 kbps) é padrão do testbed dentro da faixa da
 * literatura; ambos são opção de linha de comando.
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

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "adaptive_security.h"
#include "as_keys.h"
#include "as_metrics.h"
#include "as_pack.h"
#include "as_profile.h"
#include "as_psk.h"

#define AS_THIS_CHANNEL AS_CH_C1_CONTROL

#define DTLS_TIMEOUT_INIT 1
#define DTLS_TIMEOUT_MAX  8

/* Padrões do testbed. Tamanho e taxa vêm da faixa 3GPP; a duração, não. */
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
        "  -H  endereço do servidor (padrão %s)\n"
        "  -p  porta (padrão %u)\n"
        "  -l  tamanho da mensagem em bytes (padrão %d, mínimo %d)\n"
        "  -r  taxa de envio em Hz (padrão %d)\n"
        "  -d  duração em segundos (padrão %d)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n"
        "\n"
        "  O padrão %d B a %d Hz equivale a %.1f kbps de payload, dentro da\n"
        "  faixa de 60-100 kbps do requisito 3GPP de comando e controle.\n",
        prog, DEFAULT_HOST, as_profile_get(AS_THIS_CHANNEL)->port,
        DEFAULT_MSG_LEN, AS_HDR_LEN, DEFAULT_RATE_HZ, DEFAULT_SECONDS,
        DEFAULT_MSG_LEN, DEFAULT_RATE_HZ,
        DEFAULT_MSG_LEN * DEFAULT_RATE_HZ * 8 / 1000.0);
}

static const char *ssl_err(WOLFSSL *ssl, int ret)
{
    return wolfSSL_ERR_reason_error_string(
        (unsigned long)wolfSSL_get_error(ssl, ret));
}

/*
 * Consome todos os ecos já disponíveis e registra o RTT de cada um.
 *
 * Chamada entre envios, com o socket em modo não bloqueante: assim a cadência
 * do gerador não depende de o eco chegar, que é o que aconteceria num
 * request/response síncrono — e a taxa medida deixaria de ser a taxa
 * configurada assim que o enlace ganhasse atraso.
 */
static void drain_echoes(WOLFSSL *ssl, as_metrics_t *m, unsigned char *buf,
                         size_t buf_len, uint64_t *rtt_count)
{
    for (;;) {
        int ret = wolfSSL_read(ssl, buf, (int)buf_len);
        int err;

        if (ret >= AS_HDR_LEN) {
            uint32_t seq = as_get_u32(buf);
            uint64_t sent_ns = as_get_u64(buf + 4);
            uint64_t now = as_mono_ns();

            if (now > sent_ns) {
                as_metrics_rtt(m, seq, now - sent_ns);
                (*rtt_count)++;
            }
            continue;
        }
        if (ret > 0)
            continue;   /* eco truncado: ignora e segue */

        err = wolfSSL_get_error(ssl, ret);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
            return;     /* nada mais na fila, é o caso normal */
        return;
    }
}

int main(int argc, char **argv)
{
    const as_profile_t *profile = as_profile_get(AS_THIS_CHANNEL);
    const char *host = DEFAULT_HOST;
    const char *out_dir = "results";
    int port, opt, ret, rc = 1;
    size_t msg_len = DEFAULT_MSG_LEN;
    long rate_hz = DEFAULT_RATE_HZ, seconds = DEFAULT_SECONDS;
    int fd = -1;
    struct sockaddr_in peer;
    unsigned char msg[MAX_MSG], echo[MAX_MSG];
    unsigned char key[AS_PSK_LEN];
    char fp[AS_KEY_FP_LEN];
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    as_metrics_t *m = NULL;
    uint64_t t_start, cpu_start, period_ns, deadline, next_ns, rtt_count = 0;
    uint32_t seq = 0;
    struct timespec next;

    setvbuf(stdout, NULL, _IOLBF, 0);
    port = profile->port;

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

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %d\n", port);
        return 1;
    }
    if (msg_len < AS_HDR_LEN || msg_len > MAX_MSG) {
        fprintf(stderr, "Tamanho de mensagem fora de [%d, %d]: %zu\n",
                AS_HDR_LEN, MAX_MSG, msg_len);
        return 1;
    }
    if (rate_hz <= 0 || seconds <= 0) {
        fprintf(stderr, "Taxa e duração precisam ser positivas.\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (as_key_load(profile, key) != 0)
        return 1;
    if (as_key_fingerprint(key, fp) != 0)
        return 1;

    printf("Canal %s (%s)\nPerfil fixo: %s, UDP/%d\nChave do canal: %s\n"
           "Gerador: %zu B a %ld Hz por %ld s (%.1f kbps de payload)\n",
           profile->slug, profile->app_class, profile->profile_name, port, fp,
           msg_len, rate_hz, seconds, (double)msg_len * rate_hz * 8 / 1000.0);

    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_client_method());
    if (ctx == NULL) {
        fprintf(stderr, "DTLS 1.3 não disponível nesta build do wolfSSL.\n");
        goto out;
    }
    if (as_psk_install_client(ctx, profile, key) != 0)
        goto out;
    memset(key, 0, sizeof(key));

    m = as_metrics_open(profile, "client", out_dir);
    if (m == NULL)
        goto out;

    if (as_resolve_v4(host, port, SOCK_DGRAM, &peer) != 0)
        goto out;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        goto out;
    }
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        perror("connect");
        goto out;
    }

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão DTLS.\n");
        goto out;
    }
    if (wolfSSL_dtls_set_timeout_init(ssl, DTLS_TIMEOUT_INIT) != WOLFSSL_SUCCESS ||
        wolfSSL_dtls_set_timeout_max(ssl, DTLS_TIMEOUT_MAX) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao configurar os timeouts do DTLS.\n");
        goto out;
    }

    wolfSSL_set_fd(ssl, fd);
    if (wolfSSL_dtls_set_peer(ssl, &peer, sizeof(peer)) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao registrar o peer DTLS.\n");
        goto out;
    }
    if (as_wire_attach(ssl, m, 1) != 0)
        goto out;

    t_start = as_mono_ns();
    cpu_start = as_cpu_ns();

    ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        /* R2: falha em voz alta, sem tentar outro perfil. */
        fprintf(stderr, "Falha no handshake DTLS do canal %s: %s\n",
                profile->slug, ssl_err(ssl, ret));
        goto out;
    }

    as_metrics_handshake(m, as_mono_ns() - t_start, as_cpu_ns() - cpu_start);
    printf("Handshake concluído: %s / %s\n",
           wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl));

    /*
     * O handshake corre em modo bloqueante, que é o caminho simples e já
     * validado. Só agora o socket passa a não bloquear, para que a leitura dos
     * ecos não segure o gerador.
     */
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        perror("fcntl");
        goto out;
    }
    wolfSSL_dtls_set_using_nonblock(ssl, 1);

    memset(msg, 0xA5, sizeof(msg));   /* corpo sintético do comando */
    period_ns = 1000000000ULL / (uint64_t)rate_hz;
    deadline = as_mono_ns() + (uint64_t)seconds * 1000000000ULL;

    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop && as_mono_ns() < deadline) {
        uint64_t cpu0;

        as_hdr_put(msg, seq, as_mono_ns());

        /*
         * A janela de CPU envolve só a escrita, onde o registro é cifrado.
         * Medir a iteração inteira somaria o custo do gerador ao do perfil de
         * segurança, e é justamente a separação entre os dois que interessa.
         */
        cpu0 = as_cpu_ns();
        ret = wolfSSL_write(ssl, msg, (int)msg_len);
        if (ret <= 0) {
            int err = wolfSSL_get_error(ssl, ret);

            if (err == WOLFSSL_ERROR_WANT_WRITE)
                continue;
            fprintf(stderr, "Falha ao enviar a mensagem %u: %s\n",
                    seq, ssl_err(ssl, ret));
            goto out;
        }
        /*
         * rtt_ns = 0: o RTT desta mensagem só é conhecido quando o eco volta,
         * e é registrado lá, em drain_echoes(). Aqui fica o custo de proteger
         * e enviar.
         */
        as_metrics_msg(m, seq, 0, (size_t)ret, as_cpu_ns() - cpu0);
        seq++;

        /*
         * Cadência por prazo absoluto, e não por sleep relativo: com sleep
         * relativo o tempo gasto no envio se soma ao período, e a taxa real
         * fica abaixo da configurada, acumulando desvio ao longo dos 60 s.
         */
        next.tv_nsec += (long)period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        next_ns = (uint64_t)next.tv_sec * 1000000000ULL + (uint64_t)next.tv_nsec;

        /*
         * O tempo até o próximo envio é gasto esperando o eco, não dormindo.
         *
         * Drenar só uma vez por iteração, logo após o envio, mediria o RTT
         * como o período do gerador: o eco chega em microssegundos, mas só
         * seria lido 1/taxa depois. Com ppoll o eco é lido quando de fato
         * chega, e o que sobra do período vira espera.
         */
        while (!g_stop) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            struct timespec wait;
            uint64_t now = as_mono_ns();
            int pr;

            if (now >= next_ns)
                break;
            wait.tv_sec  = (time_t)((next_ns - now) / 1000000000ULL);
            wait.tv_nsec = (long)((next_ns - now) % 1000000000ULL);

            pr = ppoll(&pfd, 1, &wait, NULL);
            if (pr > 0)
                drain_echoes(ssl, m, echo, sizeof(echo), &rtt_count);
            else if (pr == 0)
                break;                    /* prazo alcançado */
            else if (errno != EINTR)
                break;
        }
    }

    /* Ecos ainda em trânsito depois do último envio. */
    {
        struct timespec grace = { 0, 200000000L };   /* 200 ms */

        nanosleep(&grace, NULL);
        drain_echoes(ssl, m, echo, sizeof(echo), &rtt_count);
    }

    printf("Enviadas %u mensagens, %llu ecos com RTT medido.\n",
           seq, (unsigned long long)rtt_count);

    wolfSSL_shutdown(ssl);
    rc = 0;

out:
    if (m != NULL)
        as_metrics_close(m);
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (fd >= 0)
        close(fd);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return rc;
}
