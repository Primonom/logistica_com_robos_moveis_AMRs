/*
 * navegacao.c — Tarefa "Comando de Navegação" (thread do processo robo).
 *
 * Thread: tarefa_navegacao, periódica com período PERIODO_NAVEGACAO_MS
 * (T1 = 100 ms). A cada ciclo:
 *   1) lê as mensagens da fila /atr_robo_<id> sem bloquear e trata cada uma
 *      (GOTO, MODO_MANUAL/AUTO, TAREFA_IR_BAIA, ACOPLAR, IR_ZONA, SOLTAR,
 *      PAUSAR/CONTINUAR, RECARREGAR);
 *   2) se mandou erguer a carga, verifica se o hardware confirmou;
 *   3) se está PARADO (flag "parar" da Status = DEFEITO, ou PAUSAR do
 *      Organizador porque alguém da equipe está em DEFEITO): só mantém o
 *      setpoint na posição atual;
 *   4) senão: se a Status pediu recarga, vai para a estação; se é SEGUIDOR
 *      num transporte, dá um passo em direção ao líder; senão calcula o
 *      caminho pendente (se houver) e segue o caminho.
 *
 * Caminho: calculado com A* (calcular_caminho, mapa.c) = lista de CÉLULAS.
 * O robô anda UMA célula por vez: o setpoint (o_sp_posicao_x/y) é sempre a
 * próxima célula do caminho. Como ela é vizinha, o simulador (que anda
 * primeiro em x, depois em y) nunca corta o canto de um obstáculo.
 *
 * Estados (decididos aqui e publicados pela Status em e_estado):
 *   GOTO           (só em MANUAL)  MANUAL durante e ao chegar
 *   TAREFA_IR_BAIA                 DESLOCANDO, ao chegar AGUARDANDO_EQUIPE
 *   ACOPLAR                        ao confirmar a carga, OPERANDO_CARGA
 *   IR_ZONA                        OPERANDO_CARGA até o SOLTAR
 *   SOLTAR                         DISPONIVEL
 *   ir_recarregar (da Status)      RECARREGANDO (a Status volta a
 *                                  DISPONIVEL quando chega a 100%)
 *   RECARREGAR (do Organizador,    RECARREGANDO, igual ao de cima
 *     só se DISPONIVEL)
 *   TAREFA_IR_BAIA também CANCELA um ir_recarregar ainda não atendido
 *   (ver "Bateria suficiente para a tarefa" em docs/EXPLICACAO.md).
 *   Enquanto o robô está em DEFEITO, uma mudança de estado pedida aqui é
 *   guardada em estado_antes (vale quando o defeito sair).
 *
 * Anti-colisão descentralizada (ponto extra +2; regras em ocupacao.c):
 * TODA decisão de colisão é tomada aqui, na Navegação de cada robô; o
 * Organizador não participa. Custo O(1) por passo (testar e marcar 1
 * célula), fora o replanejamento ocasional do desvio.
 *   - grupo: o id do robô, ou GRUPO_EQUIPE + carga_id numa equipe de
 *     transporte com mais de 1 robô (muda no IR_ZONA e no SOLTAR, sempre
 *     numa célula de estacionamento: baia ou zona, que não são marcadas);
 *   - protocolo de cada passo, sob o mutex da grade (dar_passo):
 *       1) reserva a próxima célula (se a regra deixa); senão ESPERA
 *          neste ciclo (setpoint = posição atual);
 *       2) quando o simulador confirma que chegou nela, devolve a anterior
 *          (confirmar_chegada). Cada robô segura no máximo 2 células;
 *   - travamento (ex.: dois robôs frente a frente): bloqueado pela grade
 *     por mais de 10 ciclos (1 s), recalcula o A* tratando como obstáculo
 *     as células de outros grupos ("desviando de robo"). Desempate: só o
 *     de MAIOR grupo (= maior id, para robôs sozinhos) desvia a partir de
 *     10 ciclos; o outro só a partir de 30. Assim os dois não desviam ao
 *     mesmo tempo (e se travariam de novo). Quem está dentro de uma
 *     passagem desvia sempre com 10 (o túnel precisa esvaziar);
 *   - robô em DEFEITO (ou pausado) continua segurando as suas células:
 *     fisicamente ele está ali; os outros desviam dele pelo mesmo mecanismo;
 *   - seguidores também usam a grade (entram livres nas células do próprio
 *     grupo). O desvio do seguidor nunca usa uma passagem (quem ocupa a
 *     vaga é o líder);
 *   - no encerramento (SIGTERM) o robô não devolve as células: o sistema
 *     inteiro está terminando e a memória compartilhada será removida.
 *
 * Passagens (região = túnel + 1 boca de cada lado, ver mapa.h):
 *   - quando a PRÓXIMA célula é da passagem p e a atual não, pede entrada
 *     no MONITOR de mão única de p (entrar_passagem, ocupacao.c: sentido
 *     + semáforo da capacidade) ANTES de mandar o robô entrar; sem
 *     entrada (cheia ou contramão): fica parado e tenta no próximo ciclo;
 *   - quando o robô (que já esteve dentro) chega numa célula fora da
 *     região de p, sai do monitor (sair_passagem: dentro-- e sem_post);
 *   - um robô nunca segura duas passagens: as regiões ficam separadas por
 *     mais de 1 célula (mapa.c), então ele sempre passa por uma célula
 *     livre (e libera p) antes de chegar à próxima. Se ainda segura p (a
 *     equipe não saiu), ele NÃO pede outra: espera. Isso quebra a
 *     condição "segura e espera" e evita deadlock;
 *   - esperando mais de 3 s na porta: recalcula o A* a partir de onde
 *     está. Com o custo dinâmico (passagem cheia ou na contramão custa
 *     mais) ele pode escolher outra passagem. Evento "replanejou rota:
 *     passagem Pn cheia";
 *   - o desvio de travamento DENTRO de uma passagem nunca volta para trás
 *     no túnel (respeita a mão única).
 *
 * Transporte em equipe: a equipe é uma unidade. Só o LÍDER calcula o
 * caminho até a zona e usa os semáforos. Cada SEGUIDOR calcula, a cada
 * ciclo, o A* até a posição atual do líder (buffer de status dele) e dá o
 * primeiro passo; não faz sem_wait/sem_post (a equipe ocupa 1 vaga). O
 * líder recebe a lista de membros no IR_ZONA e só devolve uma passagem
 * quando ele E todos os membros já saíram da região dela.
 *
 * Sincronização:
 *   - fila de mensagens /atr_robo_<id>: recebe comandos de outro processo
 *     (Console/Organizador) sem memória compartilhada nem mutex extra.
 *   - semáforos contadores nomeados /atr_passagem_<n> (valor inicial =
 *     capacidade sorteada): limitam quantos robôs ficam dentro de cada
 *     passagem. sem_trywait (dentro do monitor): se a passagem está cheia,
 *     o robô desiste NESTE ciclo e tenta de novo no próximo; assim a
 *     thread não fica presa e continua lendo mensagens e a flag "parar".
 *   - monitor de mão única (mem->monitor[p], mutex PROCESS_SHARED) e grade
 *     de ocupação (mem->ocupacao, mutex PROCESS_SHARED): ver ocupacao.c.
 *     Nunca são travados junto com outro mutex (as funções de ocupacao.c
 *     travam, mexem e soltam).
 *   - mutex do robô (PROCESS_SHARED): lê a posição e escreve o setpoint
 *     enquanto o simulador (outro processo) também usa o hw. Para PARAR,
 *     lê a posição e escreve o setpoint nela no MESMO lock
 *     (parar_onde_esta): senão o simulador poderia mover o robô entre a
 *     leitura e a escrita, para uma célula que ele acabou de devolver.
 *   - mutex de OUTRO robô (líder, para o seguidor; membros, para o líder):
 *     lê só o buffer de status dele (e_posicao, destino). Nunca junto com o
 *     meu mutex: copia para variáveis locais e solta antes do próximo lock
 *     (regra anti-deadlock do projeto).
 *   - estado_local.mutex: troca de informação com a Status (outra thread):
 *     estado, destino e cantos do caminho (que a Status publica no
 *     buffer), as flags parar e ir_recarregar que a Status liga e a flag
 *     bloqueado (esperando célula ou passagem) que a Status publica.
 *   - mem->mapa: só leitura, sem mutex (ver mapa.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include "comum.h"
#include "mapa.h"

#define REPLANEJAR_APOS_S    3  /* espera na porta antes de recalcular o A* */
#define DESVIAR_MAIOR_APOS  10  /* ciclos bloqueado pela grade (maior grupo) */
#define DESVIAR_MENOR_APOS  30  /* ... (menor grupo: espera o outro desviar) */

