/*
 * sim_stub.c — Simulador-stub do hardware dos robôs (só Etapa 1).
 *
 * Na Etapa 2 ele é substituído pelo simulador em Python (via MQTT).
 *
 * Thread: tarefa_simulador (uma só, no processo central), periódica com
 * período PERIODO_SIMULADOR_MS (100 ms). A cada ciclo, para cada robô ativo:
 *   - se não está em falha e tem bateria, anda 1 célula em direção ao
 *     setpoint (primeiro em x, depois em y) e gasta bateria; com 0% o
 *     robô fica parado (a Status dele detecta e entra em DEFEITO);
 *   - se está numa estação de recarga, recarrega a bateria;
 *   - i_carga_atracada passa a valer o_acoplar_e_erguer (a garra "obedece"
 *     no ciclo seguinte ao comando).
 *
 * Sincronização:
 *   - mutex de cada robô (PTHREAD_PROCESS_SHARED): o slot é lido/escrito
 *     também pelo processo robo (Status e Navegação) e pelo Console; o mutex
 *     garante que x, y, bateria etc. mudem juntos (ninguém lê pela metade).
 *   - clock_nanosleep com tempo absoluto para o período (ver somar_ms).
 *   - celulas_andadas[] só é usado por esta thread: não precisa de mutex.
 */
#include <stdio.h>
#include <stdlib.h>
#include "comum.h"
#include "mapa.h"

#define CELULAS_POR_PORCENTO 5  /* bateria cai 1% a cada 5 células andadas */
#define RECARGA_POR_CICLO    2  /* na estação, sobe 2% por ciclo (100 ms) */

static int celulas_andadas[MAX_ROBOS]; /* só esta thread usa */

/* Anda 1 célula em direção ao setpoint (x primeiro, depois y).
 * Retorna true se andou. Chamada com o mutex do robô travado. */
static bool mover_uma_celula(Hardware *hw)
{
    if (hw->i_posicao_x < hw->o_sp_posicao_x)      hw->i_posicao_x++;
    else if (hw->i_posicao_x > hw->o_sp_posicao_x) hw->i_posicao_x--;
    else if (hw->i_posicao_y < hw->o_sp_posicao_y) hw->i_posicao_y++;
    else if (hw->i_posicao_y > hw->o_sp_posicao_y) hw->i_posicao_y--;
    else return false; /* já está no setpoint */
    return true;
}

/* Um ciclo de simulação de um robô. Chamada com o mutex travado. */
static void simular_robo(int id, Hardware *hw)
{
    if (!hw->i_falha && hw->i_percentual_bateria > 0 && mover_uma_celula(hw)) {
        celulas_andadas[id]++;
        if (celulas_andadas[id] % CELULAS_POR_PORCENTO == 0 &&
            hw->i_percentual_bateria > 0)
            hw->i_percentual_bateria--;
    }
    if (na_estacao(hw->i_posicao_x, hw->i_posicao_y)) {
        hw->i_percentual_bateria += RECARGA_POR_CICLO;
        if (hw->i_percentual_bateria > 100)
            hw->i_percentual_bateria = 100;
    }
    hw->i_carga_atracada = hw->o_acoplar_e_erguer;
}

void *tarefa_simulador(void *arg)
{
    struct timespec proximo;
    int i;

    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        for (i = 0; i < MAX_ROBOS; i++) {
            pthread_mutex_lock(&mem->robos[i].mutex);
            if (mem->robos[i].ativo)
                simular_robo(i, &mem->robos[i].hw);
            pthread_mutex_unlock(&mem->robos[i].mutex);
        }
        somar_ms(&proximo, PERIODO_SIMULADOR_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }
    return NULL;
}
