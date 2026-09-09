/*
 * c2_oscore.h - contexto de segurança OSCORE derivado da chave do canal C2.
 *
 * OSCORE (RFC 8613) não tem handshake: as duas pontas partem de um Master
 * Secret comum e derivam dele as chaves de sentido. O libcoap recebe esse
 * contexto como um texto de configuração; aqui ele é montado em memória a
 * partir da PSK do canal, em vez de vir de arquivo, para que a chave do C2
 * tenha exatamente o mesmo tratamento das dos outros canais — uma chave por
 * classe de aplicação, injetada, sem derivação AKMA nesta fase.
 *
 * O Master Salt é derivado da própria PSK, e não sorteado: as duas VMs precisam
 * chegar ao mesmo valor sem trocar nada além da chave. Sortear exigiria
 * distribuir um segundo segredo.
 *
 * A derivação usa um rótulo de separação de domínio ("AS-OSCORE-SALT"). Sem
 * ele o salt sairia igual ao fingerprint que as_key_fingerprint() imprime em
 * log e grava no manifesto — os dois são os primeiros 8 bytes de um SHA-256 da
 * mesma chave. O salt não é secreto na RFC 8613, mas publicar por acidente um
 * valor que também serve de identificador confunde a leitura dos resultados.
 */
#ifndef C2_OSCORE_H
#define C2_OSCORE_H

#include <stdio.h>
#include <string.h>

#include <coap3/coap.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/hash.h>

#include "as_keys.h"

/*
 * Sender ID de um lado é o Recipient ID do outro. Trocá-los é o erro de
 * configuração mais comum, e se manifesta como falha de decifragem, não como
 * erro de configuração — bem mais penoso de diagnosticar.
 */
#define C2_SERVER_ID "01"
#define C2_CLIENT_ID "02"

/* Separação de domínio: mantém o salt distinto do fingerprint da chave. */
#define C2_SALT_LABEL "AS-OSCORE-SALT"

/* Espaço suficiente para o texto de configuração completo. */
#define C2_CONF_MAX 512

static inline void c2_hex(const unsigned char *in, size_t n, char *out)
{
    size_t i;

    for (i = 0; i < n; i++)
        snprintf(out + i * 2, 3, "%02x", in[i]);
}

/*
 * Monta o texto de configuração OSCORE em `buf`.
 *
 * `sender` e `recipient` são C2_SERVER_ID/C2_CLIENT_ID conforme o papel.
 * Retorna o comprimento escrito, ou 0 em erro.
 *
 * aead_alg 10 = AES-CCM-16-64-128, o AEAD obrigatório da RFC 8613.
 * hkdf_alg -10 = direct+HKDF-SHA-256.
 *
 * rfc8613_b_1_2 desligado (o padrão do libcoap é ligado). O Apêndice B.1.2 da
 * RFC 8613 manda o servidor desafiar com a opção Echo (RFC 9175) a primeira
 * requisição de um cliente cujo número de sequência ele ainda não conhece, para
 * barrar repetição de mensagens antigas depois de o servidor reiniciar.
 *
 * O que se perde: exatamente essa proteção — replay de mensagens capturadas
 * antes de um reinício do servidor. A janela anti-replay de 32 continua valendo
 * dentro da sessão, que é a defesa que importa durante uma execução.
 *
 * Por que aqui: com o desafio ligado, o libcoap desta versão enfileira o
 * reenvio que carrega o Echo em session->doing_first (coap_net.c, trecho de
 * oscore_encryption) e nunca o transmite, travando qualquer cliente que mande
 * mais de uma requisição. As chaves deste testbed são estáticas e injetadas, e
 * o servidor não reinicia no meio de uma execução, então o cenário que o B.1.2
 * cobre não ocorre. Para religar, troque o valor por true — ciente de que o canal
 * C2 passa a atender uma requisição por execução.
 */
static inline size_t c2_oscore_conf_text(const unsigned char key[AS_PSK_LEN],
                                         const char *sender,
                                         const char *recipient,
                                         char *buf, size_t buf_sz)
{
    unsigned char digest[WC_SHA256_DIGEST_SIZE];
    unsigned char salt_in[sizeof(C2_SALT_LABEL) - 1 + AS_PSK_LEN];
    char secret_hex[AS_PSK_LEN * 2 + 1];
    char salt_hex[17];
    int n;

    memcpy(salt_in, C2_SALT_LABEL, sizeof(C2_SALT_LABEL) - 1);
    memcpy(salt_in + sizeof(C2_SALT_LABEL) - 1, key, AS_PSK_LEN);

    if (wc_Sha256Hash(salt_in, (word32)sizeof(salt_in), digest) != 0) {
        fprintf(stderr, "Falha ao derivar o Master Salt do OSCORE.\n");
        return 0;
    }

    c2_hex(key, AS_PSK_LEN, secret_hex);
    c2_hex(digest, 8, salt_hex);

    n = snprintf(buf, buf_sz,
                 "master_secret,hex,\"%s\"\n"
                 "master_salt,hex,\"%s\"\n"
                 "sender_id,hex,\"%s\"\n"
                 "recipient_id,hex,\"%s\"\n"
                 "replay_window,integer,32\n"
                 "aead_alg,integer,10\n"
                 "hkdf_alg,integer,-10\n"
                 "rfc8613_b_1_2,bool,false\n",
                 secret_hex, salt_hex, sender, recipient);

    /* Não deixa a chave no stack depois de copiada para o texto. */
    memset(secret_hex, 0, sizeof(secret_hex));

    if (n < 0 || (size_t)n >= buf_sz) {
        fprintf(stderr, "Texto de configuração OSCORE não coube.\n");
        return 0;
    }
    return (size_t)n;
}

/*
 * Constrói o coap_oscore_conf_t do papel indicado. Devolve NULL em erro, com
 * a mensagem já em stderr. O chamador não precisa liberar: as funções do
 * libcoap que o recebem assumem a posse.
 */
static inline coap_oscore_conf_t *c2_oscore_conf(const unsigned char key[AS_PSK_LEN],
                                                 int is_server)
{
    char text[C2_CONF_MAX];
    coap_str_const_t conf_mem;
    coap_oscore_conf_t *conf;
    size_t len;

    len = c2_oscore_conf_text(key,
                              is_server ? C2_SERVER_ID : C2_CLIENT_ID,
                              is_server ? C2_CLIENT_ID : C2_SERVER_ID,
                              text, sizeof(text));
    if (len == 0)
        return NULL;

    conf_mem.s = (const uint8_t *)text;
    conf_mem.length = len;

    conf = coap_new_oscore_conf(conf_mem, NULL, NULL, 0);
    memset(text, 0, sizeof(text));

    if (conf == NULL)
        fprintf(stderr, "libcoap recusou a configuração OSCORE.\n");

    return conf;
}

#endif /* C2_OSCORE_H */