static mqd_t fila;                      /* /atr_robo_<id> (só leitura) */

/* Variáveis usadas só por esta thread (não precisam de mutex). */
static Caminho caminho;
static int  indice = 0;            /* próxima célula do caminho (caminho.n = sem caminho) */
static int  passagem_atual = -1;   /* passagem que estou segurando */
static bool avisou_espera = false; /* já registrou "esperando passagem"? */
static struct timespec espera_desde; /* quando começou a esperar (CLOCK_MONOTONIC) */
static bool tem_destino = false;   /* há destino com caminho ainda não calculado? */
static int  destino_x, destino_y;  /* destino atual (vale também no replanejamento) */
static Estado estado_ao_chegar = DISPONIVEL; /* estado quando o caminho acabar */
static bool acoplando = false;     /* mandou erguer; espera confirmação */
static int  lider = -1;            /* >= 0: sou seguidor deste líder */
static bool pausado = false;       /* PAUSAR do Organizador (equipe parada) */
/* Só no líder durante o transporte: os outros membros da equipe e se cada
 * um já foi VISTO dentro da passagem que o líder segura.
 * POR QUE: o líder só faz sem_post quando a equipe inteira saiu; como o
 * buffer de status dos membros tem atraso, "fora" só vale depois de ter
 * sido visto "dentro" (senão um seguidor recém-entrado contaria como fora). */
