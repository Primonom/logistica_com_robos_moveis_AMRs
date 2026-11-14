/*
 * mapa.c — Mapa aleatório do galpão e cálculo de caminhos com A*
 *          (ponto extra: "IA que otimiza as rotas").
 *
 * Não cria threads. Funções usadas por:
 *   - processo central: gerar_mapa (main, uma vez, antes das threads),
 *     calcular_caminho (Organizador escolhe robôs), posicao_valida
 *     (Console e ponte MQTT), na_estacao (simulador);
 *   - processo central: também passos_caminho e estacao_mais_proxima
 *     (Organizador estima a bateria da tarefa);
 *   - processo robo: calcular_caminho e calcular_caminho_evitando
 *     (Navegação; o segundo no desvio de travamentos, ponto extra +2),
 *     estacao_mais_proxima (Status e Navegação) e na_estacao (Status).
 *
 * 1) GERAÇÃO DO MAPA (gerar_mapa), a cada execução, com a semente da
 *    variável de ambiente ATR_SEMENTE (ou time(NULL) se ela não existir):
 *    - 3 paredes verticais fixas, 4 colunas de espessura cada;
 *    - 4 passagens (túneis de 1 célula) com as capacidades {1, 2, 3, 4}
 *      embaralhadas; uma parede sorteada recebe 2 passagens e as outras 1;
 *      y sorteado em 2..27, com 5 ou mais de distância entre duas
 *      passagens da mesma parede;
 *    - 45 obstáculos de 1 célula em lugares livres, longe das bocas das
 *      passagens, fora do corredor y = 15 e fora das ÁREAS das baias e
 *      zonas (os retângulos coloridos da tela, não só a célula do ponto);
 *    - validação com BFS (busca em largura): de cada baia tem de dar para
 *      chegar a todas as zonas, estações e à posição padrão. Se não der,
 *      sorteia de novo (até 100 tentativas).
 *
 * 2) A* (calcular_caminho): menor caminho em grade com vizinhança 4.
 *    Custo de um passo = 1 + custo extra; o extra só existe ao ENTRAR na
 *    região de uma passagem: 3 x (robôs dentro dela agora), e mais 30 se
 *    quem está dentro anda no sentido CONTRÁRIO (mão única: teria de
 *    esperar o túnel esvaziar, ver ocupacao.c). Assim um robô evita, se o
 *    desvio compensar, uma passagem cheia ou na contramão.
 *    calcular_caminho_evitando recebe ainda um vetor de células proibidas
 *    (robôs de outros grupos, no desvio de travamentos da Navegação).
 *    Heurística = distância de Manhattan até o destino. Ela nunca é maior
 *    que o custo real (cada passo custa pelo menos 1), ou seja, é
 *    ADMISSÍVEL: o A* devolve o caminho ÓTIMO.
 *
 * Sincronização:
 *   - mem->mapa: sem mutex, porque é só leitura depois de gerado (ver o
 *     comentário da struct Mapa em mapa.h).
 *   - calcular_caminho é chamada ao mesmo tempo por várias threads (no
 *     robô: Navegação e Status). Ela não tem nenhuma variável static: os
 *     vetores do A* são LOCAIS (na pilha de quem chama), então cada
 *     chamada tem os seus e não precisa de mutex.
 *   - sem_getvalue: o A* lê quantos robôs estão em cada passagem. É uma
 *     leitura instantânea, só para ESTIMAR o custo: logo depois o valor
 *     pode mudar (o caminho pode ficar "desatualizado"). Não tem problema:
 *     quem garante a capacidade continua sendo o sem_trywait do monitor
 *     da passagem (ocupacao.c). No pior caso o robô espera na porta e
 *     replaneja.
 *   - ler_monitor (ocupacao.c): lê dentro/sentido de cada passagem com o
 *     mutex do monitor, um de cada vez, copiando para vetores locais antes
 *     da busca. Também é só estimativa (quem decide é entrar_passagem).
 *   - sem_passagem[]: escrito uma vez por abrir_semaforos_passagens(),
 *     antes de o processo criar as threads; depois só lido.
 *   - rand() não é seguro entre threads, mas só gerar_mapa o usa, e ela
 *     roda antes de existirem outras threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "comum.h"
#include "mapa.h"

#define TENTATIVAS_MAPA   100
#define DISTANCIA_MIN_Y   5    /* entre duas passagens da mesma parede */
#define RAIO_BOCA         2    /* sem obstáculo a até 2 células de uma boca */
#define CUSTO_POR_ROBO    3    /* custo extra por robô dentro da passagem */
#define CUSTO_CONTRAMAO   30    /* passagem ocupada no sentido contrário */

