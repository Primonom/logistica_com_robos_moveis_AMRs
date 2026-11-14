/*
 * mapa.h — Mapa do galpão (grade 60 x 30, vista de cima) e caminhos (A*).
 *
 * x cresce para a direita e y para baixo.
 *
 * Partes FIXAS (constantes aqui): baias, zonas, estações de recarga, posição
 * padrão de um novo robô e as 3 paredes verticais.
 * Partes SORTEADAS a cada execução (struct Mapa, dentro da memória
 * compartilhada, gerada por gerar_mapa em mapa.c): a posição e a
 * capacidade das 4 passagens que atravessam as paredes e os 45 obstáculos.
 *
 * Este arquivo só descreve dados e funções; não cria threads. Sobre a
 * sincronização do mapa, ver o comentário da struct Mapa.
 */
#ifndef MAPA_H
#define MAPA_H

#include <stdbool.h>
#include <semaphore.h>

#define MAPA_LARGURA 60
#define MAPA_ALTURA  30
#define N_CELULAS    (MAPA_LARGURA * MAPA_ALTURA)   /* 1800 */
#define CORREDOR_Y   15   /* linha sempre sem obstáculos (corredor principal) */

/* Baias 1-4 (onde as caixas estão) e zonas de entrega A-D.
 * Índice 0 = baia 1 / zona A. */
#define N_BAIAS 4
#define N_ZONAS 4
static const int BAIA_X[N_BAIAS] = { 3,  3,  3,  3};
static const int BAIA_Y[N_BAIAS] = { 4, 11, 18, 25};
static const int ZONA_X[N_ZONAS] = {56, 56, 56, 56};
static const int ZONA_Y[N_ZONAS] = { 4, 11, 18, 25};

/* Estações de recarga (em células livres, fora das paredes). */
#define N_ESTACOES 2
static const int ESTACAO_X[N_ESTACOES] = {22, 37};
static const int ESTACAO_Y[N_ESTACOES] = { 1, 28};

/* Onde nasce um robô novo quando o pedido não diz a posição. */
#define POS_PADRAO_X 8
#define POS_PADRAO_Y 15

/* Paredes verticais: 4 colunas de espessura cada, de cima a baixo. */
#define N_PAREDES 3
#define ESPESSURA_PAREDE 4
static const int PAREDE_X[N_PAREDES] = {13, 28, 43};   /* 1ª coluna de cada uma */

/* Passagens: túneis horizontais de 1 célula de altura que atravessam uma
 * parede. O índice (0..3) é usado nos vetores; o nome do semáforo usa o
 * número (1..4): /atr_passagem_1 etc. */
#define N_PASSAGENS  4
#define N_OBSTACULOS 45
#define TAM_REGIAO   (ESPESSURA_PAREDE + 2)  /* túnel (4) + 1 boca de cada lado */

/* Tipo de cada célula. */
typedef enum { LIVRE, PAREDE, OBSTACULO } TipoCelula;

/* Uma passagem. A REGIÃO dela (onde vale o limite de capacidade) são as
 * células (x_entrada .. x_saida, y): a boca esquerda, as 4 do túnel e a
 * boca direita. */
typedef struct {
    int capacidade;   /* 1, 2, 3 ou 4 (sorteada) */
    int parede;       /* 0, 1 ou 2 */
    int y;            /* linha do túnel (sorteada em 2..27) */
    int x_entrada;    /* boca esquerda = PAREDE_X[parede] - 1 */
    int x_saida;      /* boca direita  = PAREDE_X[parede] + ESPESSURA_PAREDE */
} Passagem;

/* O mapa desta execução. Fica em MemoriaCompartilhada (mem->mapa).
 *
 * POR QUE não precisa de mutex: o mapa é escrito UMA vez, por gerar_mapa(),
 * na thread principal do processo central, ANTES de qualquer pthread_create
 * e de qualquer fork/exec de robô; depois disso ninguém mais escreve nele.
 * pthread_create e fork garantem que a thread/processo criado enxerga tudo
 * o que foi escrito antes da criação. Só leituras ao mesmo tempo não são
 * condição de corrida, então não há o que proteger. */
typedef struct {
    unsigned int semente;
    int tipo[MAPA_ALTURA][MAPA_LARGURA];       /* um valor de TipoCelula */
    int passagem[MAPA_ALTURA][MAPA_LARGURA];   /* índice da passagem da célula, -1 = nenhuma */
    Passagem passagens[N_PASSAGENS];
} Mapa;

/* Caminho célula a célula. x[0], y[0] = origem; x[n-1], y[n-1] = destino. */
#define MAX_CAMINHO 512
typedef struct {
    int n;
    int x[MAX_CAMINHO], y[MAX_CAMINHO];
} Caminho;

/* Semáforos das passagens abertos NESTE processo (por abrir_semaforos_passagens).
 * POR QUE: os semáforos contadores limitam quantos robôs ficam em cada
 * passagem (a Navegação faz sem_timedwait/sem_post) e o A* lê a ocupação
 * com sem_getvalue. Este vetor de ponteiros é escrito uma vez, antes de
 * criar as threads do processo, e depois só lido: não precisa de mutex. */
extern sem_t *sem_passagem[N_PASSAGENS];

/* ---------- mapa.c ---------- */
void gerar_mapa(Mapa *m);                 /* processo central, antes das threads */
void abrir_semaforos_passagens(void);     /* cada processo, antes das threads */
void fechar_semaforos_passagens(void);
bool calcular_caminho(int x0, int y0, int xd, int yd, Caminho *c);  /* A* */
int  passagem_em(int x, int y);           /* índice da passagem ou -1 */
bool dentro_de_passagem(int x, int y);
bool celula_livre(int x, int y);          /* dentro do mapa e sem parede/obstáculo */
bool posicao_valida(int x, int y);        /* livre e fora das passagens */
bool na_estacao(int x, int y);            /* (x, y) é uma estação de recarga? */
void estacao_mais_proxima(int x, int y, int *ex, int *ey);
void desenhar_mapa(char *saida, int tam); /* texto do mapa (comando "mapa") */

#endif
