/*
 * ponte_mqtt.c — Ponte MQTT entre o núcleo em C e a Tela de Operação (Etapa 2).
 *
 * Threads:
 *   - tarefa_ponte_mqtt (nossa, no processo central), periódica com período
 *     PERIODO_PONTE_MS (200 ms): monta o JSON do status e publica em
 *     "atr/status".
 *   - thread de rede da libmosquitto, criada por mosquitto_loop_start DEPOIS
 *     da primeira conexão: envia e recebe os pacotes MQTT e reconecta
 *     sozinha se o broker cair. Os callbacks (ao_conectar, ao_desconectar)
 *     rodam NELA.
 *
 * Tópicos publicados (passo 9):
 *   atr/mapa    JSON do mapa, gerado a partir de mapa.h (fonte única da
 *               verdade). "Retained": o broker guarda a última cópia e
 *               entrega a quem assinar depois. Publicado a cada conexão.
 *   atr/status  JSON com robôs, caixas, passagens e contadores.
 *
 * O núcleo NÃO depende da tela: se o broker não estiver no ar, a ponte só
 * avisa e o resto do sistema continua igual (as publicações sem conexão
 * são descartadas). A PRIMEIRA conexão é tentada por esta thread, a cada
 * 2 s (a thread de rede da biblioteca só reconecta depois de uma conexão
 * que já deu certo); depois disso, as quedas são tratadas pela biblioteca.
 *
 * Sincronização:
 *   - mutex de cada robô: copia os campos do BUFFER DE STATUS (nunca o hw)
 *     para variáveis locais e só monta o texto DEPOIS de soltar o mutex
 *     (montar texto é lento; não vale segurar o robô parado por isso).
 *   - mem->mutex_caixas: copia o vetor de caixas de uma vez, pelo mesmo motivo.
 *   - sem_getvalue nos semáforos das passagens: é só uma leitura instantânea
 *     para EXIBIÇÃO (o valor pode mudar logo depois); não serve para decidir
 *     nada, e por isso não precisa de proteção extra.
 *   - libmosquitto: mosquitto_publish pode ser chamado desta thread enquanto
 *     a thread de rede da biblioteca roda; a biblioteca protege as filas
 *     internas dela com mutexes próprios.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <semaphore.h>
#include <mosquitto.h>
#include "comum.h"
#include "mapa.h"

#define BROKER_ENDERECO "localhost"
#define BROKER_PORTA    1883
#define MANTER_VIVO_S   10      /* keepalive do MQTT, em segundos */
#define TAMANHO_JSON    16384   /* cabe 16 robôs + 64 caixas com folga */
#define CICLOS_ENTRE_TENTATIVAS 10  /* 10 x 200 ms = tenta conectar a cada 2 s */

/* Texto JSON sendo montado aos pedaços. */
typedef struct {
    char texto[TAMANHO_JSON];
    int  usado;
} Json;

/* Acrescenta um pedaço formatado (como printf) ao fim do JSON. Se não
 * couber, "usado" passa do tamanho e o JSON é descartado em publicar(). */
static void json_add(Json *j, const char *formato, ...)
{
    va_list args;
    int n;

    if (j->usado >= TAMANHO_JSON)
        return;
    va_start(args, formato);
    n = vsnprintf(j->texto + j->usado, TAMANHO_JSON - j->usado, formato, args);
    va_end(args);
    if (n > 0)
        j->usado += n;
}

static void publicar(struct mosquitto *mosq, const char *topico, Json *j, bool reter)
{
    if (j->usado >= TAMANHO_JSON) {
        fprintf(stderr, "Ponte MQTT: JSON de %s grande demais, descartado\n", topico);
        return;
    }
    /* Sem conexão retorna erro; ignoramos (o núcleo não depende da tela). */
    mosquitto_publish(mosq, NULL, topico, j->usado, j->texto, 0, reter);
}

/* ---------- atr/mapa ---------- */

/* Um retângulo de passagem: {"x1":..,"y1":..,"x2":..,"y2":..}. */
static void json_retangulo(Json *j, int x1, int y1, int x2, int y2, bool virgula)
{
    json_add(j, "{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}%s",
             x1, y1, x2, y2, virgula ? "," : "");
}

/* Monta o JSON do mapa só com as constantes de mapa.h. As passagens são
 * descritas como o código as trata (dentro_de_passagem): P1 e P2 são
 * colunas inteiras, P3 são dois trechos da coluna x = 45. */
