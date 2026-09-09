/*
 * c3_srtp.h - ponte entre o handshake DTLS e a proteção SRTP da mídia.
 *
 * Neste canal o DTLS serve APENAS para estabelecer as chaves. Quem protege os
 * pacotes de mídia é o SRTP: não é a camada de registro do DTLS que carrega o
 * vídeo. Depois do handshake, os pacotes RTP saem protegidos por SRTP no mesmo
 * socket UDP, sem passar pelo DTLS.
 *
 * A extensão use_srtp (RFC 5764) negocia o perfil durante o handshake, e
 * wolfSSL_export_dtls_srtp_keying_material() devolve o material de chave. Para
 * SRTP_AEAD_AES_256_GCM são 88 bytes, dispostos como manda a RFC 5764 §4.2:
 *
 *   [ 0..31]  client_write_SRTP_master_key    (256 bits)
 *   [32..63]  server_write_SRTP_master_key    (256 bits)
 *   [64..75]  client_write_SRTP_master_salt    (96 bits)
 *   [76..87]  server_write_SRTP_master_salt    (96 bits)
 *
 * O libsrtp2 espera chave e sal concatenados, 44 bytes por sentido.
 */
#ifndef C3_SRTP_H
#define C3_SRTP_H

#include <stdio.h>
#include <string.h>

#include <srtp2/srtp.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/*
 * Perfil negociado. Alinha com o TLS_AES_256_GCM_SHA384 dos demais canais, o
 * que mantém a força criptográfica comparável entre os quatro perfis.
 */
#define C3_SRTP_PROFILE "SRTP_AEAD_AES_256_GCM"

#define C3_KEY_LEN      32   /* SRTP_AES_256_KEY_LEN  */
#define C3_SALT_LEN     12   /* SRTP_AEAD_SALT_LEN    */
#define C3_KEYING_LEN   88   /* (256 + 96) * 2 / 8    */
#define C3_KEY_WSALT    (C3_KEY_LEN + C3_SALT_LEN)

/* Cabeçalho RTP fixo (RFC 3550, seção 5.1): sem CSRC nem extensão. */
#define C3_RTP_HDR_LEN  12

/*
 * Monta a chave de um sentido a partir do material exportado.
 * `client_side` seleciona o par do cliente (emissor da mídia).
 */
static inline void c3_key_wsalt(const unsigned char km[C3_KEYING_LEN],
                                int client_side,
                                unsigned char out[C3_KEY_WSALT])
{
    const unsigned char *key  = km + (client_side ? 0 : C3_KEY_LEN);
    const unsigned char *salt = km + (2 * C3_KEY_LEN) +
                                (client_side ? 0 : C3_SALT_LEN);

    memcpy(out, key, C3_KEY_LEN);
    memcpy(out + C3_KEY_LEN, salt, C3_SALT_LEN);
}

/*
 * Extrai o material de chave da sessão DTLS já estabelecida.
 * Retorna 0 em sucesso, negativo em erro.
 */
static inline int c3_export_keying(WOLFSSL *ssl,
                                   unsigned char out[C3_KEYING_LEN])
{
    size_t len = C3_KEYING_LEN;
    int ret;

    ret = wolfSSL_export_dtls_srtp_keying_material(ssl, out, &len);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr,
                "Falha ao exportar o material de chave SRTP (%d).\n"
                "A extensão use_srtp foi negociada? O perfil " C3_SRTP_PROFILE
                " está disponível nas duas pontas?\n", ret);
        return -1;
    }
    if (len != C3_KEYING_LEN) {
        fprintf(stderr,
                "Material de chave com %zu bytes, esperados %d. Perfil SRTP "
                "diferente do previsto.\n", len, C3_KEYING_LEN);
        return -1;
    }
    return 0;
}

/*
 * Cria a sessão SRTP de um sentido.
 *
 * `inbound` escolhe entre proteger o que sai e verificar o que entra.
 * Retorna 0 em sucesso, negativo em erro.
 */
static inline int c3_srtp_session(srtp_t *session,
                                  unsigned char key_wsalt[C3_KEY_WSALT],
                                  int inbound)
{
    srtp_policy_t policy;
    srtp_err_status_t err;

    memset(&policy, 0, sizeof(policy));

    /*
     * _16_auth, e não _8_auth: o perfil SRTP_AEAD_AES_256_GCM da RFC 7714 usa
     * tag de 128 bits. Com a política de 8 bytes o handshake fecharia e a
     * verificação falharia em todo pacote, sem dizer por quê.
     */
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtp);
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtcp);

    policy.ssrc.type = inbound ? ssrc_any_inbound : ssrc_any_outbound;
    policy.key = key_wsalt;
    policy.window_size = 128;
    policy.allow_repeat_tx = 0;
    policy.next = NULL;

    err = srtp_create(session, &policy);
    if (err != srtp_err_status_ok) {
        fprintf(stderr, "srtp_create falhou: %d\n", (int)err);
        return -1;
    }
    return 0;
}

/*
 * Demultiplexação de RFC 5764 §5.1.2: no mesmo socket UDP trafegam registros
 * DTLS e pacotes SRTP, e o primeiro byte os separa. RTP/SRTP tem versão 2 nos
 * dois bits mais altos, o que cai na faixa 128-191; registros DTLS ficam em
 * 20-63. Sem esta separação, um close_notify do DTLS chegando depois do
 * handshake seria entregue ao srtp_unprotect() e contado como pacote corrompido.
 */
static inline int c3_is_srtp(const unsigned char *pkt, size_t len)
{
    return len >= C3_RTP_HDR_LEN && pkt[0] >= 128 && pkt[0] <= 191;
}

#endif /* C3_SRTP_H */
