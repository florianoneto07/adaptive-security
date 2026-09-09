/*
 * Canal C3 - Vídeo e áudio em tempo real.
 * Perfil fixo: handshake DTLS 1.3 + SRTP nos pacotes de mídia, UDP/5003.
 *
 * ATENÇÃO: este perfil está FORA dos perfis Ua* do 3GPP. É proposta do autor,
 * e está assim marcado no README.
 *
 * Mídia em tempo real usa RTP. O DTLS aqui serve APENAS para estabelecer as
 * chaves, via extensão use_srtp (RFC 5764); quem protege os pacotes de mídia é
 * o SRTP. Não é a camada de registro do DTLS protegendo o vídeo.
 *
 * Lado emissor. Gerador de tráfego da classe: fluxo sintético de pacotes RTP
 * com bitrate e tamanho de pacote configuráveis, sem codec real.
 *
 * Fonte: vídeo FHD exige alguns Mbps, 4K acima de 30 Mbps [Wu et al.,
 * arXiv 1804.02217]. Os padrões de 4 Mbps e 1200 B por pacote são padrão do
 * testbed, não vêm da literatura.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
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
#include "as_signal.h"
#include "c3_srtp.h"

#define AS_THIS_CHANNEL AS_CH_C3_MEDIA

#define DTLS_TIMEOUT_INIT 1
#define DTLS_TIMEOUT_MAX  8

/* Padrões do testbed, não vêm da literatura. */
#define DEFAULT_HOST      "127.0.0.1"
#define DEFAULT_KBPS      4000
#define DEFAULT_PAYLOAD   1200
#define DEFAULT_SECONDS   60
#define DEFAULT_PT        96      /* tipo de payload dinâmico, RFC 3551 */
#define DEFAULT_SSRC      0x0C3ACAFEu

/* Payload + cabeçalho RTP + folga para a tag do AES-GCM. */
#define C3_BUF_MAX (C3_RTP_HDR_LEN + 4096 + SRTP_MAX_TRAILER_LEN)

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-H host] [-p porta] [-b kbps] [-l bytes] [-d seg] [-o dir]\n"
        "  -H  endereço do servidor (padrão %s)\n"
        "  -p  porta (padrão %u)\n"
        "  -b  bitrate de mídia em kbps (padrão %d)\n"
        "  -l  payload RTP em bytes (padrão %d)\n"
        "  -d  duração em segundos (padrão %d)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n"
        "\n"
        "  A taxa de pacotes é derivada: kbps / (8 * payload).\n",
        prog, DEFAULT_HOST, as_profile_get(AS_THIS_CHANNEL)->port,
        DEFAULT_KBPS, DEFAULT_PAYLOAD, DEFAULT_SECONDS);
}

static const char *ssl_err(WOLFSSL *ssl, int ret)
{
    return wolfSSL_ERR_reason_error_string(
        (unsigned long)wolfSSL_get_error(ssl, ret));
}

/* Preenche o cabeçalho RTP fixo (RFC 3550, seção 5.1). */
static void rtp_header(unsigned char *p, uint16_t seq, uint32_t ts,
                       uint32_t ssrc, unsigned pt)
{
    p[0] = 0x80;                       /* V=2, P=0, X=0, CC=0 */
    p[1] = (unsigned char)(pt & 0x7F); /* M=0, PT              */
    p[2] = (unsigned char)(seq >> 8);
    p[3] = (unsigned char)seq;
    as_put_u32(p + 4, ts);
    as_put_u32(p + 8, ssrc);
}

