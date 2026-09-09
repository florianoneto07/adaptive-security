/*
 * Canal C1 - Comando e controle. Perfil fixo: DTLS 1.3 sobre UDP/5001.
 *
 * O requisito 3GPP para comando e controle de UAV é latência de 50 ms e taxa
 * de erro de pacote de 10^-3 [Zeng et al., arXiv 1903.05289, Tab. I; Fotouhi
 * et al., arXiv 2005.00781, Tab. 1], o que exige UDP: TCP retransmitiria e
 * ordenaria mensagens de controle que já não têm valor quando chegam atrasadas.
 *
 * DTLS 1.3 tem PFS obrigatório e overhead de registro de 11 bytes. O Connection
 * ID (RFC 9146) cobre troca de endereço em mobilidade.
 *
 * Autenticação exclusivamente por PSK do canal (R2). Lado receptor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "as_keys.h"
#include "as_metrics.h"
#include "as_pack.h"
#include "as_profile.h"
#include "as_signal.h"
#include "as_psk.h"

#define AS_THIS_CHANNEL AS_CH_C1_CONTROL

/* Retransmissão do handshake DTLS: dobra a cada tentativa, de 1 s a 8 s. */
#define DTLS_TIMEOUT_INIT 1
#define DTLS_TIMEOUT_MAX  8

/* Tipo de registro DTLS "handshake" (RFC 9147): só ele inicia uma associação. */
#define DTLS_CONTENT_HANDSHAKE 22

#define MAX_MSG 2048

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-p porta] [-o dir_saida] [-k]\n"
        "  -p  porta de escuta (padrão %u)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n"
        "  -k  atende associações indefinidamente\n",
        prog, as_profile_get(AS_THIS_CHANNEL)->port);
}

static const char *ssl_err(WOLFSSL *ssl, int ret)
{
    return wolfSSL_ERR_reason_error_string(
        (unsigned long)wolfSSL_get_error(ssl, ret));
}

/*
 * Atende uma associação DTLS, do bind ao encerramento.
 *
 * O socket é criado e destruído a cada associação de propósito: como ele é
 * conectado ao peer descoberto, atender outro cliente exige um socket novo.
 * Mesmo desenho do dtls_server.c legado, que já está validado entre as VMs.
 */
static int serve_association(WOLFSSL_CTX *ctx, int port,
                             const as_profile_t *profile, as_metrics_t *m)
{
    int fd = -1, one = 1, ret, rc = -1;
    struct sockaddr_in addr, peer;
    socklen_t peer_len;
    char peer_ip[INET_ADDRSTRLEN];
    unsigned char probe[1], buf[MAX_MSG];
    uint64_t t_start, cpu_start;
    uint32_t expected_seq = 0;
    int first = 1;
    WOLFSSL *ssl = NULL;

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

    /*
     * UDP não tem accept(): antes do handshake é preciso descobrir quem está
     * falando. MSG_PEEK espia o primeiro datagrama sem consumi-lo, para que o
     * wolfSSL ainda o encontre no socket.
     *
     * Só um ClientHello inicia associação. Em modo -k, o close_notify da
     * associação anterior costuma chegar depois de o socket ser recriado; sem
     * este filtro ele seria tomado como início de uma nova associação.
     */
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
    printf("Associação de %s:%d\n", peer_ip, ntohs(peer.sin_port));

    /* Conecta ao peer: send() ganha destino e o kernel filtra outras origens. */
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

    /*
     * Cookie no HelloRetryRequest (RFC 9147, seção 5.1). Obriga o cliente a
     * provar que recebe no endereço que alega ter, antes de o servidor gastar
     * CPU com o handshake.
     */
    if (wolfSSL_send_hrr_cookie(ssl, NULL, 0) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Erro ao habilitar o cookie de HelloRetryRequest.\n");
        goto done;
    }

    t_start = as_mono_ns();
    cpu_start = as_cpu_ns();

    ret = wolfSSL_accept(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        /* R2: o canal falha, não degrada para outro perfil. */
        fprintf(stderr, "Falha no handshake DTLS do canal %s: %s\n",
                profile->slug, ssl_err(ssl, ret));
        goto done;
    }

    as_metrics_handshake(m, as_mono_ns() - t_start, as_cpu_ns() - cpu_start);

    printf("Handshake concluído: %s / %s / identidade PSK \"%s\"\n",
           wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl),
           profile->psk_identity);

    /*
     * Laço de recepção. Cada mensagem carrega seq e a marca de tempo do
     * emissor; os primeiros AS_HDR_LEN bytes voltam como eco, que é o que
     * permite ao cliente medir RTT sem relógios sincronizados.
     *
     * O eco é instrumento de medição, não mecanismo do protocolo: ele não
     * confirma entrega nem dispara retransmissão. DTLS não retransmite dados
     * de aplicação, e o testbed não acrescenta isso.
     */
    while (!g_stop) {
        uint32_t seq;
        uint64_t cpu0, cpu_used;

        cpu0 = as_cpu_ns();
        ret = wolfSSL_read(ssl, buf, sizeof(buf));
        cpu_used = as_cpu_ns() - cpu0;
        if (ret <= 0) {
            int err = wolfSSL_get_error(ssl, ret);

            if (err == WOLFSSL_ERROR_ZERO_RETURN)
                break;   /* close_notify: fim normal da associação */
            fprintf(stderr, "Fim da recepção: %s\n", ssl_err(ssl, ret));
            break;
        }
        if (ret < AS_HDR_LEN)
            continue;    /* mensagem curta demais para carregar cabeçalho */

        seq = as_get_u32(buf);

        /*
         * Lacuna de sequência é perda no enlace. Contamos aqui, e não no
         * cliente, porque o cliente só sabe o que enviou — a razão
         * entregues/enviados exige as duas pontas.
         */
        if (first) {
            first = 0;
        } else if (seq > expected_seq) {
            uint32_t gap;

            for (gap = expected_seq; gap < seq; gap++)
                as_metrics_lost(m, gap);
        }
        expected_seq = seq + 1;

        as_metrics_recv(m, seq, (size_t)ret, cpu_used);

        if (wolfSSL_write(ssl, buf, AS_HDR_LEN) != AS_HDR_LEN) {
            fprintf(stderr, "Falha ao ecoar a mensagem %u.\n", seq);
            break;
        }
    }

    printf("Associação encerrada.\n");
    wolfSSL_shutdown(ssl);
    rc = 0;

done:
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

    ctx = wolfSSL_CTX_new(wolfDTLSv1_3_server_method());
    if (ctx == NULL) {
        fprintf(stderr, "DTLS 1.3 não disponível nesta build do wolfSSL.\n"
                        "Recompile com ./scripts/setup_wolfssl.sh\n");
        goto out;
    }
    if (as_psk_install_server(ctx, profile, key) != 0)
        goto out;
    memset(key, 0, sizeof(key));

    m = as_metrics_open(profile, "server", out_dir);
    if (m == NULL)
        goto out;

    do {
        if (serve_association(ctx, port, profile, m) != 0 && !keep_running)
            goto out;
    } while (keep_running && !g_stop);

    rc = 0;

out:
    if (m != NULL)
        as_metrics_close(m);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return rc;
}
