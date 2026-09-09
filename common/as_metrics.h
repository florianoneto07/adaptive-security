/*
 * as_metrics.h - instrumentação de um canal (requisito R4).
 *
 * Cada processo de canal escreve dois arquivos por execução:
 *
 *   <slug>_<papel>.csv       uma linha por evento, dados brutos
 *   <slug>_<papel>.summary   agregados em "chave=valor", lidos pelo run.sh
 *                            ao montar o manifesto
 *
 * O CSV é bruto de propósito: percentis e razões podem ser recalculados depois
 * com outro critério sem repetir o experimento, o que não seria possível se
 * apenas os agregados fossem gravados.
 *
 * Unidades: nanossegundos para tempo, bytes para volume. A conversão para as
 * unidades da literatura (kbps, ms) fica na leitura, não na coleta.
 *
 * O resumo traz wire_instrumented=1 quando os contadores de fio vêm dos
 * callbacks de I/O do wolfSSL, e wire_instrumented=0 quando o canal não
 * consegue instrumentá-los — é o caso do C2, porque o libcoap gerencia os
 * próprios sockets e não expõe estatística de bytes. Nesse caso os campos
 * wire_* ficam zerados e a captura em pcap é a única fonte, o que precisa
 * ficar evidente para quem ler os números depois.
 */
#ifndef AS_METRICS_H
#define AS_METRICS_H

#include <stddef.h>
#include <stdint.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "as_profile.h"

typedef struct as_metrics as_metrics_t;

/* ------------------------------------------------------------------------- */
/* Relógios                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Relógio monotônico, em nanossegundos. Imune a ajuste de horário — usar
 * CLOCK_REALTIME aqui produziria RTT negativo se o NTP corrigisse o relógio no
 * meio de uma execução de 60 s.
 */
uint64_t as_mono_ns(void);

/*
 * Tempo de CPU consumido por este processo, em nanossegundos.
 *
 * É tempo de processo, não de parede: medir a proteção de uma mensagem com o
 * relógio de parede contaria também o tempo em que o escalonador tirou o
 * processo da CPU, que é ruído puro para a comparação entre perfis.
 */
uint64_t as_cpu_ns(void);

/* Pico de memória residente do processo, em kilobytes (VmHWM). */
uint64_t as_rss_peak_kb(void);

/* ------------------------------------------------------------------------- */
/* Ciclo de vida                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Abre os arquivos de saída em `out_dir`. `role` é "client" ou "server".
 * Retorna NULL em erro, já com a mensagem em stderr.
 */
as_metrics_t *as_metrics_open(const as_profile_t *profile, const char *role,
                              const char *out_dir);

/* Grava o resumo, fecha os arquivos e libera. Retorna 0 em sucesso. */
int as_metrics_close(as_metrics_t *m);

/* ------------------------------------------------------------------------- */
/* Eventos                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Estabelecimento concluído: tempo de parede e de CPU desde o primeiro byte
 * até o handshake fechar. Os bytes trocados no estabelecimento saem dos
 * contadores de fio, se as_wire_attach() estiver em uso.
 */
void as_metrics_handshake(as_metrics_t *m, uint64_t wall_ns, uint64_t cpu_ns);

/*
 * Uma mensagem de aplicação. `rtt_ns` é 0 quando o canal não mede ida e volta
 * (C3 é unidirecional). `cpu_ns` é o custo de proteger e entregar aquela
 * mensagem, medido em volta da chamada de escrita.
 */
void as_metrics_msg(as_metrics_t *m, uint64_t seq, uint64_t rtt_ns,
                    size_t app_bytes, uint64_t cpu_ns);

/*
 * Retransmissão NATIVA do protocolo, distinta do envio inicial. Hoje só o C2
 * a produz: o CoAP confirmável reenvia um objeto que o OSCORE já protegeu, e o
 * custo de CPU dessa repetição é justamente onde OSCORE e DTLS divergem na
 * literatura.
 *
 * C1 e C3 nunca chamam isto porque o DTLS não retransmite dados de aplicação,
 * e o C4 tampouco, porque a retransmissão do TCP acontece abaixo da aplicação
 * e não refaz a cifragem. Essas células ficam vazias no resultado, e isso é o
 * achado, não uma lacuna de instrumentação.
 */
void as_metrics_retx(as_metrics_t *m, uint64_t seq, size_t app_bytes,
                     uint64_t cpu_ns);

/*
 * Amostra de ida e volta de uma mensagem já contabilizada por as_metrics_msg().
 *
 * Separada do envio de propósito: em C1 e C3 o eco chega depois, fora da
 * iteração que enviou. Registrar isso como uma nova mensagem inflaria
 * msgs_sent ao dobro e falsearia a razão entregues/enviados.
 */
void as_metrics_rtt(as_metrics_t *m, uint64_t seq, uint64_t rtt_ns);

/* Mensagem enviada que não chegou ao destino (detectada por lacuna de seq). */
void as_metrics_lost(as_metrics_t *m, uint64_t seq);

/*
 * Mensagem recebida, contada do lado do servidor para a razão de entrega.
 *
 * `cpu_ns` é o custo de verificar e decifrar aquela mensagem, medido em volta
 * da chamada de leitura. Some 0 quando o canal não consegue isolar essa janela.
 */
void as_metrics_recv(as_metrics_t *m, uint64_t seq, size_t app_bytes,
                     uint64_t cpu_ns);

/* ------------------------------------------------------------------------- */
/* Bytes no fio                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Intercepta a entrada e saída do wolfSSL para contar bytes e pacotes no
 * socket, incluindo os do handshake.
 *
 * Conta o que é entregue ao socket, isto é, o registro TLS/DTLS completo — sem
 * os cabeçalhos IP e UDP/TCP, que o processo não vê. O resumo soma esses
 * cabeçalhos por pacote e o registra separadamente, e a captura em pcap serve
 * de conferência independente.
 *
 * Instala na SESSÃO, não no contexto: wolfSSL_new() copia os callbacks de I/O
 * do WOLFSSL_CTX para o WOLFSSL, de modo que mexer no contexto depois de criar
 * a sessão não tem efeito nenhum — e o sintoma é silencioso, contadores
 * zerados no fim da execução.
 *
 * Chamar DEPOIS de wolfSSL_set_fd() e, em DTLS, também depois de
 * wolfSSL_dtls_set_peer(): a função preserva o contexto de I/O que o wolfSSL
 * montou e o repassa às rotinas Embed*, cujo formato difere entre TLS e DTLS.
 *
 * Retorna 0 em sucesso, negativo em erro.
 */
int as_wire_attach(WOLFSSL *ssl, as_metrics_t *m, int is_dtls);

/*
 * Soma bytes que NÃO passam pelo wolfSSL aos contadores de fio.
 *
 * Existe por causa do C3: depois do handshake, os pacotes de mídia são SRTP
 * puro sobre UDP e o wolfSSL não os enxerga, de modo que os callbacks de I/O
 * contariam apenas o handshake. Sem isto, o canal de maior volume ficaria com
 * bytes de fio quase zerados e o overhead por pacote seria incalculável fora
 * do pcap.
 *
 * `pkts` é 1 por datagrama, para que o cálculo de cabeçalhos IP/UDP continue
 * valendo.
 */
void as_metrics_wire_tx(as_metrics_t *m, uint64_t bytes, uint64_t pkts);
void as_metrics_wire_rx(as_metrics_t *m, uint64_t bytes, uint64_t pkts);

#endif /* AS_METRICS_H */
