/*
 * as_pack.h - leitura e escrita de inteiros em ordem de rede.
 *
 * Os canais C1 e C3 carregam número de sequência e marca de tempo dentro do
 * payload de aplicação. Serializar com memcpy de um uint64_t daria resultados
 * diferentes em máquinas de endianness distinta — irrelevante entre duas VMs
 * x86, mas o testbed vai para Raspberry Pi mais adiante, e o defeito
 * apareceria lá como marca de tempo absurda, não como erro de formato.
 */
#ifndef AS_PACK_H
#define AS_PACK_H

#include <stdint.h>

static inline void as_put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static inline uint32_t as_get_u32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline void as_put_u64(unsigned char *p, uint64_t v)
{
    int i;

    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> ((7 - i) * 8));
}

static inline uint64_t as_get_u64(const unsigned char *p)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

/*
 * Cabeçalho que os geradores de C1 e C3 põem no início de cada mensagem:
 * número de sequência e o instante do envio, no relógio monotônico do emissor.
 *
 * O RTT é calculado pelo próprio emissor quando o eco volta, o que dispensa
 * relógios sincronizados entre as duas VMs — medir atraso unidirecional
 * exigiria PTP, e o erro de sincronismo seria da ordem da grandeza medida.
 */
#define AS_HDR_LEN 12

static inline void as_hdr_put(unsigned char *p, uint32_t seq, uint64_t t_ns)
{
    as_put_u32(p, seq);
    as_put_u64(p + 4, t_ns);
}

#endif /* AS_PACK_H */
