/*
 * as_signal.h - encerramento limpo dos servidores de canal.
 *
 * signal() na glibc tem semântica BSD e liga SA_RESTART: a chamada de sistema
 * interrompida é REINICIADA automaticamente depois do handler. Um servidor
 * parado em recvfrom() ou accept() nunca chega a testar a flag de parada, e o
 * processo precisa de SIGKILL — o que descarta as métricas, porque o resumo só
 * é gravado no caminho normal de saída.
 *
 * sigaction() com sa_flags = 0 é o que faz a chamada retornar EINTR e o laço
 * do servidor observar o pedido de parada. Custou uma sessão de depuração
 * descobrir isso; não troque de volta por signal().
 */
#ifndef AS_SIGNAL_H
#define AS_SIGNAL_H

#include <signal.h>
#include <string.h>

/*
 * Instala `handler` para SIGINT e SIGTERM sem SA_RESTART, e ignora SIGPIPE
 * (escrever num socket já fechado pelo peer mataria o processo).
 *
 * Retorna 0 em sucesso, negativo em erro.
 */
static inline int as_install_stop_handler(void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* sem SA_RESTART: é o ponto desta função */

    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

#endif /* AS_SIGNAL_H */