static void publicar_mapa(struct mosquitto *mosq)
{
    Json j;
    int i;

    j.usado = 0;
    json_add(&j, "{\"largura\":%d,\"altura\":%d,\"corredor_y\":%d,\"baias\":[",
             MAPA_LARGURA, MAPA_ALTURA, CORREDOR_Y);
    for (i = 0; i < N_BAIAS; i++)
        json_add(&j, "{\"id\":%d,\"x\":%d,\"y\":%d}%s", i + 1, BAIA_X[i], BAIA_Y[i],
                 i < N_BAIAS - 1 ? "," : "");
    json_add(&j, "],\"zonas\":[");
    for (i = 0; i < N_ZONAS; i++)
        json_add(&j, "{\"id\":\"%c\",\"x\":%d,\"y\":%d}%s", 'A' + i, ZONA_X[i], ZONA_Y[i],
                 i < N_ZONAS - 1 ? "," : "");
    json_add(&j, "],\"estacoes\":[");
    for (i = 0; i < N_ESTACOES; i++)
        json_add(&j, "{\"x\":%d,\"y\":%d}%s", ESTACAO_X[i], ESTACAO_Y[i],
                 i < N_ESTACOES - 1 ? "," : "");

    json_add(&j, "],\"passagens\":[");
    json_add(&j, "{\"nome\":\"P1\",\"capacidade\":%d,\"retangulos\":[", CAPACIDADE_PASSAGEM[P1]);
    json_retangulo(&j, P1_X_MIN, 0, P1_X_MAX, MAPA_ALTURA - 1, false);
    json_add(&j, "]},{\"nome\":\"P2\",\"capacidade\":%d,\"retangulos\":[", CAPACIDADE_PASSAGEM[P2]);
    json_retangulo(&j, P2_X_MIN, 0, P2_X_MAX, MAPA_ALTURA - 1, false);
    json_add(&j, "]},{\"nome\":\"P3\",\"capacidade\":%d,\"retangulos\":[", CAPACIDADE_PASSAGEM[P3]);
    json_retangulo(&j, P3_X, P3_CIMA_Y_MIN, P3_X, P3_CIMA_Y_MAX, true);
    json_retangulo(&j, P3_X, P3_BAIXO_Y_MIN, P3_X, P3_BAIXO_Y_MAX, false);
    json_add(&j, "]}]}");

    publicar(mosq, "atr/mapa", &j, true);  /* retained */
}

/* ---------- atr/status ---------- */

static const char *nome_status_caixa(int status)
{
    if (status == CAIXA_ESPERANDO)    return "ESPERANDO";
    if (status == CAIXA_EM_ANDAMENTO) return "EM_ANDAMENTO";
    return "ENTREGUE";
}

/* Robôs ativos, lidos SÓ do buffer de status. Retorna quantos são. */
static int json_robos(Json *j)
{
    Robo r;   /* cópia local do slot */
    int i, n = 0;

    json_add(j, "\"robos\":[");
    for (i = 0; i < MAX_ROBOS; i++) {
        pthread_mutex_lock(&mem->robos[i].mutex);
        r = mem->robos[i];
        pthread_mutex_unlock(&mem->robos[i].mutex);
        if (!r.ativo)
            continue;
        /* Daqui para baixo só a cópia (r) é usada, já fora do mutex. */
        json_add(j, "%s{\"id\":%d,\"estado\":\"%s\",\"modo\":\"%s\",\"x\":%d,\"y\":%d,"
                 "\"sp_x\":%d,\"sp_y\":%d,\"bateria\":%d,\"distancia\":%.0f,\"carga\":%d}",
                 n > 0 ? "," : "", i, nome_estado(r.e_estado),
                 r.modo_automatico ? "auto" : "manual", r.e_posicao_x, r.e_posicao_y,
                 r.j_sp_posicao_x, r.j_sp_posicao_y, r.bateria, r.distancia_total,
                 r.carga_id);
        n++;
    }
    json_add(j, "]");
    return n;
}

/* Caixas (todas, em ordem de chegada) e os contadores delas. */
static void json_caixas(Json *j, int *pendentes, int *andamento, int *entregues)
{
    Caixa caixas[MAX_CAIXAS];
    int i, n;

    pthread_mutex_lock(&mem->mutex_caixas);
    n = mem->n_caixas;
    memcpy(caixas, mem->caixas, n * sizeof(Caixa));
    pthread_mutex_unlock(&mem->mutex_caixas);

    *pendentes = *andamento = *entregues = 0;
    json_add(j, ",\"caixas\":[");
    for (i = 0; i < n; i++) {
        json_add(j, "%s{\"id\":%d,\"tamanho\":%d,\"baia\":%d,\"zona\":\"%c\",\"status\":\"%s\"}",
                 i > 0 ? "," : "", caixas[i].id, caixas[i].tamanho, caixas[i].baia,
                 'A' + caixas[i].zona, nome_status_caixa(caixas[i].status));
        if (caixas[i].status == CAIXA_ESPERANDO)         (*pendentes)++;
        else if (caixas[i].status == CAIXA_EM_ANDAMENTO) (*andamento)++;
        else                                             (*entregues)++;
    }
    json_add(j, "]");
}

