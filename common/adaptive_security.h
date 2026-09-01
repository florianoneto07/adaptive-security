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

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* ------------------------------------------------------------------------- */
/* Caminhos dos certificados                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Resolve o caminho de um arquivo em certs/. O diretório pode ser trocado pela
 * variável de ambiente CERT_DIR, o que evita depender do diretório de trabalho
 * atual ao rodar os binários de fora da raiz do repositório.
 */
static const char *as_cert_path(const char *filename)
{
    static char path[512];
    const char *dir = getenv("CERT_DIR");

    if (dir == NULL || dir[0] == '\0')
        dir = "certs";

    if (snprintf(path, sizeof(path), "%s/%s", dir, filename) >= (int)sizeof(path)) {
        fprintf(stderr, "CERT_DIR longo demais\n");
        return NULL;
    }
    return path;
}

/* ------------------------------------------------------------------------- */
/* Erros                                                                      */
/* ------------------------------------------------------------------------- */

/* Mensagem legível para o último erro do wolfSSL, em vez do código cru. */
static const char *as_ssl_error(WOLFSSL *ssl, int ret)
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
static int as_is_ip_literal(const char *host)
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
 * Retorna 0 em sucesso, negativo em erro.
 */
static int as_require_peer_identity(WOLFSSL *ssl, const char *host)
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