sem_t *sem_passagem[N_PASSAGENS];

/* ============================================================
 * Consultas (usam mem->mapa, só leitura)
 * ============================================================ */

static bool dentro_da_grade(int x, int y)
{
    return x >= 0 && x < MAPA_LARGURA && y >= 0 && y < MAPA_ALTURA;
}

bool celula_livre(int x, int y)
{
    return dentro_da_grade(x, y) && mem->mapa.tipo[y][x] == LIVRE;
}

int passagem_em(int x, int y)
{
    if (!dentro_da_grade(x, y))
        return -1;
    return mem->mapa.passagem[y][x];
}

bool dentro_de_passagem(int x, int y)
{
    return passagem_em(x, y) != -1;
}

/* Posição aceita para novo robô e goto: livre e fora das passagens (um
 * robô parado numa passagem ocuparia a vaga sem ter o semáforo). */
bool posicao_valida(int x, int y)
{
    return celula_livre(x, y) && !dentro_de_passagem(x, y);
}

bool na_estacao(int x, int y)
{
    int i;

    for (i = 0; i < N_ESTACOES; i++)
        if (x == ESTACAO_X[i] && y == ESTACAO_Y[i])
            return true;
    return false;
}

/* Número de passos (células andadas) do caminho A* de (x0, y0) até
 * (xd, yd), ou -1 se não há caminho. Usado para estimar a bateria. */
int passos_caminho(int x0, int y0, int xd, int yd)
{
    Caminho c;

    if (!calcular_caminho(x0, y0, xd, yd, &c))
        return -1;
    return c.n - 1;
}

/* Estação com o MENOR CAMINHO (A*) a partir de (x, y). Retorna os passos
 * até ela, ou -1 se nenhuma tem caminho (não acontece num mapa validado;
 * nesse caso fica a primeira). */
int estacao_mais_proxima(int x, int y, int *ex, int *ey)
{
    int i, p, melhor = -1;

    *ex = ESTACAO_X[0];
    *ey = ESTACAO_Y[0];
    for (i = 0; i < N_ESTACOES; i++) {
        p = passos_caminho(x, y, ESTACAO_X[i], ESTACAO_Y[i]);
        if (p >= 0 && (melhor == -1 || p < melhor)) {
            melhor = p;
            *ex = ESTACAO_X[i];
            *ey = ESTACAO_Y[i];
        }
    }
    return melhor;
}

/* ============================================================
 * Semáforos das passagens (para o custo dinâmico do A*)
 * ============================================================ */

/* Abre os semáforos já criados pelo main (nomes /atr_passagem_1..4).
 * Cada processo chama uma vez, antes de criar as suas threads. */
void abrir_semaforos_passagens(void)
{
    char nome[32];
    int i;

    for (i = 0; i < N_PASSAGENS; i++) {
        snprintf(nome, sizeof(nome), "%s%d", PREFIXO_SEM_PASSAGEM, i + 1);
        sem_passagem[i] = sem_open(nome, 0);
        if (sem_passagem[i] == SEM_FAILED) { perror("sem_open passagem"); exit(1); }
    }
}

void fechar_semaforos_passagens(void)
{
    int i;

    for (i = 0; i < N_PASSAGENS; i++)
        sem_close(sem_passagem[i]);
}

/* Quantos robôs estão dentro de cada passagem AGORA e em que sentido
 * (estimativa: ver "Sincronização" no topo). ocupados = capacidade - vagas
 * livres do semáforo; dentro/sentido vêm do monitor da mão única. */
static void ler_ocupacao(int ocupados[], int dentro[], int sentido[])
{
    int i, livres;

    for (i = 0; i < N_PASSAGENS; i++) {
        ocupados[i] = 0;
        if (sem_getvalue(sem_passagem[i], &livres) == 0 &&
            livres < mem->mapa.passagens[i].capacidade)
            ocupados[i] = mem->mapa.passagens[i].capacidade - livres;
        ler_monitor(i, &dentro[i], &sentido[i]);
    }
}

