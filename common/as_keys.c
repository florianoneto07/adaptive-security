#include "as_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/hash.h>

/* Converte um dígito hexadecimal em valor, ou -1 se não for hexadecimal. */
static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decodifica exatamente AS_PSK_LEN bytes a partir de `hex`, ignorando espaços
 * em branco à direita (o \n que todo editor acrescenta ao arquivo).
 *
 * `origem` entra só nas mensagens de erro, para que o operador saiba qual das
 * três fontes possíveis está malformada.
 */
static int decode_psk(const char *hex, const char *origem,
                      unsigned char out[AS_PSK_LEN])
{
    size_t len = strlen(hex);
    size_t i;

    while (len > 0 && (hex[len - 1] == '\n' || hex[len - 1] == '\r' ||
                       hex[len - 1] == ' '  || hex[len - 1] == '\t'))
        len--;

    if (len != AS_PSK_LEN * 2) {
        fprintf(stderr,
                "Chave inválida em %s: esperados %d dígitos hexadecimais, "
                "encontrados %zu.\nGere as chaves com "
                "./scripts/generate_keys.sh\n",
                origem, AS_PSK_LEN * 2, len);
        return -1;
    }

    for (i = 0; i < AS_PSK_LEN; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);

        if (hi < 0 || lo < 0) {
            fprintf(stderr,
                    "Chave inválida em %s: caractere não hexadecimal na "
                    "posição %zu.\n", origem, i * 2);
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

/* Lê o arquivo de chave. Retorna 0 em sucesso, negativo em erro. */
static int load_from_file(const char *path, unsigned char out[AS_PSK_LEN])
{
    char buf[AS_PSK_LEN * 2 + 8];
    struct stat st;
    FILE *f;
    size_t n;

    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "Não foi possível abrir a chave %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    /*
     * Uma chave legível por outros usuários da máquina não é uma chave. O
     * aviso não é fatal de propósito: em laboratório é comum copiar o arquivo
     * entre as VMs e perder o modo no caminho, e abortar aqui atrapalharia
     * mais do que ajudaria — mas o operador precisa ver.
     */
    if (fstat(fileno(f), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        fprintf(stderr,
                "Aviso: %s está acessível a outros usuários (modo %04o). "
                "Corrija com: chmod 600 %s\n",
                path, (unsigned)(st.st_mode & 07777), path);
    }

    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    return decode_psk(buf, path, out);
}

int as_key_load(const as_profile_t *profile, unsigned char out[AS_PSK_LEN])
{
    char path[512];
    const char *val;
    const char *dir;

    if (profile == NULL) {
        fprintf(stderr, "as_key_load: perfil nulo.\n");
        return -1;
    }

    /* 1. Variável de ambiente do canal. */
    val = getenv(profile->key_env);
    if (val != NULL && val[0] != '\0')
        return decode_psk(val, profile->key_env, out);

    /* 2. Caminho explícito. */
    val = getenv("AS_KEY_FILE");
    if (val != NULL && val[0] != '\0')
        return load_from_file(val, out);

    /* 3. Diretório de chaves, relativo ao diretório de trabalho por padrão. */
    dir = getenv("AS_KEY_DIR");
    if (dir == NULL || dir[0] == '\0')
        dir = "keys";

    if (snprintf(path, sizeof(path), "%s/%s", dir, profile->key_basename)
            >= (int)sizeof(path)) {
        fprintf(stderr, "Caminho de chave longo demais: %s/%s\n",
                dir, profile->key_basename);
        return -1;
    }

    return load_from_file(path, out);
}

int as_key_fingerprint(const unsigned char key[AS_PSK_LEN],
                       char out[AS_KEY_FP_LEN])
{
    unsigned char digest[WC_SHA256_DIGEST_SIZE];
    int i, ret;

    ret = wc_Sha256Hash(key, AS_PSK_LEN, digest);
    if (ret != 0) {
        fprintf(stderr, "Falha ao calcular o fingerprint da chave: %d\n", ret);
        return -1;
    }

    /* 8 bytes bastam para distinguir quatro chaves e caber numa linha de log. */
    for (i = 0; i < 8; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);

    return 0;
}
