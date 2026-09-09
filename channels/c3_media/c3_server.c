/*
 * Canal C3 - Vídeo e áudio em tempo real.
 * Perfil fixo: handshake DTLS 1.3 + SRTP nos pacotes de mídia, UDP/5003.
 *
 * ATENÇÃO: perfil FORA dos perfis Ua* do 3GPP. É proposta do autor.
 *
 * Lado receptor. Faz o handshake DTLS só para obter as chaves e, a partir daí,
 * recebe pacotes RTP protegidos por SRTP no mesmo socket UDP, separando-os dos
 * registros DTLS pelo primeiro byte (RFC 5764, seção 5.1.2).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "as_keys.h"
#include "as_metrics.h"
#include "as_pack.h"
#include "as_profile.h"
#include "as_psk.h"
#include "as_signal.h"
#include "c3_srtp.h"

#define AS_THIS_CHANNEL AS_CH_C3_MEDIA

#define DTLS_TIMEOUT_INIT 1
#define DTLS_TIMEOUT_MAX  8
#define DTLS_CONTENT_HANDSHAKE 22

#define C3_BUF_MAX (C3_RTP_HDR_LEN + 4096 + SRTP_MAX_TRAILER_LEN)

/*
 * Fim do fluxo. Padrão do testbed: sem pacote nenhum por este tempo, a mídia
 * acabou. Não existe close_notify em SRTP — o DTLS já encerrou seu papel
 * quando as chaves foram derivadas.
 */
#define IDLE_TIMEOUT_S 5

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida] [-k]\n"
        "  -p  porta de escuta (padrão %u)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n"
        "  -k  atende fluxos indefinidamente\n",
        prog, as_profile_get(AS_THIS_CHANNEL)->port);
}

static const char *ssl_err(WOLFSSL *ssl, int ret)
{
    return wolfSSL_ERR_reason_error_string(
        (unsigned long)wolfSSL_get_error(ssl, ret));
}

