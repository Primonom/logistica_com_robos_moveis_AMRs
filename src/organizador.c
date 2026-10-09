/*
 * organizador.c — Tarefa "Organizador da Logística" (processo central).
 *
 * Thread: tarefa_organizador, periódica com período PERIODO_ORGANIZADOR_MS
 * (T3 = 500 ms). No início carrega a fila de caixas do arquivo (por padrão
 * config/caixas.txt). A cada ciclo:
 *   1) acompanha as equipes em andamento (uma por baia):
 *      INDO_BAIA     -> todos AGUARDANDO_EQUIPE -> envia ACOPLAR a todos
 *      ACOPLANDO     -> todos OPERANDO_CARGA    -> envia IR_ZONA a todos
 *      TRANSPORTANDO -> todos na zona           -> envia SOLTAR, registra
 *      Em qualquer fase: se algum membro está em DEFEITO, envia PAUSAR a
 *      todos (seguram a posição); quando ninguém mais está em DEFEITO,
 *      envia CONTINUAR. Enquanto pausada, a equipe não muda de fase.
 *   2) tenta atribuir a primeira caixa da fila cuja baia está livre.
 *
 * Regras de atribuição:
 *   - Uma caixa só é atribuída quando TODOS os robôs necessários estão
 *     DISPONIVEL, em modo automático e com bateria >= 30% NO MESMO CICLO
 *     (a bateria mínima evita começar um transporte quase sem carga). Nunca se reserva uma
 *     equipe parcial. Isso evita deadlock: se o Organizador "segurasse"
 *     2 robôs para uma caixa de 3 e outros 2 para outra caixa de 3, as duas
 *     ficariam esperando para sempre (cada uma segura parte dos recursos e
 *     espera o resto). Sem reserva parcial, não existe "segura e espera".
 *   - Uma carga por baia de cada vez (equipes[] é indexado pela baia).
 *     Caixas cuja baia está ocupada são PULADAS. A caixa considerada é a
 *     primeira da fila com a baia livre.
 *   - Ordem ESTRITA de chegada quanto aos ROBÔS: se essa caixa não tem
 *     equipe completa, a fila PARA (as de trás esperam, mesmo que haja
 *     robôs para elas). Isso evita inanição das caixas grandes: se caixas
 *     pequenas pudessem "furar a fila", os robôs poderiam nunca ficar
 *     livres ao mesmo tempo em número suficiente para uma caixa de
 *     tamanho 4, e ela nunca seria atendida.
 *   - Pular a baia ocupada NÃO causa inanição: a baia só fica ocupada
 *     enquanto a entrega em andamento não termina (tempo limitado). Quando
 *     ela termina, a caixa pulada volta a ser considerada, e nenhuma caixa
 *     mais nova da MESMA baia pode passar na frente dela (a busca é em
 *     ordem de chegada). E pular não gasta os robôs que ela precisaria de
 *     forma permanente: eles voltam ao fim de cada entrega, e quando a
 *     baia dela libera ela é a primeira candidata, com a regra acima
 *     segurando a fila até juntar a equipe dela.
 *
 * Leitura dos robôs: SÓ o buffer de status (e_estado, e_posicao_x/y) e
 * modo_automatico, NUNCA o hw.
 *
 * Sincronização:
 *   - mutex de cada robô (PROCESS_SHARED): lê o buffer de status e escreve
 *     carga_id sem pegar a thread Status (outro processo) no meio de uma
 *     atualização.
 *   - mem->mutex_caixas: a fila de caixas também é alterada pelo Console
 *     (comando "caixa"); o mutex impede alterações simultâneas.
 *   - filas /atr_robo_<id> (enviar_para_robo): comandos para os robôs. O
 *     ACOPLAR vai para todos no mesmo ciclo (acoplamento síncrono).
 *   - pegar_robos_novos(): aviso assíncrono do Gestor (ver gestor.c).
 *   - equipes[] e em_equipe[] só são usados por esta thread: sem mutex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comum.h"
#include "mapa.h"

typedef enum { SEM_EQUIPE, INDO_BAIA, ACOPLANDO, TRANSPORTANDO } Fase;

typedef struct {
    Fase  fase;
    Caixa caixa;                        /* cópia da caixa sendo levada */
    int   indice_caixa;                 /* posição dela em mem->caixas */
    int   membros[TAMANHO_MAX_CAIXA];
    int   n;
    int   lider;                        /* membro de menor id */
    bool  pausada;                      /* mandou PAUSAR (membro em DEFEITO) */
} Equipe;

