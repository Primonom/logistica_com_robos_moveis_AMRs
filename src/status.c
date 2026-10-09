/*
 * status.c — Tarefa "Status do Robô" (thread do processo robo).
 *
 * Thread: tarefa_status, periódica com período PERIODO_STATUS_MS (T2 = 200 ms).
 * A cada ciclo:
 *   1) lê o hardware do seu robô (falha, bateria, posição) e a carga;
 *   2) DEFEITO: com i_falha ou bateria 0%, muda o estado para DEFEITO e
 *      liga a flag "parar" (a Navegação segura a posição). Quando o defeito
 *      some, volta ao estado de antes e desliga "parar";
 *   3) BATERIA: robô DISPONIVEL, sem carga e com bateria < 20% -> liga
 *      "ir_recarregar" (com a estação mais próxima) e muda para
 *      RECARREGANDO. Na estação, com 100%, volta para DISPONIVEL;
 *   4) copia o hw para o BUFFER DE STATUS (e_posicao_x/y, j_sp_posicao_x/y,
 *      bateria, e_estado, distancia_total, destino_x/y, rota) e soma a
 *      distância percorrida. Estado, destino e rota vêm do estado_local
 *      (Navegação).
 *      É a ÚNICA tarefa que escreve nesses campos; o Organizador e o
 *      Console só leem o buffer, nunca o hw.
 *
 * Sincronização:
 *   - estado_local.mutex (mutex comum do processo robo): a Navegação também
 *     escreve no estado; com o mutex, "testar o estado e mudá-lo" é feito
 *     de uma vez só, sem a Navegação mudar o estado no meio.
 *     As flags parar/ir_recarregar são o canal Status -> Navegação.
 *   - mutex do robô (PTHREAD_PROCESS_SHARED, na memória compartilhada): o
 *     simulador (outro processo) escreve no hw enquanto esta thread lê, e o
 *     Console/Organizador leem o buffer enquanto ela escreve; o mutex
 *     garante que o buffer inteiro mude de uma vez (retrato consistente).
 *   - Os dois mutexes nunca são segurados ao mesmo tempo: não há risco de
 *     deadlock por ordem de travamento.
 *   - clock_nanosleep com tempo absoluto para o período (ver somar_ms).
 */
#include <stdio.h>
#include <stdlib.h>
#include "comum.h"
#include "mapa.h"

/* 2) Entra em DEFEITO ou sai dele. */
static void verificar_defeito(bool defeito)
{
    bool entrou = false, saiu = false;

    pthread_mutex_lock(&estado_local.mutex);
    if (defeito && estado_local.estado != DEFEITO) {
        estado_local.estado_antes = estado_local.estado;
        estado_local.estado = DEFEITO;
        estado_local.parar = true;
        entrou = true;
    } else if (!defeito && estado_local.estado == DEFEITO) {
        estado_local.estado = estado_local.estado_antes;
        estado_local.parar = false;
        saiu = true;
    }
    pthread_mutex_unlock(&estado_local.mutex);

    /* Eventos fora do mutex (mq_send pode demorar). */
    if (entrou) registrar_evento(fila_coletor, id_robo, "DEFEITO: robo parado");
    if (saiu)   registrar_evento(fila_coletor, id_robo, "defeito resolvido");
}

/* 3) Manda recarregar ou termina a recarga. */
static void verificar_bateria(int bateria, int carga_id, int x, int y)
{
    bool mandou = false, terminou = false;

    pthread_mutex_lock(&estado_local.mutex);
    if (estado_local.estado == DISPONIVEL && carga_id == -1 &&
        bateria < BATERIA_RECARREGAR) {
        estacao_mais_proxima(x, y, &estado_local.recarga_x, &estado_local.recarga_y);
        estado_local.ir_recarregar = true;
        estado_local.estado = RECARREGANDO;
        mandou = true;
    } else if (estado_local.estado == RECARREGANDO && bateria == 100 &&
               na_estacao(x, y)) {
        estado_local.estado = DISPONIVEL;
        terminou = true;
    }
    pthread_mutex_unlock(&estado_local.mutex);

    if (mandou)    registrar_evento(fila_coletor, id_robo, "bateria baixa: indo recarregar");
    if (terminou)  registrar_evento(fila_coletor, id_robo, "recarga completa: disponivel");
}

/* Rota copiada do estado_local (para levar ao buffer sem segurar os dois
 * mutexes ao mesmo tempo). */
typedef struct {
    int n, x[MAX_ROTA_STATUS], y[MAX_ROTA_STATUS];
} CopiaRota;

/* 4) Copia hw -> buffer de status. Chamada com o mutex do robô travado.
 * estado, destino e rota vêm do estado_local (lidos antes, fora deste mutex). */
static void atualizar_buffer(Robo *r, Estado estado, int destino_x, int destino_y,
                             const CopiaRota *rota)
{
    int k;

    /* O robô anda só na horizontal ou na vertical, então a distância desde
     * o último ciclo é |dx| + |dy| (a posição anterior é a do buffer). */
    r->distancia_total += abs(r->hw.i_posicao_x - r->e_posicao_x)
                        + abs(r->hw.i_posicao_y - r->e_posicao_y);

    r->e_posicao_x    = r->hw.i_posicao_x;
    r->e_posicao_y    = r->hw.i_posicao_y;
    r->j_sp_posicao_x = r->hw.o_sp_posicao_x;
    r->j_sp_posicao_y = r->hw.o_sp_posicao_y;
    r->bateria        = r->hw.i_percentual_bateria;
    r->e_estado       = estado;
    r->destino_x      = destino_x;
    r->destino_y      = destino_y;
    r->n_rota         = rota->n;
    for (k = 0; k < rota->n; k++) {
        r->rota_x[k] = rota->x[k];
        r->rota_y[k] = rota->y[k];
    }
}

void *tarefa_status(void *arg)
{
    Robo *r = &mem->robos[id_robo];
    struct timespec proximo;
    bool falha;
    int bateria, carga_id, x, y, destino_x, destino_y, k;
    Estado estado;
    CopiaRota rota;

    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        /* 1) Lê o que precisa do hw e do slot. */
        pthread_mutex_lock(&r->mutex);
        falha = r->hw.i_falha;
        bateria = r->hw.i_percentual_bateria;
        x = r->hw.i_posicao_x;
        y = r->hw.i_posicao_y;
        carga_id = r->carga_id;
        pthread_mutex_unlock(&r->mutex);

        verificar_defeito(falha || bateria == 0);
        verificar_bateria(bateria, carga_id, x, y);

        pthread_mutex_lock(&estado_local.mutex);
        estado = estado_local.estado;
        destino_x = estado_local.destino_x;
        destino_y = estado_local.destino_y;
        rota.n = estado_local.n_rota;
        for (k = 0; k < rota.n; k++) {
            rota.x[k] = estado_local.rota_x[k];
            rota.y[k] = estado_local.rota_y[k];
        }
        pthread_mutex_unlock(&estado_local.mutex);

        pthread_mutex_lock(&r->mutex);
        atualizar_buffer(r, estado, destino_x, destino_y, &rota);
        pthread_mutex_unlock(&r->mutex);

        somar_ms(&proximo, PERIODO_STATUS_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }
    return NULL;
}
