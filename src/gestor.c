/*
 * gestor.c — Tarefa "Gestor de Robôs" (processo central).
 *
 * Thread: tarefa_gestor (uma só).
 *   Espera pedidos de "novo robô". Para cada pedido: escolhe um id livre,
 *   escolhe a célula LIVRE mais próxima da posição pedida (BFS na grade de
 *   ocupação, anti-colisão) e já a reserva para o robô, prepara o slot do
 *   robô na memória compartilhada, cria a fila /atr_robo_<id> e cria o
 *   PROCESSO do robô com fork() + execl("./robo"). A mensagem "robo
 *   criado" informa a posição REAL (pode ser vizinha da pedida).
 *
 * Quem produz os pedidos (via pedir_novo_robo()): o Console (comando
 * "novo", usado nos cenários de teste) e a ponte MQTT (tópico
 * atr/robo/novo, publicado pelo simulador, ver ponte_mqtt.c).
 *
 * Sincronização:
 *   - Fila circular de pedidos + mutex + variável de condição
 *     (produtor-consumidor clássico):
 *       * mutex_pedidos: Console, ponte MQTT e Gestor mexem em pedidos[],
 *         inicio e quantidade; sem o mutex dois deles poderiam alterar os
 *         índices ao mesmo tempo e um pedido seria perdido ou lido duas vezes.
 *       * cond_pedidos: o Gestor DORME enquanto a fila está vazia (não gasta
 *         CPU) e quem coloca um pedido (Console ou ponte) o ACORDA.
 *   - Mutex de cada robô (na memória compartilhada) ao mexer no slot.
 *   - Mutex da grade de ocupação (reservar_livre_perto, ocupacao.c): a BFS
 *     e a reserva acontecem sob o mesmo lock, então nenhum robô entra na
 *     célula entre a escolha e a reserva. Nunca junto com o mutex do robô
 *     (a reserva termina antes de iniciar_slot).
 *   - enviar_para_robo(): manda uma mensagem pela fila /atr_robo_<id>
 *     (usada pelo Console e pelo Organizador).
 *   - O Organizador fica sabendo do robô novo de forma ASSÍNCRONA: o Gestor
 *     marca ativo = true no slot e incrementa robos_novos (protegido por
 *     mutex_novos, porque Gestor e Organizador são threads diferentes);
 *     o Organizador lê e zera o contador no próximo ciclo dele. Nenhum dos
 *     dois espera pelo outro.
 *   - Com ATR_TEMPO_REAL=1: SCHED_FIFO prioridade 50, abaixo das periódicas
 *     (é esporádica; ver tempo_real.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "comum.h"
#include "mapa.h"

#define MAX_PEDIDOS MAX_ROBOS  /* capacidade da fila circular de pedidos: cabe
                               * um pedido por robô (o benchmark pede N de uma vez) */

typedef struct { int x, y; } Pedido;

/* Fila circular de pedidos (só existe dentro do processo central). */
static Pedido pedidos[MAX_PEDIDOS];
static int inicio = 0;       /* posição do próximo pedido a ser lido */
static int quantidade = 0;   /* quantos pedidos estão na fila */
static pthread_mutex_t mutex_pedidos = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond_pedidos  = PTHREAD_COND_INITIALIZER;

/* Aviso assíncrono ao Organizador: quantos robôs foram criados desde a
 * última vez que ele olhou. */
static int robos_novos = 0;
static pthread_mutex_t mutex_novos = PTHREAD_MUTEX_INITIALIZER;

/* Organizador: retorna quantos robôs novos surgiram e zera o contador. */
int pegar_robos_novos(void)
{
    int n;

    pthread_mutex_lock(&mutex_novos);
    n = robos_novos;
    robos_novos = 0;
    pthread_mutex_unlock(&mutex_novos);
    return n;
}

/* PRODUTOR (Console ou ponte MQTT): coloca um pedido e acorda o Gestor.
 * Se a fila estiver cheia, recusa (retorna false) em vez de bloquear quem
 * pediu (Console ou thread de rede do MQTT). Com x < 0 usa a posição
 * inicial padrão. */