static Equipe equipes[N_BAIAS];        /* uma por baia (índice = baia - 1) */
static bool   em_equipe[MAX_ROBOS];    /* robô já está numa equipe? */
static int    ultima_avisada = -1;     /* id da última caixa "aguardando" */

/* printf + evento no Coletor (robo_id = -1: evento do sistema). */
static void avisar(const char *texto)
{
    printf("Organizador: %s\n", texto);
    registrar_evento(fila_coletor, -1, texto);
}

/* Coloca uma caixa no fim da fila. id < 0: usa o maior id + 1. */
bool adicionar_caixa(int id, int tamanho, int baia, int zona)
{
    bool ok = false;
    int i;

    if (tamanho < 1 || tamanho > TAMANHO_MAX_CAIXA || baia < 1 ||
        baia > N_BAIAS || zona < 0 || zona >= N_ZONAS)
        return false;

    pthread_mutex_lock(&mem->mutex_caixas);
    if (mem->n_caixas < MAX_CAIXAS) {
        if (id < 0) {
            id = 1;
            for (i = 0; i < mem->n_caixas; i++)
                if (mem->caixas[i].id >= id) id = mem->caixas[i].id + 1;
        }
        mem->caixas[mem->n_caixas].id = id;
        mem->caixas[mem->n_caixas].tamanho = tamanho;
        mem->caixas[mem->n_caixas].baia = baia;
        mem->caixas[mem->n_caixas].zona = zona;
        mem->caixas[mem->n_caixas].status = CAIXA_ESPERANDO;
        mem->caixas[mem->n_caixas].n_robos = 0;   /* ainda sem equipe */
        mem->caixas[mem->n_caixas].lider = -1;
        mem->n_caixas++;
        ok = true;
    }
    pthread_mutex_unlock(&mem->mutex_caixas);
    return ok;
}

/* Lê o arquivo de caixas: "id tamanho baia zona" por linha (zona A-D).
 * Linhas começando com '#' são comentários. */
static void carregar_caixas(const char *arquivo)
{
    char linha[128], zona;
    int id, tamanho, baia;
    FILE *f = fopen(arquivo, "r");

    if (f == NULL) { perror(arquivo); exit(1); }
    while (fgets(linha, sizeof(linha), f) != NULL) {
        if (linha[0] == '#')
            continue;
        if (sscanf(linha, "%d %d %d %c", &id, &tamanho, &baia, &zona) != 4)
            continue;
        if (!adicionar_caixa(id, tamanho, baia, zona - 'A'))
            printf("Organizador: linha invalida em %s: %s", arquivo, linha);
    }
    fclose(f);
}

/* Lê do BUFFER DE STATUS do robô i (nunca do hw). */
static void ler_status(int i, Estado *estado, int *x, int *y, int *bateria)
{
    pthread_mutex_lock(&mem->robos[i].mutex);
    *estado = mem->robos[i].e_estado;
    *x = mem->robos[i].e_posicao_x;
    *y = mem->robos[i].e_posicao_y;
    *bateria = mem->robos[i].bateria;
    pthread_mutex_unlock(&mem->robos[i].mutex);
}

static void enviar_todos(Equipe *e, MensagemRobo *msg)
{
    int k;

    for (k = 0; k < e->n; k++)
        enviar_para_robo(e->membros[k], msg);
}

/* Todos os membros estão no estado "estado"? */
static bool todos_no_estado(Equipe *e, Estado estado)
{
    Estado atual;
    int k, x, y, bateria;

    for (k = 0; k < e->n; k++) {
        ler_status(e->membros[k], &atual, &x, &y, &bateria);
        if (atual != estado)
            return false;
    }
    return true;
}

