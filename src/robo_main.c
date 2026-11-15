/*
 * robo_main.c — Processo de um robô (executável "robo <id>").
 *
 * Criado pelo Gestor com fork() + execl("./robo", "robo", "<id>").
 * Abre a memória compartilhada e a fila do Coletor, avisa que está vivo e
 * cria as threads do robô.
 *
 * Threads: principal, Comando de Navegação e Status do Robô.
 *   A principal só cria as duas threads e espera elas terminarem.
 * Sincronização:
 *   - semáforos das passagens: abertos aqui (abrir_semaforos_passagens)
 *     ANTES de criar as threads; a Navegação usa para entrar/sair e o A*
 *     (Navegação e Status) lê a ocupação. O vetor sem_passagem[] não muda
 *     depois disso, então não precisa de mutex.
 *   - mem->mapa: só leitura (gerado pelo processo central antes do fork).
 *   - grade de ocupação e monitores das passagens (anti-colisão): ficam na
 *     memória compartilhada, com mutex próprio; quem usa é a Navegação
 *     (ver ocupacao.c).
 *   - estado_local (struct deste processo, com mutex comum): é por ela que
 *     Navegação e Status trocam informação (ver comum.h).
 *   - mem->encerrar (atomic_bool): as threads terminam quando ele vira
 *     true; pthread_join espera isso antes de desmapear a memória.
 *   - fila /atr_coletor para enviar o evento "robô vivo".
 * O processo central também envia SIGTERM no encerramento; a ação padrão
 * do SIGTERM termina o processo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "comum.h"
#include "mapa.h"

/* Variáveis globais deste processo (declaradas em comum.h).
 * POR QUE sem mutex: mem, fila_coletor e id_robo são escritas uma vez no
 * main, ANTES de criar as duas threads, e depois só lidas. estado_local
 * tem mutex próprio (ver comum.h). */
MemoriaCompartilhada *mem;
mqd_t fila_coletor;
int id_robo;
/* mutex, estado, estado_antes, parar, ir_recarregar, recarga_x/y,
 * destino_x/y (sem rota), n_rota, os vetores da rota (vazios) e bloqueado. */
EstadoLocal estado_local = { PTHREAD_MUTEX_INITIALIZER, DISPONIVEL, DISPONIVEL,
                              false, false, 0, 0, -1, -1, 0, {0}, {0}, false };

int main(int argc, char *argv[])
{
    pthread_t th_status, th_navegacao;

    if (argc != 2) { fprintf(stderr, "uso: robo <id>\n"); exit(1); }
    id_robo = atoi(argv[1]);
    if (id_robo < 0 || id_robo >= MAX_ROBOS) {
        fprintf(stderr, "id invalido\n");
        exit(1);
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* imprime linha a linha, mesmo em pipe */

    mem = abrir_memoria();
    fila_coletor = mq_open(NOME_FILA_COLETOR, O_WRONLY);
    if (fila_coletor == (mqd_t)-1) { perror("mq_open coletor"); exit(1); }

    printf("Robo %d vivo (pid %d)\n", id_robo, (int)getpid());
    registrar_evento(fila_coletor, id_robo, "robo vivo");
    abrir_semaforos_passagens();

    if (pthread_create(&th_status, NULL, tarefa_status, NULL) != 0 ||
        pthread_create(&th_navegacao, NULL, tarefa_navegacao, NULL) != 0) {
        fprintf(stderr, "pthread_create falhou\n");
        exit(1);
    }
    pthread_join(th_navegacao, NULL);
    pthread_join(th_status, NULL);

    fechar_semaforos_passagens();
    mq_close(fila_coletor);
    fechar_memoria(mem);
    return 0;
}
