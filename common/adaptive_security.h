/*
 * adaptive_security.h - utilidades compartilhadas pelos binários do protótipo.
 *
 * Header-only de propósito: cada binário é uma única unidade de tradução, e
 * assim o Makefile continua compilando cada alvo com um comando só.
 *
 * O que mora aqui é o que NÃO pode divergir entre cliente TLS e cliente DTLS,
 * em especial a amarração da identidade do peer ao certificado apresentado.
 */
#ifndef ADAPTIVE_SECURITY_H
#define ADAPTIVE_SECURITY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* ------------------------------------------------------------------------- */
/* Caminhos dos certificados                                                  */
/* ------------------------------------------------------------------------- */

/* Tamanho recomendado para os buffers passados a as_cert_path(). */
#define AS_PATH_MAX 512

/*
 * Resolve o caminho de um arquivo em certs/ dentro do buffer do chamador. O
 * diretório pode ser trocado pela variável de ambiente CERT_DIR, o que evita
 * depender do diretório de trabalho atual ao rodar os binários de fora da raiz
 * do repositório.
 *
 * O buffer é do chamador de propósito: com um buffer estático interno, duas
 * chamadas seguidas devolveriam o mesmo ponteiro e a primeira seria
 * silenciosamente sobrescrita pela segunda.
 *
 * Retorna `out` em sucesso, NULL se o caminho não couber.
 */
static inline const char *as_cert_path(char *out, size_t out_sz,
                                       const char *filename)
{
    const char *dir = getenv("CERT_DIR");

    if (dir == NULL || dir[0] == '\0')
        dir = "certs";

    if (snprintf(out, out_sz, "%s/%s", dir, filename) >= (int)out_sz) {
        fprintf(stderr, "Caminho de certificado longo demais: %s/%s\n",
                dir, filename);
        return NULL;
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Resolução de endereço                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Resolve host + porta em um sockaddr_in. Aceita tanto literal IPv4 quanto
 * hostname, para que a verificação de identidade por nome seja utilizável.
 *
 * `socktype` é SOCK_STREAM (TLS) ou SOCK_DGRAM (DTLS).
 * Retorna 0 em sucesso, negativo em erro.
 */
static inline int as_resolve_v4(const char *host, int port, int socktype,
                                struct sockaddr_in *out)
{
    struct addrinfo hints, *res = NULL;
    char port_str[16];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;

    snprintf(port_str, sizeof(port_str), "%d", port);

    rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Não foi possível resolver %s: %s\n",
                host, gai_strerror(rc));
        return -1;
    }

    memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Erros                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Mensagem legível para o último erro do wolfSSL, em vez do código cru.
 * Usa buffer estático como strerror(): válido até a próxima chamada, portanto
 * não use dois resultados na mesma expressão.
 */
static inline const char *as_ssl_error(WOLFSSL *ssl, int ret)
{
    static char buf[WOLFSSL_MAX_ERROR_SZ];
    int err = wolfSSL_get_error(ssl, ret);

    wolfSSL_ERR_error_string((unsigned long)err, buf);
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Identidade do peer                                                         */
/* ------------------------------------------------------------------------- */

/* Verdadeiro se a string for um literal IPv4 ou IPv6. */
static inline int as_is_ip_literal(const char *host)
{
    unsigned char raw[sizeof(struct in6_addr)];

    return inet_pton(AF_INET, host, raw) == 1 ||
           inet_pton(AF_INET6, host, raw) == 1;
}

/*
 * Exige que o certificado do servidor pertença de fato a `host`.
 *
 * Validar a cadeia contra a CA responde "este certificado foi emitido pela CA
 * em que confio". Não responde "este certificado é do host com quem estou
 * falando" — sem esta checagem, qualquer certificado assinado pela mesma CA é
 * aceito, e um peer interno consegue se passar por outro. É uma distinção que
 * passa despercebida em laboratório e que importa no cenário AKMA/5G.
 *
 * `host` é o endereço que o usuário pediu, não o resultado da resolução DNS:
 * verificar contra o IP resolvido tornaria a checagem circular.
 *
 * Retorna 0 em sucesso, negativo em erro.
 */
static inline int as_require_peer_identity(WOLFSSL *ssl, const char *host)
{
    if (as_is_ip_literal(host)) {
        /*
         * Endereço IP: precisa casar com um subjectAltName do tipo iPAddress.
         * wolfSSL_check_domain_name() compara com dNSName, então não serve
         * aqui; a checagem de IP vive no X509_VERIFY_PARAM.
         */
#ifdef OPENSSL_EXTRA
        WOLFSSL_X509_VERIFY_PARAM *param = wolfSSL_get0_param(ssl);

        if (param == NULL) {
            fprintf(stderr, "Não foi possível obter o X509_VERIFY_PARAM.\n");
            return -1;
        }
        if (wolfSSL_X509_VERIFY_PARAM_set1_ip_asc(param, host) != WOLFSSL_SUCCESS) {
            fprintf(stderr, "Falha ao exigir o SAN de IP %s.\n", host);
            return -1;
        }
        return 0;
#else
        (void)ssl;
        fprintf(stderr,
                "Build do wolfSSL sem OPENSSL_EXTRA: não é possível validar o "
                "SAN de IP.\nRecompile o wolfSSL com --enable-opensslextra "
                "C_EXTRA_FLAGS=\"-DWOLFSSL_IP_ALT_NAME\".\n");
        return -1;
#endif
    }

    /* Hostname: casa contra dNSName (e CN, conforme a build do wolfSSL). */
    if (wolfSSL_check_domain_name(ssl, host) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Falha ao exigir o nome de domínio %s.\n", host);
        return -1;
    }
    return 0;
}

#endif /* ADAPTIVE_SECURITY_H */