/* Algum membro está em DEFEITO? */
static bool algum_em_defeito(Equipe *e)
{
    Estado atual;
    int k, x, y, bateria;

    for (k = 0; k < e->n; k++) {
        ler_status(e->membros[k], &atual, &x, &y, &bateria);
        if (atual == DEFEITO)
            return true;
    }
    return false;
}

/* Todos os membros estão na posição (xd, yd)? */
static bool todos_na_posicao(Equipe *e, int xd, int yd)
{
    Estado atual;
    int k, x, y, bateria;

    for (k = 0; k < e->n; k++) {
        ler_status(e->membros[k], &atual, &x, &y, &bateria);
        if (x != xd || y != yd)
            return false;
    }
    return true;
}

/* Lista de ids da equipe em texto, ex.: "0 3 5". */
static void lista_membros(Equipe *e, char *texto, int tamanho)
{
    int k, usado = 0;

    texto[0] = '\0';
    for (k = 0; k < e->n; k++)
        usado += snprintf(texto + usado, tamanho - usado, k ? " %d" : "%d",
                          e->membros[k]);
}

/* Equipe e escreve carga_id (-1 = sem carga) no slot de cada membro. */
static void marcar_carga(Equipe *e, int carga_id)
{
    int k;

    for (k = 0; k < e->n; k++) {
        pthread_mutex_lock(&mem->robos[e->membros[k]].mutex);
        mem->robos[e->membros[k]].carga_id = carga_id;
        pthread_mutex_unlock(&mem->robos[e->membros[k]].mutex);
    }
}

/* Fim do transporte: SOLTAR, registra a carga, libera robôs e baia. */
static void finalizar_entrega(Equipe *e)
{
    MensagemRobo msg = {0};
    char robos[64], texto[128];
    int k;

    msg.tipo = SOLTAR;
    enviar_todos(e, &msg);
    marcar_carga(e, -1);

    lista_membros(e, robos, sizeof(robos));
    snprintf(texto, sizeof(texto), "%d;%d;%c;%s", e->caixa.id, e->caixa.baia,
             'A' + e->caixa.zona, robos);
    registrar_carga(fila_coletor, texto);
    snprintf(texto, sizeof(texto), "caixa %d entregue na zona %c (robos %s)",
             e->caixa.id, 'A' + e->caixa.zona, robos);
    avisar(texto);

    pthread_mutex_lock(&mem->mutex_caixas);
    mem->caixas[e->indice_caixa].status = CAIXA_ENTREGUE;
    pthread_mutex_unlock(&mem->mutex_caixas);

    for (k = 0; k < e->n; k++)
        em_equipe[e->membros[k]] = false;
    e->fase = SEM_EQUIPE;
}

/* Pausa a equipe se alguém entrou em DEFEITO; retoma quando ninguém mais
 * está. Retorna true se a equipe está pausada (não deve mudar de fase). */
static bool tratar_defeito(Equipe *e)
{
    MensagemRobo msg = {0};
    char texto[128];
    bool defeito = algum_em_defeito(e);

    if (defeito && !e->pausada) {
        msg.tipo = PAUSAR;
        enviar_todos(e, &msg);
        e->pausada = true;
        snprintf(texto, sizeof(texto), "caixa %d: robo da equipe em DEFEITO, PAUSAR", e->caixa.id);
        avisar(texto);
    } else if (!defeito && e->pausada) {
        msg.tipo = CONTINUAR;
        enviar_todos(e, &msg);
        e->pausada = false;
        snprintf(texto, sizeof(texto), "caixa %d: defeito resolvido, CONTINUAR", e->caixa.id);
        avisar(texto);
    }
    return e->pausada;
}