/* ============================================================
 * A* (calcular_caminho)
 * ============================================================
 *
 * Vocabulário (cada célula tem um índice i = y * LARGURA + x):
 *   g[i]   = custo do melhor caminho conhecido da origem até i;
 *   h(i)   = estimativa do custo de i até o destino (Manhattan);
 *   f[i]   = g[i] + h(i) = estimativa do caminho inteiro passando por i;
 *   pai[i] = de qual célula se chegou em i (para remontar o caminho);
 *   lista ABERTA  = células descobertas que ainda falta expandir;
 *   lista FECHADA = células já expandidas (o g delas é definitivo).
 *
 * Passo a passo:
 *   1) a origem entra na lista aberta com g = 0;
 *   2) tira da lista aberta a célula de MENOR f (a mais promissora);
 *   3) se ela é o destino, acabou: remonta o caminho pelos pais;
 *   4) senão, fecha a célula e olha os 4 vizinhos livres: se chegar ao
 *      vizinho por ela é mais barato que o g que ele tinha, atualiza g, f
 *      e pai e coloca o vizinho na lista aberta;
 *   5) volta ao passo 2. Lista aberta vazia = não há caminho.
 *
 * Complexidade: a lista aberta é procurada com busca LINEAR (passa pelas
 * 1800 células para achar o menor f), sem heap. No pior caso cada célula
 * é expandida uma vez: 1800 x 1800 ≈ 3,2 milhões de comparações, poucos
 * milissegundos — sobra muito dentro do período de 100 ms da Navegação.
 * Com uma grade tão pequena, um heap só deixaria o código mais difícil.
 *
 * Por que não precisa "reabrir" células fechadas: a heurística de
 * Manhattan também é CONSISTENTE (entre dois vizinhos ela muda no máximo
 * 1, e todo passo custa pelo menos 1). Assim, quando uma célula sai da
 * lista aberta, o g dela já é o menor possível.
 */

enum { NAO_VISTA, ABERTA, FECHADA };

static int heuristica(int x, int y, int xd, int yd)
{
    return abs(x - xd) + abs(y - yd);
}

/* Sentido de quem entra na região de p pela célula xb: pela boca
 * esquerda anda para a direita (0); pela direita, para a esquerda (1). */
int sentido_de_entrada(int p, int xb)
{
    return xb == mem->mapa.passagens[p].x_entrada ? 0 : 1;
}

/* Custo de ir da célula (xa, ya) para a vizinha (xb, yb). */
static int custo_passo(int xa, int ya, int xb, int yb, const int ocupados[],
                       const int dentro[], const int sentido[])
{
    int p = passagem_em(xb, yb), custo = 1;

    if (p != -1 && passagem_em(xa, ya) != p) {   /* entrando na região de p */
        custo += CUSTO_POR_ROBO * ocupados[p];
        if (dentro[p] > 0 && sentido[p] != sentido_de_entrada(p, xb))
            custo += CUSTO_CONTRAMAO;
    }
    return custo;
}

bool calcular_caminho(int x0, int y0, int xd, int yd, Caminho *c)
{
    return calcular_caminho_evitando(x0, y0, xd, yd, NULL, c);
}

/* A*. "proibida" (índice y * LARGURA + x) = células que o caminho não pode
 * usar além das paredes e obstáculos; NULL = nenhuma. */