bool pedir_novo_robo(int x, int y)
{
    bool aceito = false;

    pthread_mutex_lock(&mutex_pedidos);
    if (quantidade < MAX_PEDIDOS) {
        int fim = (inicio + quantidade) % MAX_PEDIDOS;
        pedidos[fim].x = (x < 0) ? POS_PADRAO_X : x;  /* posição padrão: mapa.h */
        pedidos[fim].y = (y < 0) ? POS_PADRAO_Y : y;
        quantidade++;
        aceito = true;
        pthread_cond_signal(&cond_pedidos); /* acorda o Gestor */
    }
    pthread_mutex_unlock(&mutex_pedidos);
    return aceito;
}

/* Encerramento: acorda o Gestor para ele ver encerrar = true.
 * O broadcast é feito COM o mutex travado: assim ele não acontece no meio
 * do "testa a condição e dorme" do Gestor (o aviso não se perde). */
void acordar_gestor(void)
{
    pthread_mutex_lock(&mutex_pedidos);
    pthread_cond_broadcast(&cond_pedidos);
    pthread_mutex_unlock(&mutex_pedidos);
}

/* Procura um slot livre (ativo == false). Retorna o id ou -1. */
static int escolher_id_livre(void)
{
    int i;
    bool livre;

    for (i = 0; i < MAX_ROBOS; i++) {
        pthread_mutex_lock(&mem->robos[i].mutex);
        livre = !mem->robos[i].ativo;
        pthread_mutex_unlock(&mem->robos[i].mutex);
        if (livre)
            return i;
    }
    return -1;
}

/* Prepara o slot do robô na memória compartilhada. */
static void iniciar_slot(int id, int x, int y)
{
    Robo *r = &mem->robos[id];

    /* Mutex: o comando "status" do Console pode estar lendo este slot. */
    pthread_mutex_lock(&r->mutex);
    memset(&r->hw, 0, sizeof(r->hw));
    r->hw.i_posicao_x = x;
    r->hw.i_posicao_y = y;
    r->hw.o_sp_posicao_x = x;   /* setpoint = posição: robô parado */
    r->hw.o_sp_posicao_y = y;
    r->hw.i_percentual_bateria = 100;
    /* Valores iniciais do buffer de status. Só aqui alguém além da Status
     * escreve neles: o processo robo ainda não existe. */
    r->e_posicao_x = x;
    r->e_posicao_y = y;
    r->j_sp_posicao_x = x;
    r->j_sp_posicao_y = y;
    r->bateria = 100;
    r->e_estado = DISPONIVEL;
    r->distancia_total = 0.0;
    r->destino_x = -1;          /* sem rota */
    r->destino_y = -1;
    r->n_rota = 0;
    r->bloqueado = false;
    r->modo_automatico = true;
    r->carga_id = -1;
    r->grupo = id;              /* sozinho: grupo anti-colisão = id */
    r->celula_x = x;            /* célula da grade (a Navegação atualiza) */
    r->celula_y = y;
    r->pid = 0;
    r->ativo = true;  /* a partir daqui o Organizador "vê" o robô */
    pthread_mutex_unlock(&r->mutex);
}

/* Cria a fila de mensagens /atr_robo_<id> (Organizador -> Navegação). */
static void criar_fila_robo(int id)
{
    struct mq_attr atributos = {0};
    char nome[32];
    mqd_t fila;

    atributos.mq_maxmsg  = MAX_MSG_FILA;
    atributos.mq_msgsize = sizeof(MensagemRobo);
    snprintf(nome, sizeof(nome), "%s%d", PREFIXO_FILA_ROBO, id);

    mq_unlink(nome); /* remove sobra de execução anterior */
    fila = mq_open(nome, O_CREAT | O_RDWR, 0600, &atributos);
    if (fila == (mqd_t)-1) { perror("mq_open robo"); exit(1); }
    mq_close(fila); /* o nome continua existindo; quem usa abre de novo */
}

/* Envia uma mensagem para a Navegação do robô "id".
 * Abre e fecha a fila a cada envio: simples e as mensagens são raras.
 * Se a fila estiver cheia (10 mensagens), mq_send espera esvaziar. */
