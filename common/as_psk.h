/*
 * as_psk.h - autenticação por chave pré-compartilhada em TLS 1.3 e DTLS 1.3.
 *
 * Os canais C1, C3 e C4 autenticam por PSK pura, sem certificado. A escolha
 * reproduz o modelo AKMA, em que o K_AF entregue ao Application Function é uma
 * chave simétrica, e mantém os quatro canais comparáveis entre si — o C2 usa
 * OSCORE, que também é simétrico.
 *
 * Duas garantias do requisito R2 são impostas aqui, no contexto, e não deixadas
 * ao acaso da negociação:
 *
 *   - wolfSSL_CTX_require_psk()  impede o handshake de recair em autenticação
 *                                por certificado se a PSK falhar;
 *   - wolfSSL_CTX_only_dhe_psk() exige psk_dhe_ke, mantendo o sigilo futuro.
 *                                Sem ela, o modo psk_ke seria aceitável e o
 *                                comprometimento da PSK abriria todo o tráfego
 *                                gravado anteriormente.
 *
 * O servidor confere que a identidade apresentada é a do SEU canal. É o que dá
 * sentido operacional à separação de chave: um cliente do canal de controle
 * apontado para a porta do canal de missão é recusado na identidade, antes
 * mesmo de a chave ser comparada.
 */
#ifndef AS_PSK_H
#define AS_PSK_H

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "as_keys.h"
#include "as_profile.h"

/*
 * Suíte usada nos handshakes PSK. É a mesma que os binários TLS/DTLS legados
 * já negociavam, de modo que os números de handshake continuam comparáveis
 * com as medições anteriores do repositório.
 */
#define AS_PSK_CIPHERSUITE "TLS13-AES256-GCM-SHA384"

/*
 * Instala a PSK do canal no contexto, do lado do cliente ou do servidor.
 *
 * A chave é copiada para dentro do módulo: o chamador pode limpar seu buffer
 * logo em seguida. Assume um canal por processo, que é como o testbed executa
 * — cada canal é um par de binários independente (R1).
 *
 * Retorna 0 em sucesso, negativo em erro.
 */
int as_psk_install_client(WOLFSSL_CTX *ctx, const as_profile_t *profile,
                          const unsigned char key[AS_PSK_LEN]);
int as_psk_install_server(WOLFSSL_CTX *ctx, const as_profile_t *profile,
                          const unsigned char key[AS_PSK_LEN]);

#endif /* AS_PSK_H */