bool calcular_caminho_evitando(int x0, int y0, int xd, int yd,
                               const bool proibida[], Caminho *c)
{
    /* POR QUE vetores LOCAIS (e não static): Navegação e Status chamam o A*
     * ao mesmo tempo; com vetores static uma thread estragaria a busca da
     * outra. Locais, cada chamada tem os seus e não precisa de mutex. */
    int g[N_CELULAS], f[N_CELULAS], pai[N_CELULAS];
    char situacao[N_CELULAS];
    int ocupados[N_PASSAGENS], dentro[N_PASSAGENS], sentido[N_PASSAGENS];
    static const int DX[4] = {1, -1, 0, 0};
    static const int DY[4] = {0, 0, 1, -1};
    int i, k, atual, ax, ay, vx, vy, v, novo_g, n, destino;

    c->n = 0;
    if (!dentro_da_grade(x0, y0) || !celula_livre(xd, yd))
        return false;
    ler_ocupacao(ocupados, dentro, sentido);
    memset(situacao, NAO_VISTA, sizeof(situacao));
    destino = yd * MAPA_LARGURA + xd;

    /* 1) origem na lista aberta */
    i = y0 * MAPA_LARGURA + x0;
    g[i] = 0;
    f[i] = heuristica(x0, y0, xd, yd);
    pai[i] = -1;
    situacao[i] = ABERTA;

    while (1) {
        /* 2) menor f da lista aberta (busca linear; empate: menor índice) */
        atual = -1;
        for (i = 0; i < N_CELULAS; i++)
            if (situacao[i] == ABERTA && (atual == -1 || f[i] < f[atual]))
                atual = i;
        if (atual == -1)
            return false;              /* lista aberta vazia: sem caminho */
        if (atual == destino)
            break;                     /* 3) chegou */

        /* 4) fecha e olha os vizinhos */
        situacao[atual] = FECHADA;
        ax = atual % MAPA_LARGURA;
        ay = atual / MAPA_LARGURA;
        for (k = 0; k < 4; k++) {
            vx = ax + DX[k];
            vy = ay + DY[k];
            if (!celula_livre(vx, vy))
                continue;
            v = vy * MAPA_LARGURA + vx;
            if (proibida != NULL && proibida[v])
                continue;
            if (situacao[v] == FECHADA)
                continue;
            novo_g = g[atual] + custo_passo(ax, ay, vx, vy, ocupados, dentro, sentido);
            if (situacao[v] == NAO_VISTA || novo_g < g[v]) {
                g[v] = novo_g;
                f[v] = novo_g + heuristica(vx, vy, xd, yd);
                pai[v] = atual;
                situacao[v] = ABERTA;
            }
        }
    }

    /* Remonta o caminho do destino para trás, seguindo os pais. */
    n = 0;
    for (i = destino; i != -1; i = pai[i])
        n++;
    if (n > MAX_CAMINHO)
        return false;
    c->n = n;
    for (i = destino; i != -1; i = pai[i]) {
        n--;
        c->x[n] = i % MAPA_LARGURA;
        c->y[n] = i / MAPA_LARGURA;
    }
    return true;
}

/* ============================================================
 * Geração do mapa (só gerar_mapa escreve em mem->mapa)
 * ============================================================ */

/* Célula reservada (baia, zona, estação ou posição padrão)? */
static bool ponto_fixo(int x, int y)
{
    int i;

    for (i = 0; i < N_BAIAS; i++)
        if (x == BAIA_X[i] && y == BAIA_Y[i]) return true;
    for (i = 0; i < N_ZONAS; i++)
        if (x == ZONA_X[i] && y == ZONA_Y[i]) return true;
    if (na_estacao(x, y))
        return true;
    return x == POS_PADRAO_X && y == POS_PADRAO_Y;
}

/* Dentro da ÁREA de uma baia ou zona (o retângulo colorido que a tela
 * desenha, ver regiao() em interface/mapa_svg.py)? Baia: da parede
 * esquerda até 3 células à direita do ponto; zona: de 3 células à
 * esquerda do ponto até a parede direita; nas duas, 3 células acima e
 * abaixo do ponto. Obstáculo ali deixaria a área "suja" e atrapalharia a
 * fila de robôs chegando. */
static bool area_de_baia_ou_zona(int x, int y)
{
    int i;

    for (i = 0; i < N_BAIAS; i++)
        if (x <= BAIA_X[i] + 3 && abs(y - BAIA_Y[i]) <= 3) return true;
    for (i = 0; i < N_ZONAS; i++)
        if (x >= ZONA_X[i] - 3 && abs(y - ZONA_Y[i]) <= 3) return true;
    return false;
}

/* A até RAIO_BOCA células (quadrado 5x5) da boca de alguma passagem? */
static bool perto_de_boca(const Mapa *m, int x, int y)
{
    int i;
    const Passagem *p;

    for (i = 0; i < N_PASSAGENS; i++) {
        p = &m->passagens[i];
        if (abs(y - p->y) <= RAIO_BOCA &&
            (abs(x - p->x_entrada) <= RAIO_BOCA || abs(x - p->x_saida) <= RAIO_BOCA))
            return true;
    }
    return false;
}