/* 1) Avança a equipe de uma baia, se a condição da fase atual foi atingida. */
static void acompanhar_equipe(Equipe *e)
{
    MensagemRobo msg = {0};
    char texto[128];
    int z = e->caixa.zona;

    if (tratar_defeito(e))
        return; /* pausada: espera o defeito sair */

    if (e->fase == INDO_BAIA && todos_no_estado(e, AGUARDANDO_EQUIPE)) {
        /* Acoplamento síncrono: todos chegaram -> ACOPLAR a todos no
         * mesmo ciclo. Ninguém ergue a caixa sozinho. */
        msg.tipo = ACOPLAR;
        enviar_todos(e, &msg);
        e->fase = ACOPLANDO;
        snprintf(texto, sizeof(texto), "caixa %d: equipe completa na baia, ACOPLAR",
                 e->caixa.id);
        avisar(texto);
    } else if (e->fase == ACOPLANDO && todos_no_estado(e, OPERANDO_CARGA)) {
        /* Só sai quando TODOS confirmaram a carga atracada. */
        msg.tipo = IR_ZONA;
        msg.x = ZONA_X[z];
        msg.y = ZONA_Y[z];
        msg.carga_id = e->caixa.id;
        msg.lider_id = e->lider;
        enviar_todos(e, &msg);
        e->fase = TRANSPORTANDO;
        snprintf(texto, sizeof(texto), "caixa %d: erguida por todos, IR_ZONA %c (lider %d)",
                 e->caixa.id, 'A' + z, e->lider);
        avisar(texto);
    } else if (e->fase == TRANSPORTANDO && todos_na_posicao(e, ZONA_X[z], ZONA_Y[z])) {
        finalizar_entrega(e);
    }
}

/* Escolhe até "tamanho" robôs livres, os mais próximos de (bx, by).
 * Livre = DISPONIVEL, automático e com bateria >= BATERIA_MINIMA_TAREFA
 * (pelo buffer de status) e fora de equipe. Retorna quantos achou (pode
 * ser menos que "tamanho"). */
static int escolher_robos(int tamanho, int bx, int by, int escolhidos[])
{
    int distancia[MAX_ROBOS];
    int i, k, x, y, bateria, n = 0, melhor;
    Estado estado;
    bool automatico, ativo;

    for (i = 0; i < MAX_ROBOS; i++) {
        distancia[i] = -1; /* -1 = não serve */
        pthread_mutex_lock(&mem->robos[i].mutex);
        ativo = mem->robos[i].ativo;
        automatico = mem->robos[i].modo_automatico;
        pthread_mutex_unlock(&mem->robos[i].mutex);
        if (!ativo || !automatico || em_equipe[i])
            continue;
        ler_status(i, &estado, &x, &y, &bateria);
        if (estado == DISPONIVEL && bateria >= BATERIA_MINIMA_TAREFA)
            distancia[i] = abs(x - bx) + abs(y - by);
    }
    /* Seleção simples: pega o mais próximo, depois o próximo, ... */
    for (k = 0; k < tamanho; k++) {
        melhor = -1;
        for (i = 0; i < MAX_ROBOS; i++)
            if (distancia[i] >= 0 && (melhor == -1 || distancia[i] < distancia[melhor]))
                melhor = i;
        if (melhor == -1)
            break;
        escolhidos[n++] = melhor;
        distancia[melhor] = -1;
    }
    return n;
}

/* Procura a primeira caixa ESPERANDO (a mais antiga) cuja baia está
 * livre; caixas de baia ocupada são puladas. Retorna o índice ou -1, e
 * copia a caixa em *c. */
static int primeira_da_fila(Caixa *c)
{
    int i, achou = -1;

    pthread_mutex_lock(&mem->mutex_caixas);
    for (i = 0; i < mem->n_caixas; i++) {
        if (mem->caixas[i].status == CAIXA_ESPERANDO &&
            equipes[mem->caixas[i].baia - 1].fase == SEM_EQUIPE) {
            *c = mem->caixas[i];
            achou = i;
            break;
        }
    }
    pthread_mutex_unlock(&mem->mutex_caixas);
    return achou;
}