/* Recebe um fluxo completo: handshake DTLS, depois mídia SRTP. */
static int serve_stream(WOLFSSL_CTX *ctx, int port,
                        const as_profile_t *profile, as_metrics_t *m)
{
    int fd = -1, one = 1, ret, rc = -1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len;
    char peer_ip[INET_ADDRSTRLEN];
    unsigned char probe[1], pkt[C3_BUF_MAX];
    unsigned char km[C3_KEYING_LEN], key_wsalt[C3_KEY_WSALT];
    WOLFSSL *ssl = NULL;
    srtp_t srtp_in = NULL;
    uint64_t t_start, cpu_start;
    uint32_t expected = 0, received = 0;
    int first = 1;
    struct timeval tv;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto done;
    }

    printf("Canal %s aguardando na porta %d...\n", profile->slug, port);

    /* Descobre o peer no primeiro ClientHello, como no C1. */
    for (;;) {
        peer_len = sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        if (recvfrom(fd, probe, sizeof(probe), MSG_PEEK,
                     (struct sockaddr *)&peer, &peer_len) < 0) {
            if (g_stop)
                goto done;
            perror("recvfrom");
            goto done;
        }
        if (probe[0] == DTLS_CONTENT_HANDSHAKE)
            break;
        if (recvfrom(fd, probe, sizeof(probe), 0, NULL, NULL) < 0) {
            perror("recvfrom");
            goto done;
        }
    }

    inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    printf("Fluxo de %s:%d\n", peer_ip, ntohs(peer.sin_port));

    if (connect(fd, (struct sockaddr *)&peer, peer_len) < 0) {
        perror("connect");
        goto done;
    }

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão DTLS.\n");
        goto done;
    }
    if (wolfSSL_dtls_set_timeout_init(ssl, DTLS_TIMEOUT_INIT) != WOLFSSL_SUCCESS ||
        wolfSSL_dtls_set_timeout_max(ssl, DTLS_TIMEOUT_MAX) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao configurar os timeouts do DTLS.\n");
        goto done;
    }
    wolfSSL_set_fd(ssl, fd);
    if (wolfSSL_dtls_set_peer(ssl, &peer, peer_len) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao registrar o peer DTLS.\n");
        goto done;
    }
    if (as_wire_attach(ssl, m, 1) != 0)
        goto done;
    if (wolfSSL_send_hrr_cookie(ssl, NULL, 0) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao habilitar o cookie de HelloRetryRequest.\n");
        goto done;
    }

    /*
     * O exporter de material de chave lê dados do handshake que o wolfSSL
     * descarta assim que ele termina. Sem KeepArrays a exportação falha com
     * WOLFSSL_FAILURE, e a mensagem interna que explica o motivo só aparece em
     * build de debug — o sintoma visível é um handshake bem-sucedido seguido
     * de chave SRTP indisponível. Liberados logo após a exportação.
     */
    wolfSSL_KeepArrays(ssl);

    t_start = as_mono_ns();
    cpu_start = as_cpu_ns();

    ret = wolfSSL_accept(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Falha no handshake DTLS do canal %s: %s\n",
                profile->slug, ssl_err(ssl, ret));
        goto done;
    }

    as_metrics_handshake(m, as_mono_ns() - t_start, as_cpu_ns() - cpu_start);
    printf("Handshake concluído: %s / %s / identidade PSK \"%s\"\n",
           wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl),
           profile->psk_identity);

    if (c3_export_keying(ssl, km) != 0)
        goto done;
    /* Sentido do cliente: é ele quem emite a mídia. */
    c3_key_wsalt(km, 1, key_wsalt);
    memset(km, 0, sizeof(km));

    /* Dados do handshake já não são necessários. */
    wolfSSL_FreeArrays(ssl);

    if (c3_srtp_session(&srtp_in, key_wsalt, 1) != 0)
        goto done;

    printf("Chave SRTP derivada. Recebendo mídia...\n");

    /*
     * A partir daqui o socket é lido direto, sem passar pelo wolfSSL: os
     * pacotes de mídia são SRTP puro sobre UDP, e não registros DTLS.
     */
    tv.tv_sec = IDLE_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!g_stop) {
        ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
        int len;
        uint32_t seq;
        uint64_t cpu0, cpu_verify, sent_ns, now;
        srtp_err_status_t err;

        if (n < 0)
            break;   /* silêncio por IDLE_TIMEOUT_S: fluxo terminou */

        if (!c3_is_srtp(pkt, (size_t)n))
            continue;   /* registro DTLS residual, p.ex. close_notify */

        /* Idem ao cliente: a mídia não passa pelo wolfSSL. */
        as_metrics_wire_rx(m, (uint64_t)n, 1);

        len = (int)n;
        cpu0 = as_cpu_ns();
        err = srtp_unprotect(srtp_in, pkt, &len);
        cpu_verify = as_cpu_ns() - cpu0;
        if (err != srtp_err_status_ok) {
            /* R2: pacote que não verifica é descartado, nunca aceito em claro. */
            fprintf(stderr, "srtp_unprotect recusou um pacote: %d\n", (int)err);
            continue;
        }
        now = as_mono_ns();

        if (len < C3_RTP_HDR_LEN + AS_HDR_LEN)
            continue;

        seq = as_get_u32(pkt + C3_RTP_HDR_LEN);
        sent_ns = as_get_u64(pkt + C3_RTP_HDR_LEN + 4);

        if (first) {
            first = 0;
        } else if (seq > expected) {
            uint32_t gap;

            for (gap = expected; gap < seq; gap++)
                as_metrics_lost(m, gap);
        }
        expected = seq + 1;
        received++;

        as_metrics_recv(m, seq, (size_t)(len - C3_RTP_HDR_LEN), cpu_verify);

        /*
         * Atraso unidirecional. Sem relógios sincronizados entre as VMs, o
         * valor carrega um deslocamento constante desconhecido: as diferenças
         * entre percentis valem, os valores absolutos não.
         */
        if (now > sent_ns)
            as_metrics_rtt(m, seq, now - sent_ns);

    }

    printf("Fluxo encerrado: %u pacotes recebidos.\n", received);
    rc = 0;

done:
    if (srtp_in != NULL)
        srtp_dealloc(srtp_in);
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (fd >= 0)
        close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    const as_profile_t *profile = as_profile_get(AS_THIS_CHANNEL);
    const char *out_dir = "results";
    int port, keep_running = 0, opt, rc = 1;
    int srtp_started = 0;
    unsigned char key[AS_PSK_LEN];
    char fp[AS_KEY_FP_LEN];
    WOLFSSL_CTX *ctx = NULL;
    as_metrics_t *m = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
    port = profile->port;

    while ((opt = getopt(argc, argv, "p:o:kh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'k': keep_running = 1; break;
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

    wolfSSL_Init();
    if (srtp_init() != srtp_err_status_ok) {
        fprintf(stderr, "srtp_init falhou.\n");
        goto out;
    }
    srtp_started = 1;

    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_server_method());
    if (ctx == NULL) {
        fprintf(stderr, "DTLS 1.3 não disponível nesta build do wolfSSL.\n");
        goto out;
    }
    if (as_psk_install_server(ctx, profile, key) != 0)
        goto out;
    memset(key, 0, sizeof(key));

    if (wolfSSL_CTX_set_tlsext_use_srtp(ctx, C3_SRTP_PROFILE) != 0) {
        fprintf(stderr,
                "Não foi possível exigir o perfil %s.\n"
                "O wolfSSL foi compilado com --enable-srtp?\n", C3_SRTP_PROFILE);
        goto out;
    }

    m = as_metrics_open(profile, "server", out_dir);
    if (m == NULL)
        goto out;

    do {
        if (serve_stream(ctx, port, profile, m) != 0 && !keep_running)
            goto out;
    } while (keep_running && !g_stop);

    rc = 0;

out:
    if (m != NULL)
        as_metrics_close(m);
    if (srtp_started)
        srtp_shutdown();
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return rc;
}
