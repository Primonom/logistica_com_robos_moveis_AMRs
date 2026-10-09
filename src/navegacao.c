/*
 * navegacao.c — Tarefa "Comando de Navegação" (thread do processo robo).
 *
 * Thread: tarefa_navegacao, periódica com período PERIODO_NAVEGACAO_MS
 * (T1 = 100 ms). A cada ciclo:
 *   1) lê as mensagens da fila /atr_robo_<id> sem bloquear e trata cada uma
 *      (GOTO, MODO_MANUAL/AUTO, TAREFA_IR_BAIA, ACOPLAR, IR_ZONA, SOLTAR);
 *   2) se mandou erguer a carga, verifica se o hardware confirmou;
 *   3) se está PARADO (flag "parar" da Status = DEFEITO, ou PAUSAR do
 *      Organizador porque alguém da equipe está em DEFEITO): só mantém o
 *      setpoint na posição atual;
 *   4) senão: se a Status pediu recarga, vai para a estação; se é SEGUIDOR
 *      num transporte, só copia o setpoint do líder; senão inicia a rota
 *      pendente (se houver) e segue a rota, escrevendo o setpoint
 *      (o_sp_posicao_x/y) do waypoint atual.
 *
 * Estados (decididos aqui e publicados pela Status em e_estado):
 *   GOTO           (só em MANUAL)  MANUAL durante e ao chegar
 *   TAREFA_IR_BAIA                 DESLOCANDO, ao chegar AGUARDANDO_EQUIPE
 *   ACOPLAR                        ao confirmar a carga, OPERANDO_CARGA
 *   IR_ZONA                        OPERANDO_CARGA até o SOLTAR
 *   SOLTAR                         DISPONIVEL
 *   ir_recarregar (da Status)      RECARREGANDO (a Status volta a
 *                                  DISPONIVEL quando chega a 100%)
 *   Enquanto o robô está em DEFEITO, uma mudança de estado pedida aqui é
 *   guardada em estado_antes (vale quando o defeito sair).
 *
 * Transporte em equipe: a equipe é uma unidade. Só o LÍDER (menor id)
 * calcula a rota e usa os semáforos das passagens. Os SEGUIDORES não fazem
 * sem_wait/sem_post: copiam o setpoint publicado pelo líder no buffer de
 * status dele (j_sp_posicao_x/y). Assim a equipe ocupa 1 vaga na passagem.
 *
 * Passagens estreitas: antes de seguir para um waypoint marcado com uma
 * passagem, pega o semáforo dela; ao chegar nesse waypoint (já fora da
 * passagem), devolve. Um robô nunca segura duas passagens ao mesmo tempo,
 * o que quebra a condição "segura e espera" e evita deadlock.
 *
 * Sincronização:
 *   - fila de mensagens /atr_robo_<id>: recebe comandos de outro processo
 *     (Console/Organizador) sem memória compartilhada nem mutex extra.
 *   - semáforos contadores nomeados /atr_passagem_<n> (valor inicial =
 *     capacidade): limitam quantos robôs ficam dentro de cada passagem.
 *     sem_timedwait com prazo curto: se a passagem está cheia, o robô
 *     desiste NESTE ciclo e tenta de novo no próximo; assim a thread não
 *     fica presa e continua lendo mensagens (e, no passo 7, falhas).
 *   - mutex do robô (PROCESS_SHARED): lê a posição e escreve o setpoint
 *     enquanto o simulador (outro processo) também usa o hw.
 *   - mutex do robô LÍDER (seguidor): lê o j_sp do líder sem pegar a
 *     Status dele no meio de uma atualização. Os dois mutexes (líder e o
 *     meu) nunca ficam travados ao mesmo tempo: sem risco de deadlock.
 *   - estado_local.mutex: troca de informação com a Status (outra thread):
 *     estado, e as flags parar e ir_recarregar que a Status liga.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include "comum.h"
#include "mapa.h"

#define ESPERA_SEMAFORO_MS 10  /* prazo do sem_timedwait em cada ciclo */