void enviar_para_robo(int id, const MensagemRobo *msg)
{
    char nome[32];
    mqd_t fila;

    snprintf(nome, sizeof(nome), "%s%d", PREFIXO_FILA_ROBO, id);
    fila = mq_open(nome, O_WRONLY);
    if (fila == (mqd_t)-1) { perror("mq_open robo"); exit(1); }
    if (mq_send(fila, (const char *)msg, sizeof(*msg), 0) == -1) {
        perror("mq_send robo");
        exit(1);
    }
    mq_close(fila);
}

/* Cria o processo do robô: fork() e, no filho, execl() IMEDIATAMENTE.
 * POR QUE exec logo em seguida: este processo tem várias threads; no filho
 * só existe a thread que chamou fork, e um mutex que outra thread segurava
 * ficaria travado para sempre. O exec troca o programa e o filho começa
 * limpo. */
static pid_t criar_processo_robo(int id)
{
    char id_texto[16];
    pid_t pid;

    snprintf(id_texto, sizeof(id_texto), "%d", id);
    pid = fork();
    if (pid == -1) { perror("fork"); exit(1); }
    if (pid == 0) {
        execl("./robo", "robo", id_texto, (char *)NULL);
        perror("execl ./robo"); /* só chega aqui se o exec falhou */
        _exit(1); /* _exit: não roda nada herdado do processo central */
    }
    return pid;
}

/* Atende um pedido: cria tudo que o robô precisa. */
static void atender_pedido(Pedido p)
{
    char texto[128];
    int id = escolher_id_livre();
    int x, y;
    pid_t pid;

    if (id == -1) {
        registrar_evento(fila_coletor, -1, "pedido de novo robo recusado: sem id livre");
        printf("Gestor: sem id livre (maximo %d robos)\n", MAX_ROBOS);
        return;
    }
    /* Anti-colisão: a célula pedida pode já ter robô; usa a livre mais
     * perto (BFS) e já a reserva com o grupo do robô novo (= id). */
    x = p.x;
    y = p.y;
    if (!reservar_livre_perto(&x, &y, id)) {
        registrar_evento(fila_coletor, -1, "pedido de novo robo recusado: sem celula livre");
        return;
    }
    iniciar_slot(id, x, y);
    criar_fila_robo(id);
    pid = criar_processo_robo(id);

    pthread_mutex_lock(&mem->robos[id].mutex);
    mem->robos[id].pid = pid;
    pthread_mutex_unlock(&mem->robos[id].mutex);

    pthread_mutex_lock(&mutex_novos);
    robos_novos++;  /* avisa o Organizador (ele vê no próximo ciclo) */
    pthread_mutex_unlock(&mutex_novos);

    snprintf(texto, sizeof(texto), "robo criado (pid %d) em (%d,%d)",
             (int)pid, x, y);
    registrar_evento(fila_coletor, id, texto);
    if (x != p.x || y != p.y)
        printf("Gestor: (%d,%d) ocupada; robo %d criado (pid %d) em (%d,%d)\n",
               p.x, p.y, id, (int)pid, x, y);
    else
        printf("Gestor: robo %d criado (pid %d) em (%d,%d)\n", id, (int)pid, x, y);
}

/* CONSUMIDOR: thread do Gestor. */
void *tarefa_gestor(void *arg)
{
    Pedido p;

    (void)arg;
    ativar_tempo_real(PRIO_GESTOR, "gestor");
    while (1) {
        pthread_mutex_lock(&mutex_pedidos);
        /* "while" e não "if": ao acordar, testa a condição de novo
         * (o sinal pode ser de encerramento ou um despertar espúrio). */
        while (quantidade == 0 && !atomic_load(&mem->encerrar))
            pthread_cond_wait(&cond_pedidos, &mutex_pedidos);

        if (atomic_load(&mem->encerrar)) {
            pthread_mutex_unlock(&mutex_pedidos);
            break;
        }
        p = pedidos[inicio];
        inicio = (inicio + 1) % MAX_PEDIDOS;
        quantidade--;
        pthread_mutex_unlock(&mutex_pedidos);

        /* Fora do mutex: criar processo é demorado e quem pede (Console
         * ou ponte) não deve ficar esperando por isso. */
        atender_pedido(p);
    }
    return NULL;
}