/* Ocupação das passagens: livres = valor atual do semáforo. */
static void json_passagens(Json *j, sem_t *semaforos[])
{
    int i, livres;

    json_add(j, ",\"passagens\":[");
    for (i = 0; i < N_PASSAGENS; i++) {
        /* Leitura instantânea, só para exibir (ver comentário do topo). */
        if (sem_getvalue(semaforos[i], &livres) == -1)
            livres = -1;
        json_add(j, "%s{\"nome\":\"P%d\",\"capacidade\":%d,\"livres\":%d}",
                 i > 0 ? "," : "", i + 1, CAPACIDADE_PASSAGEM[i], livres);
    }
    json_add(j, "]");
}

static void publicar_status(struct mosquitto *mosq, sem_t *semaforos[])
{
    Json j;
    struct timespec agora;
    char data_hora[48];
    int ativos, pendentes, andamento, entregues;

    clock_gettime(CLOCK_REALTIME, &agora);
    formatar_data_hora(agora, data_hora, sizeof(data_hora));

    j.usado = 0;
    json_add(&j, "{\"t\":\"%s\",", data_hora);
    ativos = json_robos(&j);
    json_caixas(&j, &pendentes, &andamento, &entregues);
    json_passagens(&j, semaforos);
    json_add(&j, ",\"contadores\":{\"robos_ativos\":%d,\"entregues\":%d,"
             "\"pendentes\":%d,\"em_andamento\":%d}}",
             ativos, entregues, pendentes, andamento);

    publicar(mosq, "atr/status", &j, false);
}

/* ---------- Conexão ---------- */

/* Callback (thread de rede da biblioteca): conexão aceita ou recusada. */
static void ao_conectar(struct mosquitto *mosq, void *dados, int codigo)
{
    (void)dados;
    if (codigo != 0) {
        printf("Ponte MQTT: conexao recusada (%s)\n", mosquitto_connack_string(codigo));
        return;
    }
    printf("Ponte MQTT: conectada ao broker %s:%d\n", BROKER_ENDERECO, BROKER_PORTA);
    publicar_mapa(mosq); /* a cada (re)conexão */
}

/* Callback (thread de rede da biblioteca): conexão perdida. */
static void ao_desconectar(struct mosquitto *mosq, void *dados, int codigo)
{
    (void)mosq;
    (void)dados;
    if (codigo != 0) /* 0 = nós pedimos para desconectar (encerramento) */
        printf("Ponte MQTT: conexao perdida; tentando reconectar\n");
}

/* Cria o cliente e liga os callbacks (ainda sem conectar). */
static struct mosquitto *criar_cliente(void)
{
    struct mosquitto *mosq;

    mosquitto_lib_init();
    mosq = mosquitto_new("atr_logistica", true, NULL);
    if (mosq == NULL) { perror("mosquitto_new"); exit(1); }
    mosquitto_connect_callback_set(mosq, ao_conectar);
    mosquitto_disconnect_callback_set(mosq, ao_desconectar);
    mosquitto_reconnect_delay_set(mosq, 1, 10, true); /* 1 s, 2 s, 4 s... até 10 s */
    return mosq;
}

/* Tenta a primeira conexão. Se der certo, inicia a thread de rede da
 * biblioteca (que daí em diante cuida das reconexões) e retorna true. */
static bool tentar_conectar(struct mosquitto *mosq, bool *avisou)
{
    if (mosquitto_connect(mosq, BROKER_ENDERECO, BROKER_PORTA, MANTER_VIVO_S)
            != MOSQ_ERR_SUCCESS) {
        if (!*avisou) {
            printf("Ponte MQTT: broker fora do ar; o sistema segue e a ponte tenta de novo a cada 2 s\n");
            *avisou = true;
        }
        return false;
    }
    if (mosquitto_loop_start(mosq) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mosquitto_loop_start falhou\n");
        exit(1);
    }
    return true;
}

static void abrir_semaforos(sem_t *semaforos[])
{
    char nome[32];
    int i;

    for (i = 0; i < N_PASSAGENS; i++) {
        snprintf(nome, sizeof(nome), "%s%d", PREFIXO_SEM_PASSAGEM, i + 1);
        semaforos[i] = sem_open(nome, 0);
        if (semaforos[i] == SEM_FAILED) { perror("sem_open ponte"); exit(1); }
    }
}

void *tarefa_ponte_mqtt(void *arg)
{
    struct mosquitto *mosq;
    sem_t *semaforos[N_PASSAGENS];
    struct timespec proximo;
    bool rede_iniciada = false, avisou = false;
    int i, ciclo = 0;

    (void)arg;
    abrir_semaforos(semaforos);
    mosq = criar_cliente();

    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        if (!rede_iniciada && ciclo % CICLOS_ENTRE_TENTATIVAS == 0)
            rede_iniciada = tentar_conectar(mosq, &avisou);
        if (rede_iniciada)
            publicar_status(mosq, semaforos);
        ciclo++;
        somar_ms(&proximo, PERIODO_PONTE_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }

    if (rede_iniciada) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false); /* espera a thread de rede terminar */
    }
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    for (i = 0; i < N_PASSAGENS; i++)
        sem_close(semaforos[i]);
    return NULL;
}
