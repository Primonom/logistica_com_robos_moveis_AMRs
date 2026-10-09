/*
 * mapa.h — Mapa do galpão (grade 60 x 30, vista de cima) e rotas.
 *
 * x cresce para a direita e y para baixo. Só dados e cálculo de rota
 * (sem threads e sem sincronização; os semáforos das passagens são usados
 * pela Navegação, em navegacao.c).
 */
#ifndef MAPA_H
#define MAPA_H

#include <stdbool.h>

#define MAPA_LARGURA 60
#define MAPA_ALTURA  30
#define CORREDOR_Y   15   /* corredor principal horizontal */

/* Baias 1-4 (onde as caixas estão) e zonas de entrega A-D.
 * Índice 0 = baia 1 / zona A. */
#define N_BAIAS 4
#define N_ZONAS 4
static const int BAIA_X[N_BAIAS] = { 3,  3,  3,  3};
static const int BAIA_Y[N_BAIAS] = { 4, 11, 18, 25};
static const int ZONA_X[N_ZONAS] = {56, 56, 56, 56};
static const int ZONA_Y[N_ZONAS] = { 4, 11, 18, 25};

/* Estações de recarga: (45, 1) e (45, 28). */
#define N_ESTACOES 2
static const int ESTACAO_X[N_ESTACOES] = {45, 45};
static const int ESTACAO_Y[N_ESTACOES] = { 1, 28};

/* Passagens estreitas. O índice (0, 1, 2) é usado nos vetores; o nome do
 * semáforo usa o número da passagem (1, 2, 3): /atr_passagem_1 etc. */
#define N_PASSAGENS 3
#define P1 0   /* colunas x 14..16, atravessada no corredor */
#define P2 1   /* colunas x 39..41, atravessada no corredor */
#define P3 2   /* coluna x = 45, y 2..13 e 17..27: acesso às estações */
static const int CAPACIDADE_PASSAGEM[N_PASSAGENS] = {3, 2, 1};

#define P1_X_MIN 14
#define P1_X_MAX 16
#define P2_X_MIN 39
#define P2_X_MAX 41
#define P3_X         45
#define P3_CIMA_Y_MIN   2
#define P3_CIMA_Y_MAX  13
#define P3_BAIXO_Y_MIN 17
#define P3_BAIXO_Y_MAX 27

/* Um ponto da rota. "passagem" = índice da passagem que o robô precisa
 * OCUPAR para ir até este ponto (-1 = nenhuma). O robô pega o semáforo
 * antes de seguir para o ponto e devolve ao chegar nele (já fora da
 * passagem). */
typedef struct {
    int x, y;
    int passagem;
} Waypoint;

#define MAX_WAYPOINTS 16
typedef struct {
    Waypoint pontos[MAX_WAYPOINTS];
    int n;
} Rota;

void calcular_rota(int x0, int y0, int xd, int yd, Rota *rota);
bool dentro_de_passagem(int x, int y);
bool posicao_valida(int x, int y);  /* dentro do mapa e fora das passagens */
bool na_estacao(int x, int y);       /* (x, y) é uma estação de recarga? */
void estacao_mais_proxima(int x, int y, int *ex, int *ey);

#endif