/* Pode haver obstáculo em (x, y)? */
static bool aceita_obstaculo(const Mapa *m, int x, int y)
{
    return m->tipo[y][x] == LIVRE && m->passagem[y][x] == -1 &&
           y != CORREDOR_Y && !ponto_fixo(x, y) && !area_de_baia_ou_zona(x, y) &&
           !perto_de_boca(m, x, y);
}

/* Sorteia o y de uma passagem na parede "parede", a 5 ou mais de
 * distância das passagens já sorteadas na mesma parede (0..n-1). */
static int sortear_y(const Mapa *m, int n, int parede)
{
    int y, i;
    bool perto;

    do {
        y = 2 + rand() % 26;   /* 2..27 */
        perto = false;
        for (i = 0; i < n; i++)
            if (m->passagens[i].parede == parede &&
                abs(m->passagens[i].y - y) < DISTANCIA_MIN_Y)
                perto = true;
    } while (perto);
    return y;
}

/* Sorteia paredes, passagens e obstáculos (uma tentativa). */
static void sortear(Mapa *m)
{
    int capacidades[N_PASSAGENS] = {1, 2, 3, 4};
    int x, y, i, j, t, w, dupla, n;
    Passagem *p;

    /* tudo livre, depois as paredes inteiras */
    for (y = 0; y < MAPA_ALTURA; y++)
        for (x = 0; x < MAPA_LARGURA; x++) {
            m->tipo[y][x] = LIVRE;
            m->passagem[y][x] = -1;
        }
    for (w = 0; w < N_PAREDES; w++)
        for (y = 0; y < MAPA_ALTURA; y++)
            for (x = PAREDE_X[w]; x < PAREDE_X[w] + ESPESSURA_PAREDE; x++)
                m->tipo[y][x] = PAREDE;

    /* capacidades embaralhadas (Fisher-Yates) */
    for (i = N_PASSAGENS - 1; i > 0; i--) {
        j = rand() % (i + 1);
        t = capacidades[i];
        capacidades[i] = capacidades[j];
        capacidades[j] = t;
    }

    /* passagens: 1 por parede e mais 1 na parede "dupla". Numeradas da
     * esquerda para a direita (e de cima para baixo na mesma parede). */
    dupla = rand() % N_PAREDES;
    n = 0;
    for (w = 0; w < N_PAREDES; w++) {
        m->passagens[n].parede = w;
        m->passagens[n].y = sortear_y(m, n, w);
        n++;
        if (w == dupla) {
            m->passagens[n].parede = w;
            m->passagens[n].y = sortear_y(m, n, w);
            if (m->passagens[n].y < m->passagens[n - 1].y) {   /* de cima para baixo */
                t = m->passagens[n].y;
                m->passagens[n].y = m->passagens[n - 1].y;
                m->passagens[n - 1].y = t;
            }
            n++;
        }
    }

    /* abre o túnel e marca a região (bocas + túnel) de cada passagem */
    for (i = 0; i < N_PASSAGENS; i++) {
        p = &m->passagens[i];
        p->capacidade = capacidades[i];
        p->x_entrada = PAREDE_X[p->parede] - 1;
        p->x_saida = PAREDE_X[p->parede] + ESPESSURA_PAREDE;
        for (x = p->x_entrada; x <= p->x_saida; x++) {
            m->tipo[p->y][x] = LIVRE;
            m->passagem[p->y][x] = i;
        }
    }

    /* obstáculos isolados (1x1) em lugares permitidos */
    n = 0;
    while (n < N_OBSTACULOS) {
        x = rand() % MAPA_LARGURA;
        y = rand() % MAPA_ALTURA;
        if (aceita_obstaculo(m, x, y)) {
            m->tipo[y][x] = OBSTACULO;
            n++;
        }
    }
}

/* BFS a partir de (x0, y0): marca em alcancavel[] toda célula livre que
 * dá para alcançar. Fila simples num vetor (cada célula entra no máximo
 * uma vez). */
