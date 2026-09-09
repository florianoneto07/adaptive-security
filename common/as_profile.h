/*
 * as_profile.h - os quatro perfis de segurança fixos do testbed.
 *
 * Cada classe de aplicação do UAV recebe um perfil de segurança em projeto. A
 * atribuição NÃO é negociada em tempo de execução, e essa restrição é
 * deliberada: a tabela abaixo é `const`, cada binário de canal escolhe sua
 * entrada em tempo de compilação, e não existe caminho de código que troque o
 * perfil de um canal pelo de outro.
 *
 * A consequência prática é o requisito R2: se o perfil configurado falhar, o
 * canal falha em voz alta. Não há fallback para um perfil mais fraco, porque
 * um fallback silencioso é indistinguível, do lado de fora, de um ataque de
 * rebaixamento bem-sucedido.
 *
 * Racional de cada atribuição está no README.md, com as fontes.
 */
#ifndef AS_PROFILE_H
#define AS_PROFILE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------------- */
/* Identificadores                                                            */
/* ------------------------------------------------------------------------- */

typedef enum {
    AS_CH_C1_CONTROL   = 1,
    AS_CH_C2_TELEMETRY = 2,
    AS_CH_C3_MEDIA     = 3,
    AS_CH_C4_BULK      = 4
} as_channel_t;

typedef enum {
    AS_PROF_DTLS13 = 0,   /* DTLS 1.3 protegendo a camada de registro        */
    AS_PROF_OSCORE,       /* OSCORE (RFC 8613) protegendo o objeto CoAP      */
    AS_PROF_DTLS_SRTP,    /* DTLS só deriva chave; SRTP protege a mídia      */
    AS_PROF_TLS13         /* TLS 1.3 protegendo a camada de registro         */
} as_profile_kind_t;

/* ------------------------------------------------------------------------- */
/* Descrição de um canal                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    as_channel_t      channel;
    const char       *slug;         /* usado em nomes de arquivo: "c1_control" */
    const char       *app_class;    /* texto para logs e para o manifesto      */
    as_profile_kind_t profile;
    const char       *profile_name;
    int               socktype;     /* SOCK_DGRAM ou SOCK_STREAM               */
    uint16_t          port;
    const char       *key_basename; /* arquivo dentro de keys/                 */
    const char       *key_env;      /* variável de ambiente alternativa        */
    const char       *psk_identity; /* identidade PSK anunciada no handshake   */
} as_profile_t;

/*
 * A separação de chave por canal é o que simula o AKMA: cada classe de
 * aplicação corresponde a um Application Function com seu próprio K_AF, e uma
 * aplicação não alcança a chave de outra. Daí um arquivo e uma identidade PSK
 * distintos por linha — nunca um segredo comum com rótulos diferentes.
 *
 * C2 não usa identidade PSK porque OSCORE não tem handshake: o contexto de
 * segurança é derivado direto do master secret (RFC 8613, seção 3.2).
 */
static const as_profile_t as_profile_table[] = {
    { AS_CH_C1_CONTROL, "c1_control",
      "Comando e controle (C2)",
      AS_PROF_DTLS13,    "DTLS 1.3",
      SOCK_DGRAM,  5001, "c1.key", "AS_C1_KEY", "uav-c1-control"   },

    { AS_CH_C2_TELEMETRY, "c2_telemetry",
      "Telemetria periódica",
      AS_PROF_OSCORE,    "OSCORE sobre CoAP",
      SOCK_DGRAM,  5002, "c2.key", "AS_C2_KEY", NULL               },

    { AS_CH_C3_MEDIA, "c3_media",
      "Vídeo e áudio em tempo real",
      AS_PROF_DTLS_SRTP, "handshake DTLS 1.3 + SRTP na mídia",
      SOCK_DGRAM,  5003, "c3.key", "AS_C3_KEY", "uav-c3-media"     },

    { AS_CH_C4_BULK, "c4_bulk",
      "Missão, firmware, logs",
      AS_PROF_TLS13,     "TLS 1.3",
      SOCK_STREAM, 5004, "c4.key", "AS_C4_KEY", "uav-c4-bulk"      },
};

#define AS_PROFILE_COUNT \
    (sizeof(as_profile_table) / sizeof(as_profile_table[0]))

/*
 * Devolve o perfil do canal, ou NULL se o identificador não existir.
 *
 * Os binários chamam isto com uma constante de compilação, de modo que NULL
 * aqui significa tabela e binário fora de sincronia — um erro de programação,
 * não uma condição de execução.
 */
static inline const as_profile_t *as_profile_get(as_channel_t channel)
{
    size_t i;

    for (i = 0; i < AS_PROFILE_COUNT; i++) {
        if (as_profile_table[i].channel == channel)
            return &as_profile_table[i];
    }
    return NULL;
}

#endif /* AS_PROFILE_H */