static mqd_t fila;                      /* /atr_robo_<id> (só leitura) */
static sem_t *semaforos[N_PASSAGENS];   /* /atr_passagem_1..3 */

/* Variáveis usadas só por esta thread (não precisam de mutex). */
static Rota rota;
static int  indice = 0;            /* waypoint atual (rota.n = sem rota) */
static int  passagem_atual = -1;   /* passagem que estou segurando */
static bool avisou_espera = false; /* já registrou "esperando passagem"? */
static bool tem_destino = false;   /* há destino ainda não iniciado? */
static int  destino_x, destino_y;
static Estado estado_ao_chegar = DISPONIVEL; /* estado quando a rota acabar */
static bool acoplando = false;     /* mandou erguer; espera confirmação */
static int  lider = -1;            /* >= 0: sou seguidor deste líder */
static bool pausado = false;       /* PAUSAR do Organizador (equipe parada) */

static void abrir_ipc(void)
{
    char nome[32];
    int i;

    snprintf(nome, sizeof(nome), "%s%d", PREFIXO_FILA_ROBO, id_robo);
    fila = mq_open(nome, O_RDONLY);
    if (fila == (mqd_t)-1) { perror("mq_open robo"); exit(1); }

    for (i = 0; i < N_PASSAGENS; i++) {
        snprintf(nome, sizeof(nome), "%s%d", PREFIXO_SEM_PASSAGEM, i + 1);
        semaforos[i] = sem_open(nome, 0);
        if (semaforos[i] == SEM_FAILED) { perror("sem_open"); exit(1); }
    }
}

/* Muda o estado. Se o robô está em DEFEITO, não apaga o DEFEITO: guarda
 * o novo estado para quando o defeito sair. O teste e a escrita ficam sob
 * o mesmo mutex, então a Status não entra em DEFEITO no meio deles. */
static void definir_estado(Estado e)
{
    pthread_mutex_lock(&estado_local.mutex);
    if (estado_local.estado == DEFEITO)
        estado_local.estado_antes = e;
    else
        estado_local.estado = e;
    pthread_mutex_unlock(&estado_local.mutex);
}

static Estado ler_estado(void)
{
    Estado e;

    pthread_mutex_lock(&estado_local.mutex);
    e = estado_local.estado;
    pthread_mutex_unlock(&estado_local.mutex);
    return e;
}

/* Escreve o comando da garra no hw (o simulador lê). */
static void comandar_garra(bool erguer)
{
    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    mem->robos[id_robo].hw.o_acoplar_e_erguer = erguer;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
}

/* Modo automático/manual fica no slot: o Organizador lê para escolher
 * robôs (só usa os automáticos). */
static void definir_modo(bool automatico)
{
    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    mem->robos[id_robo].modo_automatico = automatico;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
}

/* Guarda um destino; a rota começa em iniciar_rota_pendente(). */
static void novo_destino(int x, int y, Estado durante, Estado ao_chegar)
{
    tem_destino = true;
    destino_x = x;
    destino_y = y;
    estado_ao_chegar = ao_chegar;
    definir_estado(durante);
}

/* Registra um evento deste robô no Coletor (texto com um número). */
static void evento(const char *formato, int valor)
{
    char texto[128];

    snprintf(texto, sizeof(texto), formato, valor);
    registrar_evento(fila_coletor, id_robo, texto);
}