static int  n_membros = 0;
static int  membros[TAMANHO_MAX_CAIXA];
static bool visto_dentro[TAMANHO_MAX_CAIXA];
/* Anti-colisão (só esta thread usa; a grade e o monitor ficam em
 * ocupacao.c). */
static int  meu_grupo;               /* id do robô ou GRUPO_EQUIPE + carga */
static int  cel_x, cel_y;            /* célula onde estou (reservada) */
static bool reservou = false;        /* reservei a próxima célula? */
static int  res_x, res_y;            /* a próxima célula, reservada */
static int  ciclos_bloqueado = 0;    /* ciclos seguidos esperando célula */
static int  grupo_bloqueador = 0;    /* grupo de quem está na célula que eu quero */
static bool bloqueado = false;       /* neste ciclo esperou célula ou passagem? */
static bool entrou_na_passagem = false; /* já estive dentro da região de passagem_atual? */
static int  sentido_atual = 0;       /* sentido em que peguei passagem_atual */

static void abrir_fila(void)
{
    char nome[32];

    snprintf(nome, sizeof(nome), "%s%d", PREFIXO_FILA_ROBO, id_robo);
    fila = mq_open(nome, O_RDONLY);
    if (fila == (mqd_t)-1) { perror("mq_open robo"); exit(1); }
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

/* Informa à Status o fim do caminho atual (-1, -1 = sem caminho), para ela
 * publicar no buffer de status (a tela desenha a bandeirinha lá). */
static void publicar_destino(int x, int y)
{
    pthread_mutex_lock(&estado_local.mutex);
    estado_local.destino_x = x;
    estado_local.destino_y = y;
    pthread_mutex_unlock(&estado_local.mutex);
}

/* A célula k do caminho é um "canto" (a direção muda nela)? */
static bool canto(int k)
{
    return caminho.x[k] - caminho.x[k - 1] != caminho.x[k + 1] - caminho.x[k] ||
           caminho.y[k] - caminho.y[k - 1] != caminho.y[k + 1] - caminho.y[k];
}

/* Informa à Status só os CANTOS do caminho que falta (e o destino), no
 * máximo MAX_ROTA_STATUS: a tela liga os pontos com retas. Sem caminho:
 * n_rota = 0. */
static void publicar_rota(void)
{
    int k, n = 0;

    pthread_mutex_lock(&estado_local.mutex);
    for (k = indice; k < caminho.n && n < MAX_ROTA_STATUS; k++) {
        if (k == caminho.n - 1 || (k > 0 && canto(k))) {
            estado_local.rota_x[n] = caminho.x[k];
            estado_local.rota_y[n] = caminho.y[k];
            n++;
        }
    }
    estado_local.n_rota = n;
    pthread_mutex_unlock(&estado_local.mutex);
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

/* Posição atual do MEU robô (hw: este processo é o robô). */
static void ler_posicao(int *x, int *y)
{
    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    *x = mem->robos[id_robo].hw.i_posicao_x;
    *y = mem->robos[id_robo].hw.i_posicao_y;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
}

static void escrever_setpoint(int x, int y)
{
    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    mem->robos[id_robo].hw.o_sp_posicao_x = x;
    mem->robos[id_robo].hw.o_sp_posicao_y = y;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
}

/* Para o robô onde ele está AGORA: lê a posição e escreve o setpoint nela
 * no MESMO lock. Separado (ler, soltar, escrever), o simulador poderia
 * mover o robô no meio, para uma célula que já foi devolvida. */
static void parar_onde_esta(int *x, int *y)
{
    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    *x = mem->robos[id_robo].hw.i_posicao_x;
    *y = mem->robos[id_robo].hw.i_posicao_y;
    mem->robos[id_robo].hw.o_sp_posicao_x = *x;
    mem->robos[id_robo].hw.o_sp_posicao_y = *y;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
}

/* Troca o grupo anti-colisão. Fica também no slot: o simulador lê para
 * conferir colisões (ver simulador.c). */
static void definir_grupo(int grupo)
{
    meu_grupo = grupo;
    pthread_mutex_lock(&mem->robos[id_robo].mutex);
    mem->robos[id_robo].grupo = grupo;
    pthread_mutex_unlock(&mem->robos[id_robo].mutex);
}

/* Passo 2 do protocolo (início de cada ciclo): se o simulador já levou o
 * robô para a célula reservada, devolve a anterior. */
static void confirmar_chegada(void)
{
    int x, y;

    if (!reservou)
        return;
    ler_posicao(&x, &y);
    if (x == res_x && y == res_y) {
        liberar_celula(cel_x, cel_y);
        cel_x = x;
        cel_y = y;
        reservou = false;
    }
}

/* O caminho mudou e a próxima célula reservada não serve mais: para o robô
 * e desfaz a reserva. Se ele já tinha chegado nela, devolve a anterior e
 * retorna true. */
static bool cancelar_reserva(void)
{
    int x, y;

    parar_onde_esta(&x, &y);
    reservou = false;
    if (x == res_x && y == res_y) {
        liberar_celula(cel_x, cel_y);
        cel_x = x;
        cel_y = y;
        return true;
    }
    liberar_celula(res_x, res_y);
    return false;
}

/* Passo 1 do protocolo: um passo para a célula VIZINHA (nx, ny) — ou ficar,
 * se (nx, ny) é a célula atual. Reserva a célula e só então escreve o
 * setpoint. Retorna false se ela é de outro grupo: o robô espera parado
 * neste ciclo (e conta o ciclo para o desvio). */
static bool dar_passo(int nx, int ny)
{
    if (reservou && (nx != res_x || ny != res_y) && cancelar_reserva())
        return true;   /* chegou bem agora em outra célula: decide no próximo ciclo */
    if (!reservou) {
        if (nx == cel_x && ny == cel_y) {
            escrever_setpoint(cel_x, cel_y);
            return true;
        }
        if (!reservar_celula(nx, ny, meu_grupo, &grupo_bloqueador)) {
            escrever_setpoint(cel_x, cel_y);   /* espera onde está */
            ciclos_bloqueado++;
            bloqueado = true;
            return false;
        }
        reservou = true;
        res_x = nx;
        res_y = ny;
    }
    ciclos_bloqueado = 0;
    escrever_setpoint(nx, ny);
    return true;
}

/* Desempate dos travamentos: já esperou o bastante para desviar? O de
 * MAIOR grupo desvia primeiro (10 ciclos); o outro espera 30. Exceção:
 * quem está DENTRO da passagem que segura também desvia com 10 (o túnel
 * precisa esvaziar para o outro sentido passar; quem o bloqueia na boca é
 * justamente quem espera para entrar). */
static bool hora_de_desviar(void)
{
    int x, y, limite = DESVIAR_MENOR_APOS;

    ler_posicao(&x, &y);
    if (meu_grupo > grupo_bloqueador ||
        (passagem_atual != -1 && passagem_em(x, y) == passagem_atual))
        limite = DESVIAR_MAIOR_APOS;

    if (ciclos_bloqueado <= limite)
        return false;
    ciclos_bloqueado = 0;   /* tenta desviar uma vez e recomeça a contagem */
    return true;
}

/* Posição de outro robô, pelo BUFFER DE STATUS dele (nunca o hw). */
static void posicao_de(int id, int *x, int *y)
{
    pthread_mutex_lock(&mem->robos[id].mutex);
    *x = mem->robos[id].e_posicao_x;
    *y = mem->robos[id].e_posicao_y;
    pthread_mutex_unlock(&mem->robos[id].mutex);
}

/* Guarda um destino; o caminho é calculado em calcular_pendente(). */
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

/* IR_ZONA para o líder: guarda os OUTROS membros da equipe. */
static void guardar_membros(const MensagemRobo *m)
{
    int k;

    n_membros = 0;
    for (k = 0; k < m->n_membros; k++)
        if (m->membros[k] != id_robo)
            membros[n_membros++] = m->membros[k];
}

/* RECARREGAR do Organizador: a bateria não basta para a caixa da vez.
 * Vai para a estação de menor caminho (a Status volta a DISPONIVEL quando
 * chegar a 100%, igual à recarga que ela mesma pede). */
static void ir_recarregar_agora(void)
{
    int x, y, ex, ey;

    ler_posicao(&x, &y);
    estacao_mais_proxima(x, y, &ex, &ey);
    novo_destino(ex, ey, RECARREGANDO, RECARREGANDO);
    registrar_evento(fila_coletor, id_robo,
                     "bateria insuficiente para a tarefa: indo recarregar");
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
        /* O Organizador já conferiu que a bateria basta para a tarefa
         * inteira; se a Status pediu recarga no mesmo instante (corrida
         * rara, ver EXPLICACAO.md), a tarefa vence: o pedido é apagado. */
        pthread_mutex_lock(&estado_local.mutex);
        estado_local.ir_recarregar = false;
        pthread_mutex_unlock(&estado_local.mutex);
        evento("tarefa: ir para a baia (caixa %d)", m->carga_id);
        novo_destino(m->x, m->y, DESLOCANDO, AGUARDANDO_EQUIPE);
        break;
    case RECARREGAR:
        if (e == DISPONIVEL)
            ir_recarregar_agora();
        break;
    case ACOPLAR:
        comandar_garra(true);
        acoplando = true;
        break;
    case IR_ZONA:
        /* A equipe vira um grupo só (entra nas células uns dos outros).
         * O robô está na baia (estacionamento, não marcada na grade). */
        definir_grupo(m->n_membros > 1 ? GRUPO_EQUIPE + m->carga_id : id_robo);
        if (m->lider_id == id_robo) {
            guardar_membros(m);
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
        definir_grupo(id_robo);   /* na zona (estacionamento): volta a ser sozinho */
        pausado = false;
        lider = -1;
        n_membros = 0;
        caminho.n = 0;
        indice = 0;
        tem_destino = false;
        publicar_destino(-1, -1);
        publicar_rota();          /* caminho.n = 0: rota vazia */
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

/* Desvio do seguidor: proíbe as células de outros grupos e TODAS as
 * passagens (quem ocupa a vaga é o líder; se o líder já está do outro
 * lado, o seguidor espera). */
static void proibir_passagens(bool proibida[])
{
    int x, y;

    for (y = 0; y < MAPA_ALTURA; y++)
        for (x = 0; x < MAPA_LARGURA; x++)
            if (dentro_de_passagem(x, y))
                proibida[y * MAPA_LARGURA + x] = true;
}

/* 4a) Seguidor: A* da minha posição até a posição ATUAL do líder (lida do
 * buffer de status dele) e um passo só, para a 1ª célula do caminho
 * (pela grade, como todo robô). Não usa o monitor nem o semáforo: quem
 * ocupa a vaga na passagem é o líder. O destino publicado é o do líder (a
 * equipe vai para o mesmo lugar). */
static void seguir_lider(void)
{
    Caminho c;
    bool proibida[N_CELULAS];
    int lx, ly, dx, dy, x, y;

    pthread_mutex_lock(&mem->robos[lider].mutex);
    lx = mem->robos[lider].e_posicao_x;
    ly = mem->robos[lider].e_posicao_y;
    dx = mem->robos[lider].destino_x;
    dy = mem->robos[lider].destino_y;
    pthread_mutex_unlock(&mem->robos[lider].mutex);
    publicar_destino(dx, dy);

    ler_posicao(&x, &y);
    if (!calcular_caminho(x, y, lx, ly, &c) || c.n < 2) {
        dar_passo(x, y);   /* já está com o líder (ou sem caminho): fica */
        return;
    }
    if (dar_passo(c.x[1], c.y[1]) || !hora_de_desviar())
        return;
    /* Travado: um passo pelo caminho que evita os outros robôs. */
    copiar_ocupadas(meu_grupo, proibida);
    proibir_passagens(proibida);
    proibida[ly * MAPA_LARGURA + lx] = false;
    if (calcular_caminho_evitando(x, y, lx, ly, proibida, &c) && c.n >= 2 &&
        dar_passo(c.x[1], c.y[1]))
        registrar_evento(fila_coletor, id_robo, "desviando de robo");
}

/* Calcula o caminho de (x, y) até o destino atual. caminho.x[0] é a
 * própria posição: com indice = 0 o próximo ciclo já "chega" nela e passa
 * para a célula seguinte (e um caminho de 1 célula = já está no destino). */
static void planejar(int x, int y)
{
    indice = 0;
    if (!calcular_caminho(x, y, destino_x, destino_y, &caminho)) {
        caminho.n = 0;
        publicar_destino(-1, -1);
        registrar_evento(fila_coletor, id_robo, "sem caminho ate o destino");
    } else {
        publicar_destino(destino_x, destino_y);
    }
    publicar_rota();
}

/* 4b) Calcula o caminho do destino pendente. Só fora de passagem: senão o
 * robô poderia sair por outro lado segurando o semáforo errado. */
static void calcular_pendente(void)
{
    int x, y;

    if (!tem_destino || passagem_atual != -1)
        return;
    tem_destino = false;
    ler_posicao(&x, &y);
    planejar(x, y);
}

/* Todos os membros da equipe já saíram da passagem p? Um membro só conta
 * como "saiu" depois de ter sido VISTO dentro dela: o buffer de status tem
 * até 200 ms de atraso, e um seguidor que acabou de entrar ainda pode
 * aparecer do lado de fora (antes da boca). Um mutex por vez. */
static bool equipe_saiu(int p)
{
    int k, x, y;
    bool saiu = true;

    for (k = 0; k < n_membros; k++) {
        posicao_de(membros[k], &x, &y);
        if (passagem_em(x, y) == p) {
            visto_dentro[k] = true;
            saiu = false;
        } else if (!visto_dentro[k]) {
            saiu = false;   /* ainda não entrou */
        }
    }
    return saiu;
}

/* Sai do monitor da passagem que estou segurando. */
static void soltar_passagem(void)
{
    sair_passagem(passagem_atual);
    evento("saiu da passagem P%d", passagem_atual + 1);
    passagem_atual = -1;
}

/* Devolve a passagem quando eu (em (x, y)) e a equipe já saímos dela.
 * "Saiu" só vale depois de ter estado DENTRO: entre pegar a passagem e
 * entrar o robô pode ficar uns ciclos parado na porta (esperando a boca
 * na grade), e isso não é "ter saído". */
static void liberar_se_saiu(int x, int y)
{
    int p = passagem_atual;

    if (p == -1)
        return;
    if (passagem_em(x, y) == p) {
        entrou_na_passagem = true;
        return;
    }
    if (entrou_na_passagem && equipe_saiu(p))
        soltar_passagem();
}

/* Tenta entrar na passagem p no sentido "sentido" (monitor de mão única +
 * semáforo, ver ocupacao.c). Nunca espera. Retorna true se conseguiu. */
static bool tentar_entrar(int p, int sentido)
{
    char texto[128];
    int k;

    if (!entrar_passagem(p, sentido)) {
        bloqueado = true;
        if (!avisou_espera) {
            evento("esperando passagem P%d (cheia ou na contramao)", p + 1);
            avisou_espera = true;
            clock_gettime(CLOCK_MONOTONIC, &espera_desde);
        }
        return false;
    }
    passagem_atual = p;
    entrou_na_passagem = false;
    sentido_atual = sentido;
    avisou_espera = false;
    for (k = 0; k < n_membros; k++)
        visto_dentro[k] = false;
    snprintf(texto, sizeof(texto), "entrou na passagem P%d (para a %s)", p + 1,
             sentido == 0 ? "direita" : "esquerda");
    registrar_evento(fila_coletor, id_robo, texto);
    return true;
}

/* Esperando a passagem p há mais de REPLANEJAR_APOS_S: recalcula o A* a
 * partir de (x, y). O custo dinâmico (3 por robô dentro de p, +30 se eles
 * andam na contramão) pode fazer o caminho novo usar outra passagem; se não
 * houver opção melhor, sai o mesmo caminho e o robô continua esperando (e
 * replaneja de novo depois). O texto do evento diz "cheia" nos dois casos. */
static void talvez_replanejar(int p, int x, int y)
{
    struct timespec agora;

    clock_gettime(CLOCK_MONOTONIC, &agora);
    if (agora.tv_sec - espera_desde.tv_sec < REPLANEJAR_APOS_S ||
        (agora.tv_sec - espera_desde.tv_sec == REPLANEJAR_APOS_S &&
         agora.tv_nsec < espera_desde.tv_nsec))
        return;
    evento("replanejou rota: passagem P%d cheia", p + 1);
    avisou_espera = false;   /* uma espera nova recomeça a contagem */
    planejar(x, y);
}

/* Desvio DENTRO da passagem que seguro: proíbe as células do túnel atrás
 * de mim (voltar seria andar na contramão de quem entrou depois). */
static void proibir_contramao(int x, int y, bool proibida[])
{
    const Passagem *ps;
    int cx;

    if (passagem_atual == -1 || passagem_em(x, y) != passagem_atual)
        return;
    ps = &mem->mapa.passagens[passagem_atual];
    for (cx = ps->x_entrada; cx <= ps->x_saida; cx++)
        if ((sentido_atual == 0 && cx < x) || (sentido_atual == 1 && cx > x))
            proibida[ps->y * MAPA_LARGURA + cx] = true;
}

/* Travamento: recalcula o A* de (x, y) até o fim do caminho tratando como
 * obstáculo as células de robôs de outros grupos (cópia local da grade;
 * o A* roda sem nenhum mutex). Sem caminho: continua esperando. */
static void desviar(int x, int y)
{
    bool proibida[N_CELULAS];
    Caminho novo;
    int xd = caminho.x[caminho.n - 1], yd = caminho.y[caminho.n - 1];

    copiar_ocupadas(meu_grupo, proibida);
    proibir_contramao(x, y, proibida);
    proibida[yd * MAPA_LARGURA + xd] = false;  /* o destino pode estar ocupado agora */
    if (!calcular_caminho_evitando(x, y, xd, yd, proibida, &novo))
        return;
    /* Peguei uma passagem mas ainda nem entrei: o caminho novo talvez não
     * passe por ela; solto e, se passar, peço de novo. */
    if (passagem_atual != -1 && !entrou_na_passagem)
        soltar_passagem();
    caminho = novo;
    indice = 0;
    publicar_rota();
    registrar_evento(fila_coletor, id_robo, "desviando de robo");
}

/* 4c) Segue o caminho: no máximo uma célula por ciclo. */
static void seguir_caminho(void)
{
    int x, y, p, nx, ny;

    ler_posicao(&x, &y);
    liberar_se_saiu(x, y);
    if (indice >= caminho.n)
        return; /* sem caminho */

    /* Chegou na célula do setpoint? Passa para a próxima. */
    if (x == caminho.x[indice] && y == caminho.y[indice]) {
        indice++;
        publicar_rota();              /* uma célula a menos */
        if (indice == caminho.n) {
            publicar_destino(-1, -1);
            definir_estado(estado_ao_chegar);
            registrar_evento(fila_coletor, id_robo, "chegou ao destino");
            return;
        }
    }

    /* A próxima célula é de uma passagem que ainda não seguro? */
    nx = caminho.x[indice];
    ny = caminho.y[indice];
    p = passagem_em(nx, ny);
    if (p != -1 && p != passagem_atual) {
        if (passagem_atual != -1) {
            bloqueado = true;
            dar_passo(cel_x, cel_y);
            return;   /* ainda seguro outra (equipe saindo): nunca duas */
        }
        if (!tentar_entrar(p, sentido_de_entrada(p, nx))) {
            dar_passo(cel_x, cel_y);   /* fica parado na porta */
            talvez_replanejar(p, x, y);
            return;
        }
    }
    /* Grade: reserva a próxima célula; ocupada por outro grupo = espera,
     * e se travar por muito tempo, desvia. */
    if (!dar_passo(nx, ny) && hora_de_desviar())
        desviar(x, y);
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
 * O caminho, a passagem e as células que ele segura NÃO são liberados: um
 * robô parado continua ocupando a vaga e a célula, porque fisicamente ele
 * está ali. Os outros esperam no semáforo ou desviam dele (grade). */
static void segurar_posicao(void)
{
    int x, y;

    parar_onde_esta(&x, &y);
}

/* Informa à Status se neste ciclo o robô esperou célula ou passagem (ela
 * publica no buffer; a tela mostra "aguardando"). */
static void publicar_bloqueado(void)
{
    pthread_mutex_lock(&estado_local.mutex);
    estado_local.bloqueado = bloqueado;
    pthread_mutex_unlock(&estado_local.mutex);
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
    abrir_fila();
    caminho.n = 0;
    /* A célula inicial já foi reservada pelo Gestor, com grupo = meu id. */
    meu_grupo = id_robo;
    ler_posicao(&cel_x, &cel_y);

    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        bloqueado = false;
        ler_mensagens();
        confirmar_chegada();
        verificar_acoplamento();
        if (deve_parar()) {
            segurar_posicao();
        } else {
            verificar_recarga();
            if (lider >= 0) {
                seguir_lider();
            } else {
                calcular_pendente();
                seguir_caminho();
            }
        }
        publicar_bloqueado();

        somar_ms(&proximo, PERIODO_NAVEGACAO_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }
    return NULL;
}
