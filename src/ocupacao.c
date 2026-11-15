/*
 * ocupacao.c — Anti-colisão descentralizada (ponto extra +2): grade de
 *              ocupação e monitor de mão única das passagens.
 *
 * Não cria threads. Quem usa:
 *   - Navegação de CADA robô (processo robo): reservar/liberar células,
 *     entrar/sair das passagens, copiar a grade para desviar;
 *   - Gestor (processo central): reserva a célula inicial do robô novo;
 *   - A* (mapa.c) e ponte MQTT: só LEEM o monitor (custo e exibição);
 *   - simulador: usa estacionamento() para conferir colisões.
 * O Organizador da Logística NÃO usa nada daqui: cada robô decide sozinho
 * se pode andar (por isso é "descentralizado").
 *
 * 1) GRADE (mem->ocupacao): n[y][x] = quantos robôs estão ou reservaram a
 *    célula; grupo[y][x] = de que grupo eles são. Regra para ENTRAR: pode
 *    se n == 0 ou se o grupo da célula é o meu (a equipe anda junta).
 *    Células de ESTACIONAMENTO (baias, zonas, estações e a posição padrão
 *    do Gestor) aceitam vários robôs e não são marcadas: a equipe se
 *    reúne na baia, entrega na zona, vários robôs podem recarregar na
 *    mesma estação e os robôs novos nascem todos na posição padrão. Sem
 *    isso os robôs parados numa zona depois da entrega bloqueariam a
 *    próxima entrega para sempre.
 *    Robô OCIOSO (DISPONIVEL, sem tarefa) fora de um estacionamento vai
 *    sozinho para o mais próximo (estacionamento_ocioso: zona, estação ou
 *    posição padrão; a baia NÃO, ela é só da equipe da caixa). Decisão da
 *    Navegação de cada robô (navegacao.c). Assim um robô parado nunca
 *    ocupa uma célula marcada: não fica no caminho de ninguém.
 *    robos_do_grupo() diz quantos robôs de um grupo estão/reservaram uma
 *    célula: o líder de uma equipe usa para andar em bloco.
 *
 * 2) MONITOR DAS PASSAGENS (mem->monitor[p], problema da "ponte de uma
 *    via"): num túnel de 1 célula de altura, dois robôs em sentidos
 *    opostos se travariam no meio. Regra de entrada:
 *      pode se dentro == 0, OU (sentido == meu E esperando[oposto] == false);
 *      se pode: sem_trywait no semáforo da capacidade; se conseguiu,
 *      dentro++, sentido = meu, esperando[meu] = false;
 *      senão (ou semáforo sem vaga): esperando[meu] = true e o robô tenta
 *      de novo no próximo ciclo.
 *    O esperando[oposto] evita INANIÇÃO: sem ele, um fluxo contínuo num
 *    sentido nunca deixaria dentro chegar a 0 e o outro lado esperaria
 *    para sempre. Com ele, quem chega depois no sentido atual para de
 *    entrar, o túnel esvazia e o outro lado passa.
 *
 * Sincronização:
 *   - mem->ocupacao.mutex (PROCESS_SHARED): um só para a grade inteira;
 *     cada operação testa e marca 1 célula (ou copia a grade, no desvio,
 *     ou faz a BFS do Gestor, poucos microssegundos). Sem ele, dois robôs
 *     veriam a mesma célula livre e entrariam juntos.
 *   - mem->monitor[p].mutex (PROCESS_SHARED): dentro, sentido e
 *     esperando[] mudam juntos. sem_trywait é chamado com ele travado, mas
 *     NÃO bloqueia (retorna na hora), então ninguém fica preso dentro do
 *     mutex. Sem ele, dois robôs de sentidos opostos veriam dentro == 0
 *     ao mesmo tempo e entrariam os dois.
 *   - Nenhuma função daqui trava dois mutexes ao mesmo tempo (regra
 *     anti-deadlock do projeto), e nenhuma espera: tudo é TENTATIVA; quem
 *     não conseguiu tenta no próximo ciclo da Navegação.
 *   - mem->mapa: só leitura (ver mapa.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <semaphore.h>
#include "comum.h"
#include "mapa.h"

/* ============================================================
 * Grade de ocupação
 * ============================================================ */

/* Zona, estação de recarga ou posição padrão: onde um robô ocioso pode
 * ficar parado (ver o topo). */
bool estacionamento_ocioso(int x, int y)
{
    int i;

    for (i = 0; i < N_ZONAS; i++)
        if (x == ZONA_X[i] && y == ZONA_Y[i]) return true;
    return na_estacao(x, y) || (x == POS_PADRAO_X && y == POS_PADRAO_Y);
}

/* Estacionamento (aceita vários robôs, não é marcado na grade): os de
 * robô ocioso e as baias (ver o topo). */
bool estacionamento(int x, int y)
{
    int i;

    for (i = 0; i < N_BAIAS; i++)
        if (x == BAIA_X[i] && y == BAIA_Y[i]) return true;
    return estacionamento_ocioso(x, y);
}

/* Quantos robôs do grupo "grupo" estão ou reservaram (x, y): n da célula
 * se ela é desse grupo, senão 0. Retrato instantâneo da grade, sem o
 * atraso do buffer de status. */
int robos_do_grupo(int x, int y, int grupo)
{
    Ocupacao *o = &mem->ocupacao;
    int n;

    pthread_mutex_lock(&o->mutex);
    n = (o->n[y][x] > 0 && o->grupo[y][x] == grupo) ? o->n[y][x] : 0;
    pthread_mutex_unlock(&o->mutex);
    return n;
}

