/*
 * main.c — Processo central (logistica).
 *
 * Cria os objetos de IPC (memória compartilhada /atr_shm, fila
 * /atr_coletor e semáforos das passagens /atr_passagem_<n>), cria as
 * threads e faz o encerramento limpo.
 *
 * Uso: ./logistica [arquivo_de_caixas]   (padrão: config/caixas.txt)
 *
 * Threads: principal, Coletor, Gestor, Organizador e Simulador-stub.
 *   A thread principal roda o Console (laço de comandos do stdin). Quando
 *   chega "sair" ou o fim da entrada, ela faz o encerramento limpo.
 *   Ctrl+C usa o comportamento padrão do SIGINT: mata o processo central e
 *   os robôs (mesmo grupo do terminal) sem limpeza; as sobras de IPC são
 *   removidas na próxima inicialização (unlink dos nomes antigos).
 *
 * Sincronização:
 *   - Semáforos contadores nomeados /atr_passagem_<n>, com valor inicial =
 *     capacidade da passagem: criados aqui, usados pela Navegação dos robôs
 *     (ver navegacao.c).
 *   - mem->encerrar (atomic_bool): avisa todas as tarefas para pararem.
 *   - pthread_join / waitpid: espera threads e processos terminarem antes
 *     de destruir os recursos que eles usam.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/wait.h>
#include "comum.h"
#include "mapa.h"

/* Variáveis globais deste processo (declaradas em comum.h). */
MemoriaCompartilhada *mem;
mqd_t fila_coletor;

/* Cria a fila do Coletor. Usada por todos os processos para enviar eventos. */
static mqd_t criar_fila_coletor(void)
{
    struct mq_attr atributos = {0};
    mqd_t fila;

    atributos.mq_maxmsg  = MAX_MSG_FILA;
    atributos.mq_msgsize = sizeof(MensagemColetor);

    mq_unlink(NOME_FILA_COLETOR); /* remove sobra de execução anterior */
    fila = mq_open(NOME_FILA_COLETOR, O_CREAT | O_EXCL | O_RDWR, 0600,
                   &atributos);
    if (fila == (mqd_t)-1) { perror("mq_open coletor"); exit(1); }
    return fila;
}

/* Cria um semáforo nomeado por passagem, com valor = capacidade.
 * Fecha logo em seguida: o processo central não usa; o nome continua
 * existindo para os robôs abrirem. */
static void criar_semaforos(void)
{
    char nome[32];
    sem_t *sem;
    int i;

    for (i = 0; i < N_PASSAGENS; i++) {
        snprintf(nome, sizeof(nome), "%s%d", PREFIXO_SEM_PASSAGEM, i + 1);
        sem_unlink(nome); /* remove sobra de execução anterior */
        sem = sem_open(nome, O_CREAT | O_EXCL, 0600, CAPACIDADE_PASSAGEM[i]);
        if (sem == SEM_FAILED) { perror("sem_open"); exit(1); }
        sem_close(sem);
    }
}

static void remover_semaforos(void)
{
    char nome[32];
    int i;

    for (i = 0; i < N_PASSAGENS; i++) {
        snprintf(nome, sizeof(nome), "%s%d", PREFIXO_SEM_PASSAGEM, i + 1);
        if (sem_unlink(nome) == -1) perror("sem_unlink");
    }
}

static void criar_thread(pthread_t *th, void *(*funcao)(void *), void *arg,
                         const char *nome)
{
    if (pthread_create(th, NULL, funcao, arg) != 0) {
        fprintf(stderr, "pthread_create %s falhou\n", nome);
        exit(1);
    }
}

/* Envia SIGTERM a cada robô ativo, espera ele terminar e apaga sua fila. */
static void encerrar_robos(void)
{
    char nome[32];
    pid_t pid;
    int i;

    for (i = 0; i < MAX_ROBOS; i++) {
        pthread_mutex_lock(&mem->robos[i].mutex);
        pid = mem->robos[i].ativo ? mem->robos[i].pid : 0;
        mem->robos[i].ativo = false;
        pthread_mutex_unlock(&mem->robos[i].mutex);

        if (pid > 0) {
            kill(pid, SIGTERM);
            if (waitpid(pid, NULL, 0) == -1) perror("waitpid");
        }
        snprintf(nome, sizeof(nome), "%s%d", PREFIXO_FILA_ROBO, i);
        mq_unlink(nome); /* pode não existir: erro ignorado */
    }
}

int main(int argc, char *argv[])
{
    pthread_t th_coletor, th_gestor, th_simulador, th_organizador;
    char *arquivo_caixas = (argc > 1) ? argv[1] : "config/caixas.txt";

    setvbuf(stdout, NULL, _IOLBF, 0); /* imprime linha a linha, mesmo em pipe */

    mem = criar_memoria();
    fila_coletor = criar_fila_coletor();
    criar_semaforos();

    criar_thread(&th_coletor, tarefa_coletor, NULL, "coletor");
    registrar_evento(fila_coletor, -1, "sistema iniciado");
    criar_thread(&th_gestor, tarefa_gestor, NULL, "gestor");
    criar_thread(&th_simulador, tarefa_simulador, NULL, "simulador");
    criar_thread(&th_organizador, tarefa_organizador, arquivo_caixas, "organizador");

    /* Console na thread principal: só retorna em "sair" ou fim da entrada. */
    executar_console();
    printf("Encerrando...\n");

    /* 1. Avisa todos e espera o Gestor (para não criar robô durante o fim),
     *    o Organizador e o Simulador (percebem encerrar no próximo ciclo). */
    atomic_store(&mem->encerrar, true);
    acordar_gestor();
    pthread_join(th_gestor, NULL);
    pthread_join(th_organizador, NULL);
    pthread_join(th_simulador, NULL);

    /* 2. Encerra os processos robo. */
    encerrar_robos();

    /* 3. Coletor grava o que falta e termina. */
    registrar_evento(fila_coletor, -1, "sistema encerrado");
    pedir_fim_coletor(fila_coletor);
    pthread_join(th_coletor, NULL);

    /* 4. Remove os objetos de IPC. */
    mq_close(fila_coletor);
    if (mq_unlink(NOME_FILA_COLETOR) == -1) perror("mq_unlink coletor");
    remover_semaforos();
    destruir_memoria(mem);
    printf("Fim.\n");
    return 0;
}
