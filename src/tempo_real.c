/*
 * tempo_real.c — Prioridades de tempo real (opcional, ATR_TEMPO_REAL=1).
 *
 * Não cria threads. Cada thread periódica (e o Coletor e o Gestor) chama
 * ativar_tempo_real() no início, para ELA MESMA. Usado pelos dois
 * processos (logistica e robo; o robo herda a variável de ambiente pelo
 * execl do Gestor).
 *
 * Sem ATR_TEMPO_REAL=1 não faz nada: todas as threads ficam em
 * SCHED_OTHER (o escalonador normal do Linux, "justo", sem prioridade fixa).
 * Com ATR_TEMPO_REAL=1 tenta SCHED_FIFO com a prioridade Rate Monotonic
 * (comum.h: menor período = maior prioridade). SCHED_FIFO exige permissão
 * (root, CAP_SYS_NICE ou "ulimit -r" > 0); sem ela pthread_setschedparam
 * falha com EPERM, o programa avisa UMA vez e segue em SCHED_OTHER.
 *
 * Sincronização:
 *   - "avisou" (atomic_bool): várias threads podem falhar ao mesmo tempo e
 *     todas tentariam imprimir; atomic_exchange troca para true e devolve o
 *     valor antigo numa operação só, então só a primeira imprime. Sem ele
 *     (bool comum) seria uma condição de corrida e o aviso podia sair
 *     repetido. Não precisa de mutex: é uma variável só, que vai de false
 *     para true.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "comum.h"

/* POR QUE atomic: ver "Sincronização" acima (aviso uma vez só, várias threads). */
static atomic_bool avisou = false;

void ativar_tempo_real(int prioridade, const char *nome)
{
    const char *valor = getenv("ATR_TEMPO_REAL");
    struct sched_param param;
    int ret;

    if (valor == NULL || strcmp(valor, "1") != 0)
        return;
    memset(&param, 0, sizeof(param));
    param.sched_priority = prioridade;
    ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (ret != 0 && !atomic_exchange(&avisou, true))
        printf("Tempo real: SCHED_FIFO negado (%s) na thread %s; seguindo com SCHED_OTHER\n",
               strerror(ret), nome);
}