/* Tenta reservar (x, y) para "grupo". Se não pode, retorna false e diz em
 * *grupo_ocupante quem está lá (para o desempate dos travamentos). */
bool reservar_celula(int x, int y, int grupo, int *grupo_ocupante)
{
    Ocupacao *o = &mem->ocupacao;
    bool pode;

    if (estacionamento(x, y))
        return true;   /* não é verificada nem marcada */
    pthread_mutex_lock(&o->mutex);
    pode = o->n[y][x] == 0 || o->grupo[y][x] == grupo;
    if (pode) {
        o->n[y][x]++;
        o->grupo[y][x] = grupo;
    } else {
        *grupo_ocupante = o->grupo[y][x];
    }
    pthread_mutex_unlock(&o->mutex);
    return pode;
}

/* Devolve uma célula reservada antes (o robô saiu dela). */
void liberar_celula(int x, int y)
{
    Ocupacao *o = &mem->ocupacao;

    if (estacionamento(x, y))
        return;
    pthread_mutex_lock(&o->mutex);
    if (o->n[y][x] > 0)
        o->n[y][x]--;
    pthread_mutex_unlock(&o->mutex);
}

/* Gestor: procura, com BFS (busca em largura) a partir de (*x, *y), a
 * célula válida (posicao_valida) e LIVRE (n == 0) mais próxima, e já a
 * reserva para "grupo" (o id do robô novo). Tudo sob o mutex da grade,
 * para nenhum robô entrar nela entre a escolha e a reserva. A posição
 * padrão é estacionamento (n sempre 0): todo robô pedido sem posição
 * nasce nela, sem se espalhar em volta. Retorna false se não achou
 * nenhuma (não acontece: o mapa tem ~1500 células livres). */
bool reservar_livre_perto(int *x, int *y, int grupo)
{
    static const int DX[4] = {1, -1, 0, 0};
    static const int DY[4] = {0, 0, 1, -1};
    Ocupacao *o = &mem->ocupacao;
    int fila[N_CELULAS];
    bool visto[N_CELULAS];
    int inicio = 0, fim = 0, i, k, cx, cy, vx, vy;
    bool achou = false;

    memset(visto, 0, sizeof(visto));
    visto[*y * MAPA_LARGURA + *x] = true;
    fila[fim++] = *y * MAPA_LARGURA + *x;

    pthread_mutex_lock(&o->mutex);
    while (inicio < fim && !achou) {
        i = fila[inicio++];
        cx = i % MAPA_LARGURA;
        cy = i / MAPA_LARGURA;
        if (posicao_valida(cx, cy) && o->n[cy][cx] == 0) {
            achou = true;
            break;
        }
        for (k = 0; k < 4; k++) {
            vx = cx + DX[k];
            vy = cy + DY[k];
            if (!celula_livre(vx, vy) || visto[vy * MAPA_LARGURA + vx])
                continue;
            visto[vy * MAPA_LARGURA + vx] = true;
            fila[fim++] = vy * MAPA_LARGURA + vx;
        }
    }
    if (achou) {
        *x = cx;
        *y = cy;
        if (!estacionamento(cx, cy)) {
            o->n[cy][cx] = 1;
            o->grupo[cy][cx] = grupo;
        }
    }
    pthread_mutex_unlock(&o->mutex);
    return achou;
}

/* Desvio: copia a grade e marca em ocupada[] (índice y * LARGURA + x) as
 * células onde há robô de OUTRO grupo. A cópia é feita com o mutex e o A*
 * roda depois, sem nenhum mutex travado. */
void copiar_ocupadas(int meu_grupo, bool ocupada[])
{
    Ocupacao *o = &mem->ocupacao;
    int x, y;

    pthread_mutex_lock(&o->mutex);
    for (y = 0; y < MAPA_ALTURA; y++)
        for (x = 0; x < MAPA_LARGURA; x++)
            ocupada[y * MAPA_LARGURA + x] = o->n[y][x] > 0 && o->grupo[y][x] != meu_grupo;
    pthread_mutex_unlock(&o->mutex);
}

/* ============================================================
 * Monitor das passagens (mão única)
 * ============================================================ */

/* Tenta entrar na passagem p no sentido "sentido" (0 ou 1). Retorna true
 * se entrou (e ocupou 1 vaga do semáforo). Nunca espera. */
bool entrar_passagem(int p, int sentido)
{
    MonitorPassagem *m = &mem->monitor[p];
    bool entrou = false;

    pthread_mutex_lock(&m->mutex);
    if (m->dentro == 0 || (m->sentido == sentido && !m->esperando[1 - sentido])) {
        if (sem_trywait(sem_passagem[p]) == 0) {
            m->dentro++;
            m->sentido = sentido;
            m->esperando[sentido] = false;
            entrou = true;
        } else if (errno != EAGAIN) {
            perror("sem_trywait");
            exit(1);
        }
    }
    if (!entrou)
        m->esperando[sentido] = true;
    pthread_mutex_unlock(&m->mutex);
    return entrou;
}

/* Saiu da passagem p (a equipe inteira, no caso do líder). */
void sair_passagem(int p)
{
    MonitorPassagem *m = &mem->monitor[p];

    pthread_mutex_lock(&m->mutex);
    m->dentro--;
    sem_post(sem_passagem[p]);
    pthread_mutex_unlock(&m->mutex);
}

/* Retrato do monitor (para o custo do A* e para a tela). */
void ler_monitor(int p, int *dentro, int *sentido)
{
    MonitorPassagem *m = &mem->monitor[p];

    pthread_mutex_lock(&m->mutex);
    *dentro = m->dentro;
    *sentido = m->sentido;
    pthread_mutex_unlock(&m->mutex);
}
