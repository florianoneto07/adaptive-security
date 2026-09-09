/*
 * as_keys.h - carga da chave pré-compartilhada de um canal.
 *
 * Cada canal tem sua própria PSK, distinta das demais. Isso simula a separação
 * de chave por aplicação do AKMA: cada classe de aplicação corresponde a um
 * Application Function com seu próprio K_AF, e uma aplicação não alcança a
 * chave de outra.
 *
 * A derivação AKMA propriamente dita está fora de escopo nesta fase. As chaves
 * são INJETADAS, por arquivo ou por variável de ambiente, e geradas por
 * scripts/generate_keys.sh.
 */
#ifndef AS_KEYS_H
#define AS_KEYS_H

#include "as_profile.h"

/*
 * 32 bytes. Não vem de nenhuma fonte da literatura — é padrão do testbed,
 * escolhido para casar com a força do TLS_AES_256_GCM_SHA384 usado nos
 * handshakes e do AES-256-GCM do perfil SRTP do C3.
 */
#define AS_PSK_LEN 32

/* Comprimento do fingerprint hexadecimal devolvido por as_key_fingerprint(). */
#define AS_KEY_FP_LEN 17  /* 16 dígitos + terminador */

/*
 * Carrega a PSK do canal em `out`. Ordem de precedência:
 *
 *   1. a variável de ambiente do canal (AS_C1_KEY, AS_C2_KEY, ...), em hex;
 *   2. AS_KEY_FILE, caminho de arquivo explícito;
 *   3. <AS_KEY_DIR, ou "keys">/<basename do canal>.
 *
 * O arquivo carrega 64 dígitos hexadecimais. Hex, e não binário, para que a
 * chave possa ser conferida a olho e comparada entre as duas VMs sem
 * ferramenta extra — o erro mais comum nesta montagem é as pontas ficarem com
 * chaves diferentes, e isso se manifesta como falha de handshake genérica.
 *
 * Retorna 0 em sucesso. Em erro imprime o motivo em stderr e retorna negativo:
 * o canal deve abortar, nunca seguir com chave ausente ou truncada.
 */
int as_key_load(const as_profile_t *profile, unsigned char out[AS_PSK_LEN]);

/*
 * Escreve em `out` um identificador curto da chave, para o manifesto de
 * reprodutibilidade: os 8 primeiros bytes do SHA-256, em hex.
 *
 * Permite confirmar que as duas VMs usam a mesma chave, e que os quatro canais
 * usam chaves diferentes, sem gravar nenhum segredo no manifesto.
 *
 * Retorna 0 em sucesso, negativo em erro.
 */
int as_key_fingerprint(const unsigned char key[AS_PSK_LEN],
                       char out[AS_KEY_FP_LEN]);

#endif /* AS_KEYS_H */
