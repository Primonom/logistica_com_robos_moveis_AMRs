/*
 * mapa.c — Cálculo de rotas no galpão.
 *
 * Não cria threads e não usa sincronização: são funções puras, chamadas
 * pela Navegação (processo robo) e pelo Console (validação de posição).
 *
 * Rota simples, sempre pelo corredor principal (y = CORREDOR_Y):
 *   1) anda na vertical da origem até o corredor;
 *   2) anda no corredor, na horizontal, até o x do destino;
 *   3) anda na vertical até o destino.
 * (Se origem e destino estão na mesma coluna, vai direto na vertical.)
 * Cada trecho anda num só eixo, então o simulador (que anda primeiro em x,
 * depois em y) segue exatamente a rota.
 *
 * Quando um trecho atravessa uma passagem estreita, são inseridos dois
 * pontos: um logo ANTES da passagem (onde o robô espera o semáforo) e um
 * logo DEPOIS (marcado com a passagem; ao chegar nele o robô a libera).
 * Assim um robô nunca segura mais de uma passagem ao mesmo tempo.
 *
 * Origem e destino nunca ficam dentro de uma passagem (posicao_valida),
 * então um trecho ou atravessa a passagem inteira ou não toca nela.
 */
#include <stdlib.h>
#include "comum.h"
#include "mapa.h"

static void adicionar(Rota *rota, int x, int y, int passagem)
{
    if (rota->n < MAX_WAYPOINTS) {
        rota->pontos[rota->n].x = x;
        rota->pontos[rota->n].y = y;
        rota->pontos[rota->n].passagem = passagem;
        rota->n++;
    }
}

/* Indo de "a" até "b" (numa dimensão), atravessa a faixa [min, max]?
 * Se sim, adiciona o ponto antes da faixa e o ponto depois dela.
 * "fixo" é a outra coordenada (y se horizontal, x se vertical). */
static void atravessar_faixa(Rota *rota, bool horizontal, int fixo,
                             int a, int b, int min, int max, int passagem)
{
    int antes, depois;

    if (a < min && b > max) {        /* indo no sentido crescente */
        antes = min - 1;
        depois = max + 1;
    } else if (a > max && b < min) { /* indo no sentido decrescente */
        antes = max + 1;
        depois = min - 1;
    } else {
        return;                      /* não atravessa */
    }
    if (horizontal) {
        adicionar(rota, antes, fixo, -1);
        adicionar(rota, depois, fixo, passagem);
    } else {
        adicionar(rota, fixo, antes, -1);
        adicionar(rota, fixo, depois, passagem);
    }
}

/* Trecho horizontal no corredor, de xa até xb. */
static void trecho_horizontal(Rota *rota, int xa, int xb)
{
    int y = CORREDOR_Y;

    if (xa < xb) { /* para a direita: encontra P1 antes de P2 */
        atravessar_faixa(rota, true, y, xa, xb, P1_X_MIN, P1_X_MAX, P1);
        atravessar_faixa(rota, true, y, xa, xb, P2_X_MIN, P2_X_MAX, P2);
    } else {       /* para a esquerda: encontra P2 antes de P1 */
        atravessar_faixa(rota, true, y, xa, xb, P2_X_MIN, P2_X_MAX, P2);
        atravessar_faixa(rota, true, y, xa, xb, P1_X_MIN, P1_X_MAX, P1);
    }
    adicionar(rota, xb, y, -1);
}

/* Trecho vertical na coluna x, de ya até yb. Só a coluna P3_X tem passagem. */
static void trecho_vertical(Rota *rota, int x, int ya, int yb)
{
    if (x == P3_X) {
        if (ya < yb) { /* descendo: encontra a parte de cima primeiro */
            atravessar_faixa(rota, false, x, ya, yb, P3_CIMA_Y_MIN, P3_CIMA_Y_MAX, P3);
            atravessar_faixa(rota, false, x, ya, yb, P3_BAIXO_Y_MIN, P3_BAIXO_Y_MAX, P3);
        } else {       /* subindo: encontra a parte de baixo primeiro */
            atravessar_faixa(rota, false, x, ya, yb, P3_BAIXO_Y_MIN, P3_BAIXO_Y_MAX, P3);
            atravessar_faixa(rota, false, x, ya, yb, P3_CIMA_Y_MIN, P3_CIMA_Y_MAX, P3);
        }
    }
    adicionar(rota, x, yb, -1);
}

/* Monta a rota de (x0, y0) até (xd, yd). */
void calcular_rota(int x0, int y0, int xd, int yd, Rota *rota)
{
    rota->n = 0;
    if (x0 == xd) {
        trecho_vertical(rota, x0, y0, yd);
        return;
    }
    trecho_vertical(rota, x0, y0, CORREDOR_Y);
    trecho_horizontal(rota, x0, xd);
    trecho_vertical(rota, xd, CORREDOR_Y, yd);
}

bool dentro_de_passagem(int x, int y)
{
    if (x >= P1_X_MIN && x <= P1_X_MAX) return true;
    if (x >= P2_X_MIN && x <= P2_X_MAX) return true;
    if (x == P3_X && y >= P3_CIMA_Y_MIN && y <= P3_CIMA_Y_MAX) return true;
    if (x == P3_X && y >= P3_BAIXO_Y_MIN && y <= P3_BAIXO_Y_MAX) return true;
    return false;
}

bool na_estacao(int x, int y)
{
    int i;

    for (i = 0; i < N_ESTACOES; i++)
        if (x == ESTACAO_X[i] && y == ESTACAO_Y[i])
            return true;
    return false;
}

/* Estação de recarga mais próxima (distância |dx| + |dy|). */
void estacao_mais_proxima(int x, int y, int *ex, int *ey)
{
    int i, d, melhor = -1;

    for (i = 0; i < N_ESTACOES; i++) {
        d = abs(x - ESTACAO_X[i]) + abs(y - ESTACAO_Y[i]);
        if (melhor == -1 || d < melhor) {
            melhor = d;
            *ex = ESTACAO_X[i];
            *ey = ESTACAO_Y[i];
        }
    }
}

bool posicao_valida(int x, int y)
{
    if (x < 0 || x >= MAPA_LARGURA || y < 0 || y >= MAPA_ALTURA)
        return false;
    return !dentro_de_passagem(x, y);
}
