/*
 * Canal C4 - Missão, firmware e logs. Perfil fixo: TLS 1.3 sobre TCP/5004.
 *
 * Lado emissor. Gerador de tráfego da classe: transferência de arquivo de
 * tamanho configurável.
 *
 * Fonte do perfil de tráfego: dados de aplicação de UAV chegam a 50 Mbps
 * [3GPP, via arXiv 1903.05289]. A literatura dá a TAXA, não o TAMANHO do
 * arquivo; o padrão de 32 MiB é padrão do testbed, não vem da literatura.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "adaptive_security.h"
#include "as_keys.h"
#include "as_metrics.h"
#include "as_profile.h"
#include "as_psk.h"

#define AS_THIS_CHANNEL AS_CH_C4_BULK

/* Padrões do testbed, não vêm da literatura. */
#define DEFAULT_HOST     "127.0.0.1"
#define DEFAULT_SIZE_MIB 32
#define DEFAULT_BLOCK    16384

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-H host] [-p porta] [-s tamanho_MiB] [-b bloco] [-o dir]\n"
        "  -H  endereço do servidor (padrão %s)\n"
        "  -p  porta (padrão %u)\n"
        "  -s  tamanho da transferência em MiB (padrão %d)\n"
        "  -b  tamanho do bloco em bytes (padrão %d)\n"
        "  -o  diretório dos CSVs de métrica (padrão results)\n",
        prog, DEFAULT_HOST, as_profile_get(AS_THIS_CHANNEL)->port,
        DEFAULT_SIZE_MIB, DEFAULT_BLOCK);
}

int main(int argc, char **argv)
{
    const as_profile_t *profile = as_profile_get(AS_THIS_CHANNEL);
    const char *host = DEFAULT_HOST;
    const char *out_dir = "results";
    int port, opt, ret, rc = 1;
    size_t block = DEFAULT_BLOCK;
    uint64_t total = (uint64_t)DEFAULT_SIZE_MIB * 1024 * 1024;
    uint64_t sent = 0, seq = 0, t_start, cpu_start, t_first_byte;
    int sockfd = -1;
    struct sockaddr_in server_addr;
    unsigned char *buf = NULL, hdr[8];
    unsigned char key[AS_PSK_LEN];
    char fp[AS_KEY_FP_LEN];
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    as_metrics_t *m = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
    port = profile->port;

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

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Porta inválida: %d\n", port);
        return 1;
    }
    if (block == 0 || total == 0) {
        fprintf(stderr, "Tamanho ou bloco inválido (zero).\n");
        return 1;
    }

    if (as_key_load(profile, key) != 0)
        return 1;
    if (as_key_fingerprint(key, fp) != 0)
        return 1;

    printf("Canal %s (%s)\nPerfil fixo: %s, TCP/%d\nChave do canal: %s\n",
           profile->slug, profile->app_class, profile->profile_name, port, fp);

    wolfSSL_Init();

    ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (ctx == NULL) {
        fprintf(stderr, "TLS 1.3 não disponível nesta build do wolfSSL.\n");
        goto out;
    }
    if (as_psk_install_client(ctx, profile, key) != 0)
        goto out;
    memset(key, 0, sizeof(key));

    /*
     * Conteúdo sintético. O gerador não precisa de dados reais: o que importa
     * para as métricas é o volume e a cadência, não o significado dos bytes.
     * Preenchido com um padrão não constante para não favorecer nenhuma
     * otimização de memória do caminho de dados.
     */
    buf = malloc(block);
    if (buf == NULL) {
        fprintf(stderr, "Sem memória para o bloco de %zu bytes.\n", block);
        goto out;
    }
    for (size_t i = 0; i < block; i++)
        buf[i] = (unsigned char)(i * 31 + 7);

    m = as_metrics_open(profile, "client", out_dir);
    if (m == NULL)
        goto out;

    if (as_resolve_v4(host, port, SOCK_STREAM, &server_addr) != 0)
        goto out;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        goto out;
    }
    if (connect(sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect");
        goto out;
    }

    ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        fprintf(stderr, "Erro ao criar a sessão TLS.\n");
        goto out;
    }
    wolfSSL_set_fd(ssl, sockfd);
    if (as_wire_attach(ssl, m, 0) != 0)
        goto out;

    t_start = as_mono_ns();
    cpu_start = as_cpu_ns();

    ret = wolfSSL_connect(ssl);
    if (ret != WOLFSSL_SUCCESS) {
        /* R2: falha em voz alta, sem tentar outro perfil. */
        fprintf(stderr, "Falha no handshake TLS do canal %s: %s\n",
                profile->slug, wolfSSL_ERR_reason_error_string(
                    (unsigned long)wolfSSL_get_error(ssl, ret)));
        goto out;
    }

    as_metrics_handshake(m, as_mono_ns() - t_start, as_cpu_ns() - cpu_start);
    t_first_byte = as_mono_ns();

    printf("Handshake concluído: %s / %s\nTransferindo %llu bytes...\n",
           wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl),
           (unsigned long long)total);

    /* Cabeçalho de tamanho, em ordem de rede. */
    for (int i = 7; i >= 0; i--)
        hdr[i] = (unsigned char)((total >> ((7 - i) * 8)) & 0xFF);
    if (wolfSSL_write(ssl, hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        fprintf(stderr, "Falha ao enviar o cabeçalho de tamanho.\n");
        goto out;
    }

    while (sent < total) {
        size_t want = total - sent;
        uint64_t cpu0;

        if (want > block)
            want = block;

        /*
         * A janela de CPU envolve só a escrita: é dentro dela que o registro é
         * cifrado. Medir a iteração inteira misturaria o custo do gerador com
         * o do perfil de segurança, que é justamente o que se quer separar.
         */
        cpu0 = as_cpu_ns();
        ret = wolfSSL_write(ssl, buf, (int)want);
        if (ret <= 0) {
            fprintf(stderr, "Falha ao enviar após %llu bytes: %s\n",
                    (unsigned long long)sent,
                    wolfSSL_ERR_reason_error_string(
                        (unsigned long)wolfSSL_get_error(ssl, ret)));
            goto out;
        }
        as_metrics_msg(m, seq++, 0, (size_t)ret, as_cpu_ns() - cpu0);
        sent += (uint64_t)ret;
    }

    /* Confirmação do servidor: fecha o RTT da transferência inteira. */
    if (wolfSSL_read(ssl, hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        fprintf(stderr, "Confirmação não recebida.\n");
        goto out;
    }

    {
        uint64_t acked = 0, elapsed = as_mono_ns() - t_first_byte;

        for (int i = 0; i < 8; i++)
            acked = (acked << 8) | hdr[i];

        /* Uma amostra de RTT: da primeira aplicação até a confirmação final. */
        as_metrics_msg(m, seq, elapsed, 0, 0);

        printf("Confirmados %llu de %llu bytes em %.3f s (%.2f Mbps).\n",
               (unsigned long long)acked, (unsigned long long)total,
               elapsed / 1e9,
               elapsed > 0 ? (double)total * 8.0 / (double)elapsed * 1000.0 : 0.0);

        if (acked != total) {
            fprintf(stderr, "Servidor confirmou volume diferente do enviado.\n");
            goto out;
        }
    }

    wolfSSL_shutdown(ssl);
    rc = 0;

out:
    if (m != NULL)
        as_metrics_close(m);
    free(buf);
    if (ssl != NULL)
        wolfSSL_free(ssl);
    if (sockfd >= 0)
        close(sockfd);
    if (ctx != NULL)
        wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return rc;
}
