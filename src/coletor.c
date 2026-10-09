/*
 * coletor.c — Tarefa "Coletor de Dados".
 *
 * Thread: tarefa_coletor (uma só, no processo central).
 *   Fica bloqueada em mq_receive na fila /atr_coletor. Para cada mensagem,
 *   grava uma linha com data e hora em logs/eventos.csv ou logs/cargas.csv.
 *
 * Funções de envio (registrar_evento, registrar_carga, pedir_fim_coletor):
 *   chamadas por QUALQUER thread de QUALQUER processo (central ou robo).
 *
 * Sincronização: a fila de mensagens POSIX /atr_coletor.
 *   - É um problema produtor-consumidor: muitos produtores (as tarefas que
 *     geram eventos) e um consumidor (o Coletor).
 *   - A própria fila resolve o problema: mq_send/mq_receive são atômicos
 *     (duas mensagens nunca se misturam), mq_receive bloqueia quando a fila
 *     está vazia e mq_send bloqueia quando ela está cheia.
 *   - Só a thread Coletor escreve nos arquivos, então os arquivos não
 *     precisam de mutex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "comum.h"

/* Converte o timestamp em texto "AAAA-MM-DD HH:MM:SS.mmm". */
static void formatar_data_hora(struct timespec quando, char *saida, int tamanho)
{
    struct tm data;
    char sem_ms[32];

    localtime_r(&quando.tv_sec, &data);
    strftime(sem_ms, sizeof(sem_ms), "%Y-%m-%d %H:%M:%S", &data);
    snprintf(saida, tamanho, "%s.%03ld", sem_ms, quando.tv_nsec / 1000000);
}

/* Abre um arquivo de log para acrescentar linhas.
 * Se o arquivo estiver vazio (novo), escreve o cabeçalho. */
static FILE *abrir_log(const char *caminho, const char *cabecalho)
{
    FILE *f = fopen(caminho, "a");
    if (f == NULL) { perror(caminho); exit(1); }
    if (ftell(f) == 0) {
        fprintf(f, "%s\n", cabecalho);
        fflush(f);
    }
    return f;
}

/* Thread Coletor: consome a fila e grava em disco. */
void *tarefa_coletor(void *arg)
{
    MensagemColetor msg;
    FILE *eventos, *cargas;
    char data_hora[48];
    ssize_t ret;

    (void)arg; /* não usado: a fila é a variável global fila_coletor */

    /* Pasta relativa ao diretório de execução; "já existe" não é erro. */
    if (mkdir("logs", 0755) == -1 && errno != EEXIST) {
        perror("mkdir logs");
        exit(1);
    }
    eventos = abrir_log("logs/eventos.csv", "data_hora;robo;evento");
    cargas  = abrir_log("logs/cargas.csv", "data_hora;caixa;baia;zona;robos");

    while (1) {
        /* Bloqueia até chegar uma mensagem (não gasta CPU esperando). */
        ret = mq_receive(fila_coletor, (char *)&msg, sizeof(msg), NULL);
        if (ret == -1) { perror("mq_receive coletor"); exit(1); }

        if (msg.tipo == COLETOR_FIM)
            break;

        formatar_data_hora(msg.quando, data_hora, sizeof(data_hora));
        if (msg.tipo == COLETOR_CARGA) {
            fprintf(cargas, "%s;%s\n", data_hora, msg.texto);
            fflush(cargas);
        } else {
            fprintf(eventos, "%s;%d;%s\n", data_hora, msg.robo_id, msg.texto);
            fflush(eventos); /* grava já: não perde o log se o programa cair */
        }
    }

    fclose(eventos);
    fclose(cargas);
    return NULL;
}

/* Monta a mensagem (com o horário de AGORA) e coloca na fila. */
static void enviar_coletor(mqd_t fila, int tipo, int robo_id, const char *texto)
{
    MensagemColetor msg;

    memset(&msg, 0, sizeof(msg));
    msg.tipo = tipo;
    clock_gettime(CLOCK_REALTIME, &msg.quando); /* hora real do evento */
    msg.robo_id = robo_id;
    strncpy(msg.texto, texto, sizeof(msg.texto) - 1);

    if (mq_send(fila, (const char *)&msg, sizeof(msg), 0) == -1) {
        perror("mq_send coletor");
        exit(1);
    }
}

/* Evento de um robô (ou do sistema, com robo_id = -1). */
void registrar_evento(mqd_t fila, int robo_id, const char *texto)
{
    enviar_coletor(fila, COLETOR_EVENTO, robo_id, texto);
}

/* Carga entregue. texto = "caixa;baia;zona;robos". */
void registrar_carga(mqd_t fila, const char *texto)
{
    enviar_coletor(fila, COLETOR_CARGA, -1, texto);
}

/* Pede para a thread Coletor terminar. Como a fila é FIFO, ela ainda
 * grava todas as mensagens enviadas antes desta. */
void pedir_fim_coletor(mqd_t fila)
{
    enviar_coletor(fila, COLETOR_FIM, -1, "");
}
