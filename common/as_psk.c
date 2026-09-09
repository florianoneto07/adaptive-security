#include "as_psk.h"

#include <stdio.h>
#include <string.h>

/*
 * Estado do canal deste processo. Um canal por processo é premissa do testbed
 * (R1: cada canal é um par de binários independente), então uma única chave e
 * uma única identidade por imagem em execução bastam.
 *
 * As callbacks de PSK do wolfSSL recebem o WOLFSSL*, e daria para pendurar
 * este estado nele com wolfSSL_set_psk_callback_ctx(). Não vale a indireção
 * enquanto a premissa acima valer; se um dia um processo servir dois canais,
 * é aqui que a mudança precisa acontecer.
 */
static struct {
    unsigned char key[AS_PSK_LEN];
    char          identity[128];
    int           ready;
} g_psk;

/*
 * Callback do cliente (TLS 1.3 e DTLS 1.3).
 *
 * Devolve o tamanho da chave; 0 aborta o handshake. `hint` vem do servidor e é
 * ignorado de propósito: o perfil é fixo, e deixar o servidor influenciar qual
 * chave o cliente usa seria uma forma de negociação — proibida pelo R2.
 */
static unsigned int psk_client_cb(WOLFSSL *ssl, const char *hint,
                                  char *identity, unsigned int id_max_len,
                                  unsigned char *key, unsigned int key_max_len,
                                  const char **ciphersuite)
{
    (void)ssl;
    (void)hint;

    if (!g_psk.ready || key_max_len < AS_PSK_LEN)
        return 0;

    if (strlen(g_psk.identity) + 1 > id_max_len)
        return 0;

    strcpy(identity, g_psk.identity);
    memcpy(key, g_psk.key, AS_PSK_LEN);
    *ciphersuite = AS_PSK_CIPHERSUITE;

    return AS_PSK_LEN;
}

/*
 * Callback do servidor (TLS 1.3 e DTLS 1.3).
 *
 * Recusa qualquer identidade que não seja a do canal. Sem esta comparação, um
 * cliente de outro canal seria rejeitado só mais adiante, ao falhar a
 * verificação do binder, e o log não diria que o problema foi canal trocado.
 */
static unsigned int psk_server_cb(WOLFSSL *ssl, const char *identity,
                                  unsigned char *key, unsigned int key_max_len,
                                  const char **ciphersuite)
{
    (void)ssl;

    if (!g_psk.ready || key_max_len < AS_PSK_LEN)
        return 0;

    if (identity == NULL || strcmp(identity, g_psk.identity) != 0) {
        fprintf(stderr,
                "PSK recusada: identidade \"%s\" não pertence a este canal "
                "(esperada \"%s\").\n",
                identity != NULL ? identity : "(nula)", g_psk.identity);
        return 0;
    }

    memcpy(key, g_psk.key, AS_PSK_LEN);
    *ciphersuite = AS_PSK_CIPHERSUITE;

    return AS_PSK_LEN;
}

/* Guarda chave e identidade, e aplica as travas de perfil comuns aos dois lados. */
static int psk_prepare(WOLFSSL_CTX *ctx, const as_profile_t *profile,
                       const unsigned char key[AS_PSK_LEN])
{
    if (ctx == NULL || profile == NULL || key == NULL) {
        fprintf(stderr, "as_psk: argumento nulo.\n");
        return -1;
    }
    if (profile->psk_identity == NULL) {
        fprintf(stderr,
                "Canal %s não usa PSK de handshake (perfil %s).\n",
                profile->slug, profile->profile_name);
        return -1;
    }
    if (strlen(profile->psk_identity) >= sizeof(g_psk.identity)) {
        fprintf(stderr, "Identidade PSK longa demais: %s\n",
                profile->psk_identity);
        return -1;
    }

    memcpy(g_psk.key, key, AS_PSK_LEN);
    strcpy(g_psk.identity, profile->psk_identity);
    g_psk.ready = 1;

    /*
     * R2: sem fallback para certificado, e sigilo futuro obrigatório.
     *
     * Estas duas devolvem 0 em sucesso, e não WOLFSSL_SUCCESS (que vale 1) —
     * conferido em src/tls13.c da versão em uso, não presumido pelo nome. Além
     * de exigir a PSK, require_psk() desliga o downgrade de versão, para que um
     * contexto capaz de cair para (D)TLS 1.2 não conclua sem PSK nenhuma.
     */
    if (wolfSSL_CTX_require_psk(ctx) != 0) {
        fprintf(stderr, "Não foi possível exigir PSK no contexto.\n");
        return -1;
    }
    if (wolfSSL_CTX_only_dhe_psk(ctx) != 0) {
        fprintf(stderr,
                "Não foi possível exigir psk_dhe_ke. Sem ele o handshake "
                "perderia o sigilo futuro, e o perfil deixaria de ser o que o "
                "canal declara.\n");
        return -1;
    }

    return 0;
}

int as_psk_install_client(WOLFSSL_CTX *ctx, const as_profile_t *profile,
                          const unsigned char key[AS_PSK_LEN])
{
    if (psk_prepare(ctx, profile, key) != 0)
        return -1;

    wolfSSL_CTX_set_psk_client_tls13_callback(ctx, psk_client_cb);
    return 0;
}

int as_psk_install_server(WOLFSSL_CTX *ctx, const as_profile_t *profile,
                          const unsigned char key[AS_PSK_LEN])
{
    if (psk_prepare(ctx, profile, key) != 0)
        return -1;

    /*
     * A dica de identidade é o que permite ao cliente escolher entre várias
     * chaves. Aqui ela é informativa: cada canal tem uma só.
     */
    if (wolfSSL_CTX_use_psk_identity_hint(ctx, profile->psk_identity)
            != WOLFSSL_SUCCESS) {
        fprintf(stderr, "Não foi possível anunciar a dica de identidade PSK.\n");
        return -1;
    }

    wolfSSL_CTX_set_psk_server_tls13_callback(ctx, psk_server_cb);
    return 0;
}
