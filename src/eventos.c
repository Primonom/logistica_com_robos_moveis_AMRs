/*
 * eventos.c — Funções de ENVIO para o Coletor de Dados.
 *
 * Não cria threads. registrar_evento, registrar_carga e pedir_fim_coletor
 * são chamadas por QUALQUER thread de QUALQUER processo (central ou robo):
 * cada uma monta uma mensagem com o horário do evento e a coloca na fila
 * /atr_coletor. Quem consome a fila é a thread Coletor (coletor.c, só no
 * processo central). Ficam separadas do coletor.c porque o processo robo
 * só precisa enviar (e não deve depender da ponte MQTT, que o Coletor usa).
 *
 * Sincronização: a fila de mensagens POSIX /atr_coletor (produtor-
 * consumidor). mq_send é atômico (duas mensagens nunca se misturam) e
 * bloqueia quando a fila está cheia, até o Coletor consumir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comum.h"

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