/* Trata uma mensagem recebida. */
static void tratar_mensagem(const MensagemRobo *m)
{
    Estado e = ler_estado();

    switch (m->tipo) {
    case GOTO:
        if (e == MANUAL)
            novo_destino(m->x, m->y, MANUAL, MANUAL);
        else
            registrar_evento(fila_coletor, id_robo, "goto ignorado: robo nao esta em modo manual");
        break;
    case MODO_MANUAL:
        if (e == DISPONIVEL) {
            definir_modo(false);
            definir_estado(MANUAL);
            registrar_evento(fila_coletor, id_robo, "modo manual");
        } else {
            registrar_evento(fila_coletor, id_robo, "modo manual ignorado: robo ocupado");
        }
        break;
    case MODO_AUTO:
        if (e == MANUAL) {
            tem_destino = false;
            estado_ao_chegar = DISPONIVEL;
            definir_modo(true);
            definir_estado(DISPONIVEL);
            registrar_evento(fila_coletor, id_robo, "modo automatico");
        }
        break;
    case TAREFA_IR_BAIA:
        evento("tarefa: ir para a baia (caixa %d)", m->carga_id);
        novo_destino(m->x, m->y, DESLOCANDO, AGUARDANDO_EQUIPE);
        break;
    case ACOPLAR:
        comandar_garra(true);
        acoplando = true;
        break;
    case IR_ZONA:
        if (m->lider_id == id_robo) {
            novo_destino(m->x, m->y, OPERANDO_CARGA, OPERANDO_CARGA);
        } else {
            lider = m->lider_id;
            evento("seguindo o lider %d", lider);
        }
        break;
    case PAUSAR:
        pausado = true;
        registrar_evento(fila_coletor, id_robo, "pausado (equipe com defeito)");
        break;
    case CONTINUAR:
        pausado = false;
        registrar_evento(fila_coletor, id_robo, "continuando");
        break;
    case SOLTAR:
        comandar_garra(false);
        pausado = false;
        lider = -1;
        rota.n = 0;
        indice = 0;
        tem_destino = false;
        definir_estado(DISPONIVEL);
        registrar_evento(fila_coletor, id_robo, "carga solta");
        break;
    }
}

/* 1) Lê todas as mensagens que já chegaram, sem esperar.
 * mq_timedreceive com prazo = agora: se a fila está vazia, retorna na
 * hora com ETIMEDOUT e o ciclo continua. */
static void ler_mensagens(void)
{
    MensagemRobo msg;
    struct timespec prazo;

    while (1) {
        clock_gettime(CLOCK_REALTIME, &prazo); /* mq usa CLOCK_REALTIME */
        if (mq_timedreceive(fila, (char *)&msg, sizeof(msg), NULL, &prazo) == -1) {
            if (errno == ETIMEDOUT) return; /* fila vazia */
            perror("mq_timedreceive");
            exit(1);
        }
        tratar_mensagem(&msg);
    }
}

/* 2) Depois do ACOPLAR: quando o hardware confirma a carga atracada,
 * passa a OPERANDO_CARGA (o Organizador espera isso de TODOS). */
static void verificar_acoplamento(void)
{
    bool atracada;

    if (!acoplando)
        return;
    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    atracada = mem->robos[id_robo].hw.i_carga_atracada;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
    if (atracada) {
        acoplando = false;
        definir_estado(OPERANDO_CARGA);
        registrar_evento(fila_coletor, id_robo, "carga erguida");
    }
}

/* 3a) Seguidor: copia o setpoint do líder (lido do buffer de status dele).
 * Não usa semáforos: quem ocupa a vaga na passagem é o líder. */
static void seguir_lider(void)
{
    int x, y;

    pthread_mutex_lock(&mem->robos[lider].mutex);
    x = mem->robos[lider].j_sp_posicao_x;
    y = mem->robos[lider].j_sp_posicao_y;
    pthread_mutex_unlock(&mem->robos[lider].mutex);

    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    mem->robos[id_robo].hw.o_sp_posicao_x = x;
    mem->robos[id_robo].hw.o_sp_posicao_y = y;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
}

/* 3b) Começa a rota pendente. Só se não estiver dentro de uma passagem:
 * a rota nova parte da posição atual, que precisa ser fora delas. */
static void iniciar_rota_pendente(void)
{
    Robo *r = &mem->robos[id_robo];
    int x, y;

    if (!tem_destino || passagem_atual != -1)
        return;
    pthread_mutex_lock(&r->mutex);
    x = r->hw.i_posicao_x;
    y = r->hw.i_posicao_y;
    pthread_mutex_unlock(&r->mutex);

    calcular_rota(x, y, destino_x, destino_y, &rota);
    indice = 0;
    tem_destino = false;
}

/* Tenta pegar o semáforo da passagem p, esperando no máximo
 * ESPERA_SEMAFORO_MS. Retorna true se conseguiu. */