/* Forma a equipe da caixa c (já com todos os robôs escolhidos). */
static void formar_equipe(Equipe *e, Caixa c, int indice, int membros[])
{
    MensagemRobo msg = {0};
    char robos[64], texto[128];
    int k;

    e->caixa = c;
    e->indice_caixa = indice;
    e->n = c.tamanho;
    e->lider = membros[0];
    for (k = 0; k < e->n; k++) {
        e->membros[k] = membros[k];
        em_equipe[membros[k]] = true;
        if (membros[k] < e->lider) e->lider = membros[k];
    }
    e->fase = INDO_BAIA;
    e->pausada = false;
    marcar_carga(e, c.id);

    /* Status e equipe mudam juntos, sob o mesmo lock: quem lê a caixa
     * (Console, ponte MQTT) nunca vê EM_ANDAMENTO sem a equipe. */
    pthread_mutex_lock(&mem->mutex_caixas);
    mem->caixas[indice].status = CAIXA_EM_ANDAMENTO;
    mem->caixas[indice].n_robos = e->n;
    mem->caixas[indice].lider = e->lider;
    for (k = 0; k < e->n; k++)
        mem->caixas[indice].robos[k] = e->membros[k];
    pthread_mutex_unlock(&mem->mutex_caixas);

    msg.tipo = TAREFA_IR_BAIA;
    msg.x = BAIA_X[c.baia - 1];
    msg.y = BAIA_Y[c.baia - 1];
    msg.carga_id = c.id;
    enviar_todos(e, &msg);

    lista_membros(e, robos, sizeof(robos));
    snprintf(texto, sizeof(texto), "caixa %d (tam %d, baia %d -> zona %c): robos %s indo para a baia",
             c.id, c.tamanho, c.baia, 'A' + c.zona, robos);
    avisar(texto);
}

/* 2) Atribui caixas (primeira com baia livre) enquanto houver equipe
 * completa. Cada caixa atribuída ocupa a baia dela, então a próxima volta
 * do laço já pula essa baia. */
/* Publica na memória compartilhada qual caixa está esperando equipe (-1 =
 * nenhuma) e quantos robôs livres havia: só para a Tela de Operação mostrar
 * o motivo da espera. */
static void informar_espera(int caixa_id, int livres)
{
    pthread_mutex_lock(&mem->mutex_caixas);
    mem->caixa_em_espera = caixa_id;
    mem->robos_livres_espera = livres;
    pthread_mutex_unlock(&mem->mutex_caixas);
}

static void atribuir_caixas(void)
{
    int membros[TAMANHO_MAX_CAIXA];
    char texto[128];
    Caixa c;
    int indice, n;

    while ((indice = primeira_da_fila(&c)) != -1) {
        n = escolher_robos(c.tamanho, BAIA_X[c.baia - 1], BAIA_Y[c.baia - 1], membros);
        if (n < c.tamanho) {
            /* Equipe incompleta: NÃO reserva ninguém e a fila espera. */
            informar_espera(c.id, n);
            if (ultima_avisada != c.id) {
                snprintf(texto, sizeof(texto), "caixa %d (tam %d) aguardando equipe: %d robo(s) livre(s)",
                         c.id, c.tamanho, n);
                avisar(texto);
                ultima_avisada = c.id;
            }
            return;
        }
        formar_equipe(&equipes[c.baia - 1], c, indice, membros);
    }
    informar_espera(-1, 0);   /* nenhuma caixa esperando equipe */
}

void *tarefa_organizador(void *arg)
{
    struct timespec proximo;
    char texto[64];
    int b, novos;

    carregar_caixas((const char *)arg);
    for (b = 0; b < N_BAIAS; b++)
        equipes[b].fase = SEM_EQUIPE;

    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        novos = pegar_robos_novos();
        if (novos > 0) {
            snprintf(texto, sizeof(texto), "%d robo(s) novo(s) na frota", novos);
            avisar(texto);
        }
        for (b = 0; b < N_BAIAS; b++)
            if (equipes[b].fase != SEM_EQUIPE)
                acompanhar_equipe(&equipes[b]);
        atribuir_caixas();

        somar_ms(&proximo, PERIODO_ORGANIZADOR_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }
    return NULL;
}
