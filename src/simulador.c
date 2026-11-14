/*
 * simulador.c — Simulador do hardware dos robôs (item 8 do enunciado).
 *
 * Faz o papel do robô físico: lê o setpoint que a Navegação escreve no hw
 * (o_sp_posicao_x/y) e atualiza a posição (i_posicao_x/y), simula a
 * bateria (gasto ao andar e recarga na estação) e a garra
 * (i_carga_atracada responde a o_acoplar_e_erguer).
 *
 * A interface gráfica do simulador (itens 8a–8d: gerar defeito, baixar a
 * bateria, incluir caixas, criar novos robôs) é o painel "Simulação" da
 * Tela de Operação (o enunciado permite que seja a mesma tela). Ele manda
 * comandos por MQTT: atr/comando ("falha", "ok", "bateria", "caixa") e
 * atr/robo/novo (ver ponte_mqtt.c). O simulador roda em C, dentro do
 * processo central, para usar a memória compartilhada diretamente.
 *
 * Thread: tarefa_simulador (uma só, no processo central), periódica com
 * período PERIODO_SIMULADOR_MS (100 ms). A cada ciclo, para cada robô ativo:
 *   - se não está em falha e tem bateria, anda 1 célula em direção ao
 *     setpoint (primeiro em x, depois em y) e gasta bateria; com 0% o
 *     robô fica parado (a Status dele detecta e entra em DEFEITO);
 *   - se está numa estação de recarga, recarrega a bateria;
 *   - i_carga_atracada passa a valer o_acoplar_e_erguer (a garra "obedece"
 *     no ciclo seguinte ao comando).
 * Depois, CONFERE COLISÕES (teste do anti-colisão, ponto extra +2): lê a
 * posição (hw) e o grupo de todos os robôs e, se dois robôs de grupos
 * diferentes estão na mesma célula que não é de estacionamento, registra
 * o evento "COLISAO" (uma vez por encontro). Os cenários 8, 9 e 10 têm de
 * terminar sem nenhum (o rodar_todos.sh confere). Ler o hw aqui é a
 * exceção documentada: o simulador É o hardware.
 *
 * Sincronização:
 *   - mutex de cada robô (PTHREAD_PROCESS_SHARED): o slot é lido/escrito
 *     também pelo processo robo (Status e Navegação) e pelo Console; o mutex
 *     garante que x, y, bateria etc. mudem juntos (ninguém lê pela metade).
 *   - clock_nanosleep com tempo absoluto para o período (ver somar_ms).
 *   - mem->mutex_tempos (PROCESS_SHARED): no fim de cada ciclo grava a
 *     medição (C, atraso, perdas) em mem->med_simulador; o comando "tempos"
 *     lê/zera ao mesmo tempo. Travado sozinho (nenhum mutex de robô junto).
 *   - Com ATR_TEMPO_REAL=1: SCHED_FIFO prioridade 80 (tempo_real.c).
 *   - celulas_andadas[] e em_colisao[][] só são usados por esta thread:
 *     não precisam de mutex.
 *   - conferir colisões: um mutex de robô por vez (copia x, y e grupo
 *     para vetores locais e solta antes do próximo); a comparação é feita
 *     depois, sem nenhum mutex. Regra anti-deadlock: nunca dois juntos.
 */
#include <stdio.h>
#include <stdlib.h>
#include "comum.h"
#include "mapa.h"

/* CELULAS_POR_PORCENTO fica em comum.h: o Organizador e a Status usam o
 * mesmo valor para estimar o gasto de um caminho. */
#define RECARGA_POR_CICLO    2  /* na estação, sobe 2% por ciclo (100 ms) */

static int celulas_andadas[MAX_ROBOS]; /* só esta thread usa */
static bool em_colisao[MAX_ROBOS][MAX_ROBOS]; /* par já registrado (só esta thread) */

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

/* Dois robôs de grupos diferentes na mesma célula (fora das células de
 * estacionamento) = colisão. Registra cada encontro uma vez só. */
static void conferir_colisoes(void)
{
    int x[MAX_ROBOS], y[MAX_ROBOS], grupo[MAX_ROBOS];
    bool ativo[MAX_ROBOS], junto;
    char texto[128];
    int i, j;

    for (i = 0; i < MAX_ROBOS; i++) {
        pthread_mutex_lock(&mem->robos[i].mutex);
        ativo[i] = mem->robos[i].ativo;
        x[i] = mem->robos[i].hw.i_posicao_x;
        y[i] = mem->robos[i].hw.i_posicao_y;
        grupo[i] = mem->robos[i].grupo;
        pthread_mutex_unlock(&mem->robos[i].mutex);
    }
    for (i = 0; i < MAX_ROBOS; i++)
        for (j = i + 1; j < MAX_ROBOS; j++) {
            junto = ativo[i] && ativo[j] && x[i] == x[j] && y[i] == y[j] &&
                    grupo[i] != grupo[j] && !estacionamento(x[i], y[i]);
            if (junto && !em_colisao[i][j]) {
                snprintf(texto, sizeof(texto), "COLISAO: robos %d e %d em (%d,%d)",
                         i, j, x[i], y[i]);
                registrar_evento(fila_coletor, -1, texto);
                printf("Simulador: %s\n", texto);
            }
            em_colisao[i][j] = junto;
        }
}

void *tarefa_simulador(void *arg)
{
    struct timespec proximo, inicio, fim;
    int i;

    (void)arg;
    ativar_tempo_real(PRIO_SIMULADOR, "simulador");
    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        clock_gettime(CLOCK_MONOTONIC, &inicio);
        for (i = 0; i < MAX_ROBOS; i++) {
            pthread_mutex_lock(&mem->robos[i].mutex);
            if (mem->robos[i].ativo)
                simular_robo(i, &mem->robos[i].hw);
            pthread_mutex_unlock(&mem->robos[i].mutex);
        }
        conferir_colisoes();
        clock_gettime(CLOCK_MONOTONIC, &fim);
        pthread_mutex_lock(&mem->mutex_tempos);
        medir(&mem->med_simulador, proximo, inicio, fim, PERIODO_SIMULADOR_MS);
        pthread_mutex_unlock(&mem->mutex_tempos);

        somar_ms(&proximo, PERIODO_SIMULADOR_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }
    return NULL;
}