static bool tentar_entrar(int p)
{
    struct timespec prazo;

    clock_gettime(CLOCK_REALTIME, &prazo); /* sem_timedwait usa REALTIME */
    somar_ms(&prazo, ESPERA_SEMAFORO_MS);
    if (sem_timedwait(semaforos[p], &prazo) == -1) {
        if (errno != ETIMEDOUT && errno != EINTR) { perror("sem_timedwait"); exit(1); }
        if (!avisou_espera) {
            evento("esperando passagem P%d", p + 1);
            avisou_espera = true;
        }
        return false;
    }
    passagem_atual = p;
    avisou_espera = false;
    evento("entrou na passagem P%d", p + 1);
    return true;
}

/* 3c) Segue a rota: um passo de controle por ciclo. */
static void seguir_rota(void)
{
    Robo *r = &mem->robos[id_robo];
    Waypoint w;
    bool chegou;

    if (indice >= rota.n)
        return; /* sem rota */
    w = rota.pontos[indice];

    /* Precisa de uma passagem que ainda não segura? Pede o semáforo antes
     * de mandar o robô entrar. Sem vaga: fica parado (setpoint antigo). */
    if (w.passagem != -1 && passagem_atual != w.passagem && !tentar_entrar(w.passagem))
        return;

    pthread_mutex_lock(&r->mutex);
    chegou = (r->hw.i_posicao_x == w.x && r->hw.i_posicao_y == w.y);
    if (!chegou) {
        r->hw.o_sp_posicao_x = w.x;
        r->hw.o_sp_posicao_y = w.y;
    }
    pthread_mutex_unlock(&r->mutex);

    if (!chegou)
        return;
    if (w.passagem != -1) { /* chegou ao ponto depois da passagem: libera */
        sem_post(semaforos[w.passagem]);
        passagem_atual = -1;
        evento("saiu da passagem P%d", w.passagem + 1);
    }
    indice++;
    if (indice == rota.n) {
        definir_estado(estado_ao_chegar);
        registrar_evento(fila_coletor, id_robo, "chegou ao destino");
    }
}

/* 3) Deve ficar parado? (DEFEITO, pela flag da Status, ou PAUSAR.) */
static bool deve_parar(void)
{
    bool parar;

    pthread_mutex_lock(&estado_local.mutex);
    parar = estado_local.parar;
    pthread_mutex_unlock(&estado_local.mutex);
    return parar || pausado;
}

/* 3) Parado: o setpoint vira a posição atual (o robô não anda).
 * A rota e a passagem que ele segura NÃO são liberadas: um robô parado
 * dentro de uma passagem continua ocupando a vaga, porque fisicamente ele
 * está bloqueando o caminho. Os outros esperam no semáforo. */
static void segurar_posicao(void)
{
    Robo *r = &mem->robos[id_robo];

    pthread_mutex_lock(&r->mutex);
    r->hw.o_sp_posicao_x = r->hw.i_posicao_x;
    r->hw.o_sp_posicao_y = r->hw.i_posicao_y;
    pthread_mutex_unlock(&r->mutex);
}

/* 4) A Status pediu recarga? Vai para a estação que ela escolheu. */
static void verificar_recarga(void)
{
    bool ir;
    int x, y;

    pthread_mutex_lock(&estado_local.mutex);
    ir = estado_local.ir_recarregar;
    x = estado_local.recarga_x;
    y = estado_local.recarga_y;
    estado_local.ir_recarregar = false;
    pthread_mutex_unlock(&estado_local.mutex);

    if (ir)
        novo_destino(x, y, RECARREGANDO, RECARREGANDO);
}

void *tarefa_navegacao(void *arg)
{
    struct timespec proximo;

    (void)arg;
    abrir_ipc();
    rota.n = 0;

    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        ler_mensagens();
        verificar_acoplamento();
        if (deve_parar()) {
            segurar_posicao();
        } else {
            verificar_recarga();
            if (lider >= 0) {
                seguir_lider();
            } else {
                iniciar_rota_pendente();
                seguir_rota();
            }
        }

        somar_ms(&proximo, PERIODO_NAVEGACAO_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }
    return NULL;
}
