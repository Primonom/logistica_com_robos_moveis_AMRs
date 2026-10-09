/*
 * coletor.c — Tarefa "Coletor de Dados".
 *
 * Thread: tarefa_coletor (uma só, no processo central).
 *   Fica bloqueada em mq_receive na fila /atr_coletor. Para cada mensagem,
 *   grava uma linha com data e hora em logs/eventos.csv ou logs/cargas.csv
 *   e, depois de gravar, publica o registro no tópico MQTT "atr/eventos"
 *   (para a Tela de Operação; se o broker não estiver no ar, a publicação
 *   é simplesmente descartada).
 *
 * As funções de ENVIO (registrar_evento, registrar_carga, pedir_fim_coletor)
 * ficam em eventos.c, porque são usadas pelos dois processos (central e
 * robo); este arquivo é só do processo central.
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

/* Copia "texto" para "saida" escapando aspas e barras invertidas, para o
 * texto poder ir dentro de uma string JSON. */
static void escapar_json(const char *texto, char *saida, int tamanho)
{
    int i, j = 0;

    for (i = 0; texto[i] != '\0' && j < tamanho - 2; i++) {
        if (texto[i] == '"' || texto[i] == '\\')
            saida[j++] = '\\';
        saida[j++] = texto[i];
    }
    saida[j] = '\0';
}

/* Publica o registro em atr/eventos:
 * {"data_hora":"...","robo":N,"tipo":"evento"|"carga","texto":"..."} */
static void publicar_evento(const char *data_hora, const MensagemColetor *msg)
{
    char texto[2 * sizeof(msg->texto)];
    char json[512];

    escapar_json(msg->texto, texto, sizeof(texto));
    snprintf(json, sizeof(json),
             "{\"data_hora\":\"%s\",\"robo\":%d,\"tipo\":\"%s\",\"texto\":\"%s\"}",
             data_hora, msg->robo_id, msg->tipo == COLETOR_CARGA ? "carga" : "evento", texto);
    ponte_publicar("atr/eventos", json);
}

/* Converte o timestamp em texto "AAAA-MM-DD HH:MM:SS.mmm".
 * Também usada pela ponte MQTT (campo "t" do atr/status). */
void formatar_data_hora(struct timespec quando, char *saida, int tamanho)
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
        publicar_evento(data_hora, &msg); /* depois de gravar */
    }

    fclose(eventos);
    fclose(cargas);
    return NULL;
}