static void bfs(const Mapa *m, int x0, int y0, bool alcancavel[])
{
    static const int DX[4] = {1, -1, 0, 0};
    static const int DY[4] = {0, 0, 1, -1};
    int fila[N_CELULAS];
    int inicio = 0, fim = 0, i, k, x, y;

    memset(alcancavel, 0, N_CELULAS * sizeof(bool));
    alcancavel[y0 * MAPA_LARGURA + x0] = true;
    fila[fim++] = y0 * MAPA_LARGURA + x0;
    while (inicio < fim) {
        i = fila[inicio++];
        for (k = 0; k < 4; k++) {
            x = i % MAPA_LARGURA + DX[k];
            y = i / MAPA_LARGURA + DY[k];
            if (x < 0 || x >= MAPA_LARGURA || y < 0 || y >= MAPA_ALTURA ||
                m->tipo[y][x] != LIVRE || alcancavel[y * MAPA_LARGURA + x])
                continue;
            alcancavel[y * MAPA_LARGURA + x] = true;
            fila[fim++] = y * MAPA_LARGURA + x;
        }
    }
}

/* De cada baia dá para chegar a todas as zonas, estações e à posição padrão? */
static bool mapa_conectado(const Mapa *m)
{
    bool alcancavel[N_CELULAS];
    int b, i;

    for (b = 0; b < N_BAIAS; b++) {
        bfs(m, BAIA_X[b], BAIA_Y[b], alcancavel);
        for (i = 0; i < N_ZONAS; i++)
            if (!alcancavel[ZONA_Y[i] * MAPA_LARGURA + ZONA_X[i]]) return false;
        for (i = 0; i < N_ESTACOES; i++)
            if (!alcancavel[ESTACAO_Y[i] * MAPA_LARGURA + ESTACAO_X[i]]) return false;
        if (!alcancavel[POS_PADRAO_Y * MAPA_LARGURA + POS_PADRAO_X]) return false;
    }
    return true;
}

/* Processo central, thread principal, ANTES de criar threads e robôs. */
void gerar_mapa(Mapa *m)
{
    const char *texto = getenv("ATR_SEMENTE");
    int tentativa, i;
    Passagem *p;

    m->semente = (texto != NULL && texto[0] != '\0') ? (unsigned int)atoi(texto)
                                                     : (unsigned int)time(NULL);
    srand(m->semente);
    for (tentativa = 1; tentativa <= TENTATIVAS_MAPA; tentativa++) {
        sortear(m);
        if (mapa_conectado(m))
            break;
    }
    if (tentativa > TENTATIVAS_MAPA) {
        fprintf(stderr, "Nao foi possivel gerar um mapa conectado (semente %u)\n", m->semente);
        exit(1);
    }

    printf("Mapa gerado com semente %u\n", m->semente);
    for (i = 0; i < N_PASSAGENS; i++) {
        p = &m->passagens[i];
        printf("  P%d: parede %d, y = %d, x %d..%d, capacidade %d\n", i + 1,
               p->parede + 1, p->y, p->x_entrada, p->x_saida, p->capacidade);
    }
}

/* Texto do mapa para o comando "mapa" do Console:
 *   # parede   o obstáculo   1-4 região da passagem P1-P4
 *   B baia     Z zona        E estação   R posição padrão   . livre
 * "saida" precisa ter espaço para o mapa inteiro (cerca de 1,9 KB). */
#define TAMANHO_DESENHO (64 + MAPA_ALTURA * (MAPA_LARGURA + 1))
void desenhar_mapa(char *saida, int tam)
{
    int x, y, i, p, usado;
    char ch;

    if (tam < TAMANHO_DESENHO) {
        snprintf(saida, tam, "resposta pequena demais para o mapa\n");
        return;
    }
    usado = sprintf(saida, "Mapa (semente %u):\n", mem->mapa.semente);
    for (y = 0; y < MAPA_ALTURA; y++) {
        for (x = 0; x < MAPA_LARGURA; x++) {
            p = mem->mapa.passagem[y][x];
            if (mem->mapa.tipo[y][x] == PAREDE)         ch = '#';
            else if (mem->mapa.tipo[y][x] == OBSTACULO) ch = 'o';
            else if (p != -1)                           ch = (char)('1' + p);
            else                                        ch = '.';
            for (i = 0; i < N_BAIAS; i++)
                if (x == BAIA_X[i] && y == BAIA_Y[i]) ch = 'B';
            for (i = 0; i < N_ZONAS; i++)
                if (x == ZONA_X[i] && y == ZONA_Y[i]) ch = 'Z';
            if (na_estacao(x, y)) ch = 'E';
            if (x == POS_PADRAO_X && y == POS_PADRAO_Y) ch = 'R';
            saida[usado++] = ch;
        }
        saida[usado++] = '\n';
    }
    saida[usado] = '\0';
}