int main(int argc, char **argv)
{
    const as_profile_t *profile = as_profile_get(AS_THIS_CHANNEL);
    const char *host = DEFAULT_HOST;
    const char *out_dir = "results";
    int port, opt, ret, rc = 1;
    long kbps = DEFAULT_KBPS, payload = DEFAULT_PAYLOAD;
    long seconds = DEFAULT_SECONDS;
    int fd = -1;
    struct sockaddr_in peer;
    unsigned char key[AS_PSK_LEN], km[C3_KEYING_LEN], key_wsalt[C3_KEY_WSALT];
    unsigned char pkt[C3_BUF_MAX];
    char fp[AS_KEY_FP_LEN];
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    as_metrics_t *m = NULL;
    srtp_t srtp_out = NULL;
    int srtp_started = 0;
    uint64_t t_start, cpu_start, period_ns, deadline, next_ns;
    uint32_t seq = 0;
    struct timespec next;

    setvbuf(stdout, NULL, _IOLBF, 0);
    port = profile->port;

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

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %d\n", port);
        return 1;
    }
    if (payload < AS_HDR_LEN || payload > 4096) {
        fprintf(stderr, "Payload fora de [%d, 4096]: %ld\n", AS_HDR_LEN, payload);
        return 1;
    }
    if (kbps <= 0 || seconds <= 0) {
        fprintf(stderr, "Bitrate e duração precisam ser positivos.\n");
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

    /* Taxa de pacotes derivada do bitrate e do tamanho do payload. */
    period_ns = (uint64_t)payload * 8ULL * 1000000ULL / (uint64_t)kbps;
    if (period_ns == 0) {
        fprintf(stderr, "Bitrate alto demais para o tamanho de pacote.\n");
        return 1;
    }

    printf("Canal %s (%s)\nPerfil fixo: %s, UDP/%d\nChave do canal: %s\n"
           "Gerador: %ld kbps em pacotes de %ld B (%.1f pacotes/s) por %ld s\n",
           profile->slug, profile->app_class, profile->profile_name, port, fp,
           kbps, payload, 1e9 / (double)period_ns, seconds);

    wolfSSL_Init();
    if (srtp_init() != srtp_err_status_ok) {
        fprintf(stderr, "srtp_init falhou.\n");
        goto out;
    }
    srtp_started = 1;

    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_client_method());
    if (ctx == NULL) {
        fprintf(stderr, "DTLS 1.3 não disponível nesta build do wolfSSL.\n");
        goto out;
    }
    if (as_psk_install_client(ctx, profile, key) != 0)
        goto out;
    memset(key, 0, sizeof(key));

    /*
     * R2: o perfil SRTP é exigido no handshake. Se a outra ponta não oferecer
     * use_srtp, a exportação de chave falha e o canal para — não há caminho que
     * mande mídia em claro nem que a proteja com a camada de registro do DTLS.
     */
    if (wolfSSL_CTX_set_tlsext_use_srtp(ctx, C3_SRTP_PROFILE) != 0) {
        fprintf(stderr,
                "Não foi possível exigir o perfil %s.\n"
                "O wolfSSL foi compilado com --enable-srtp?\n", C3_SRTP_PROFILE);
        goto out;
    }

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

    ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Falha no handshake DTLS do canal %s: %s\n",
                profile->slug, ssl_err(ssl, ret));
        goto out;
    }

    as_metrics_handshake(m, as_mono_ns() - t_start, as_cpu_ns() - cpu_start);
    printf("Handshake concluído: %s / %s\n",
           wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl));

    if (c3_export_keying(ssl, km) != 0)
        goto out;
    c3_key_wsalt(km, 1, key_wsalt);
    memset(km, 0, sizeof(km));

    /* Dados do handshake já não são necessários. */
    wolfSSL_FreeArrays(ssl);

    if (c3_srtp_session(&srtp_out, key_wsalt, 0) != 0)
        goto out;

    printf("Chave SRTP derivada do handshake DTLS (perfil %s).\n"
           "A mídia segue protegida por SRTP, não pela camada de registro DTLS.\n",
           C3_SRTP_PROFILE);

    /* Corpo sintético: o que se mede é volume e cadência, não conteúdo. */
    memset(pkt, 0x5A, sizeof(pkt));

    deadline = as_mono_ns() + (uint64_t)seconds * 1000000000ULL;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop && as_mono_ns() < deadline) {
        int len = C3_RTP_HDR_LEN + (int)payload;
        uint64_t cpu0, now;
        srtp_err_status_t err;
        ssize_t sent;

        now = as_mono_ns();
        rtp_header(pkt, (uint16_t)seq, (uint32_t)(now / 1000), DEFAULT_SSRC,
                   DEFAULT_PT);

        /*
         * Marca de tempo do emissor dentro do payload. O receptor a usa para o
         * atraso unidirecional; como os relógios das duas VMs não são
         * sincronizados, o valor absoluto carrega um deslocamento desconhecido
         * e só as DIFERENÇAS entre percentis são interpretáveis. Está anotado
         * no README, junto da leitura dos CSVs.
         */
        as_hdr_put(pkt + C3_RTP_HDR_LEN, seq, now);

        /*
         * A janela de CPU cobre só o srtp_protect: é onde o pacote de mídia é
         * cifrado. O envio no socket fica de fora de propósito, para que o
         * número seja comparável ao custo de cifragem dos outros canais.
         */
        cpu0 = as_cpu_ns();
        err = srtp_protect(srtp_out, pkt, &len);
        if (err != srtp_err_status_ok) {
            fprintf(stderr, "srtp_protect falhou no pacote %u: %d\n",
                    seq, (int)err);
            goto out;
        }
        sent = send(fd, pkt, (size_t)len, 0);
        if (sent < 0) {
            perror("send");
            goto out;
        }
        as_metrics_msg(m, seq, 0, (size_t)payload, as_cpu_ns() - cpu0);
        /*
         * A mídia sai por send() direto, fora do wolfSSL, então os callbacks de
         * I/O não a veem: sem esta soma, wire_tx_bytes cobriria só o handshake.
         */
        as_metrics_wire_tx(m, (uint64_t)sent, 1);
        seq++;

        next.tv_nsec += (long)period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        next_ns = (uint64_t)next.tv_sec * 1000000000ULL + (uint64_t)next.tv_nsec;
        if (next_ns > as_mono_ns())
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    printf("Enviados %u pacotes RTP protegidos por SRTP.\n", seq);
    wolfSSL_shutdown(ssl);
    rc = 0;

out:
    if (m != NULL)
        as_metrics_close(m);
    if (srtp_out != NULL)
        srtp_dealloc(srtp_out);
    if (srtp_started)
        srtp_shutdown();
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (fd >= 0)
        close(fd);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return rc;
}
