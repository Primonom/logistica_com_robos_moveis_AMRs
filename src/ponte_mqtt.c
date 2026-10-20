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
 * Tópicos:
 *   atr/mapa    (publica) JSON do mapa, gerado a partir de mapa.h (fonte
 *               única da verdade). "Retained": o broker guarda a última
 *               cópia e entrega a quem assinar depois. Publicado a cada
 *               conexão.
 *   atr/status  (publica) JSON com robôs, caixas, passagens, a caixa que o
 *               Organizador está esperando (e quantos robôs livres havia) e
 *               contadores.
 *   atr/comando (assina) uma linha de comando igual à do Console; é
 *               executada por executar_comando() (console.c) na thread de
 *               rede da biblioteca, e o resultado vai para
 *   atr/comando/resposta (publica) "ok", a saída do comando, ou
 *               "erro: <mensagem>".
 *   atr/eventos (publicado pelo Coletor, via ponte_publicar()).
 *
 * Ciclo de vida do cliente MQTT (um só, compartilhado):
 *   ponte_criar()    main, ANTES de criar as threads
 *   tarefa_ponte_mqtt  conecta e publica o status
 *   ponte_destruir() main, DEPOIS de todas as threads terminarem (o Coletor
 *                    publica até o último evento, "sistema encerrado").
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
 *   - libmosquitto: mosquitto_publish pode ser chamado desta thread, do
 *     Coletor e da thread de rede ao mesmo tempo; a biblioteca protege as
 *     filas internas dela com mutexes próprios.
 *   - "cliente" e "rede_iniciada" são globais deste arquivo, mas sem mutex:
 *     "cliente" é escrito só em ponte_criar(), antes de existirem as outras
 *     threads (pthread_create garante que elas veem o valor), e só é
 *     destruído depois de todas terminarem; "rede_iniciada" é escrito só
 *     pela thread da ponte e lido pelo main depois do pthread_join dela.
 *   - Comandos recebidos: executar_comando() é segura para ser chamada ao
 *     mesmo tempo pelo Console e por aqui (ver console.c).
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

#define BROKER_PORTA    1883
#define MANTER_VIVO_S   10      /* keepalive do MQTT, em segundos */
#define TAMANHO_JSON    16384   /* cabe 16 robôs + 64 caixas com folga */
#define CICLOS_ENTRE_TENTATIVAS 10  /* 10 x 200 ms = tenta conectar a cada 2 s */

static struct mosquitto *cliente = NULL;  /* ver "Sincronização" no topo */
static bool rede_iniciada = false;
static const char *broker_endereco = "localhost";

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

/* Líder da equipe que leva a caixa "carga_id" (-1 se não há). */
static int lider_da_carga(int carga_id, const Caixa caixas[], int n_caixas)
{
    int i;

    for (i = 0; i < n_caixas; i++)
        if (caixas[i].id == carga_id)
            return caixas[i].lider;
    return -1;
}

/* Termina o robô com ,"rota":[[x,y],...]} (próximos waypoints). */
static void json_rota(Json *j, const Robo *r)
{
    int k;

    json_add(j, ",\"rota\":[");
    for (k = 0; k < r->n_rota; k++)
        json_add(j, "%s[%d,%d]", k > 0 ? "," : "", r->rota_x[k], r->rota_y[k]);
    json_add(j, "]}");
}

/* Robôs ativos, lidos SÓ do buffer de status. Retorna quantos são.
 * "caixas" é a cópia feita antes, usada só para achar o líder. */
static int json_robos(Json *j, const Caixa caixas[], int n_caixas)
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
                 "\"sp_x\":%d,\"sp_y\":%d,\"destino_x\":%d,\"destino_y\":%d,"
                 "\"bateria\":%d,\"distancia\":%.0f,\"carga\":%d,\"lider\":%d",
                 n > 0 ? "," : "", i, nome_estado(r.e_estado),
                 r.modo_automatico ? "auto" : "manual", r.e_posicao_x, r.e_posicao_y,
                 r.j_sp_posicao_x, r.j_sp_posicao_y, r.destino_x, r.destino_y,
                 r.bateria, r.distancia_total, r.carga_id,
                 r.carga_id == -1 ? -1 : lider_da_carga(r.carga_id, caixas, n_caixas));
        json_rota(j, &r);
        n++;
    }
    json_add(j, "]");
    return n;
}

/* Copia a fila de caixas (e quem está esperando equipe) com o mutex; o
 * resto usa só a cópia. */
static int copiar_caixas(Caixa caixas[], int *em_espera, int *livres)
{
    int n;

    pthread_mutex_lock(&mem->mutex_caixas);
    n = mem->n_caixas;
    memcpy(caixas, mem->caixas, n * sizeof(Caixa));
    *em_espera = mem->caixa_em_espera;
    *livres = mem->robos_livres_espera;
    pthread_mutex_unlock(&mem->mutex_caixas);
    return n;
}

/* Caixas (todas, em ordem de chegada), com a equipe, e os contadores. */
static void json_caixas(Json *j, const Caixa caixas[], int n,
                        int *pendentes, int *andamento, int *entregues)
{
    int i, k;

    *pendentes = *andamento = *entregues = 0;
    json_add(j, ",\"caixas\":[");
    for (i = 0; i < n; i++) {
        json_add(j, "%s{\"id\":%d,\"tamanho\":%d,\"baia\":%d,\"zona\":\"%c\","
                 "\"status\":\"%s\",\"robos\":[",
                 i > 0 ? "," : "", caixas[i].id, caixas[i].tamanho, caixas[i].baia,
                 'A' + caixas[i].zona, nome_status_caixa(caixas[i].status));
        for (k = 0; k < caixas[i].n_robos; k++)
            json_add(j, "%s%d", k > 0 ? "," : "", caixas[i].robos[k]);
        json_add(j, "],\"lider\":%d}", caixas[i].lider);
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
    Caixa caixas[MAX_CAIXAS];
    struct timespec agora;
    char data_hora[48];
    int n_caixas, ativos, pendentes, andamento, entregues, em_espera, livres;

    clock_gettime(CLOCK_REALTIME, &agora);
    formatar_data_hora(agora, data_hora, sizeof(data_hora));

    j.usado = 0;
    json_add(&j, "{\"t\":\"%s\",", data_hora);
    n_caixas = copiar_caixas(caixas, &em_espera, &livres);
    ativos = json_robos(&j, caixas, n_caixas);
    json_caixas(&j, caixas, n_caixas, &pendentes, &andamento, &entregues);
    json_passagens(&j, semaforos);
    json_add(&j, ",\"organizador\":{\"caixa_em_espera\":%d,\"robos_livres\":%d}",
             em_espera, livres);
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
    printf("Ponte MQTT: conectada ao broker %s:%d\n", broker_endereco, BROKER_PORTA);
    /* A cada (re)conexão: assina de novo (o broker esquece as assinaturas
     * de um cliente "clean session") e republica o mapa. */
    mosquitto_subscribe(mosq, NULL, "atr/comando", 0);
    publicar_mapa(mosq);
}

/* Callback (thread de rede da biblioteca): chegou uma mensagem. A única
 * assinatura é atr/comando: executa a linha como se fosse o Console e
 * publica a resposta. */
static void ao_receber(struct mosquitto *mosq, void *dados,
                       const struct mosquitto_message *msg)
{
    char linha[128];
    char resp[TAMANHO_RESPOSTA];
    char texto[TAMANHO_RESPOSTA + 8];
    int n;
    bool ok;

    (void)dados;
    if (strcmp(msg->topic, "atr/comando") != 0)
        return;
    /* O payload não termina em '\0': copia com limite e termina. */
    n = msg->payloadlen < (int)sizeof(linha) - 1 ? msg->payloadlen : (int)sizeof(linha) - 1;
    memcpy(linha, msg->payload, n);
    linha[n] = '\0';

    if (atomic_load(&mem->encerrar)) {
        snprintf(texto, sizeof(texto), "erro: sistema encerrando");
    } else {
        ok = executar_comando(linha, resp, sizeof(resp));
        snprintf(texto, sizeof(texto), "%s%s", ok ? "" : "erro: ", resp);
        printf("Ponte MQTT: comando \"%s\" -> %s\n", linha, ok ? "ok" : resp);
    }
    mosquitto_publish(mosq, NULL, "atr/comando/resposta", strlen(texto), texto, 0, false);
}

/* Callback (thread de rede da biblioteca): conexão perdida. */
static void ao_desconectar(struct mosquitto *mosq, void *dados, int codigo)
{
    (void)mosq;
    (void)dados;
    if (codigo != 0) /* 0 = nós pedimos para desconectar (encerramento) */
        printf("Ponte MQTT: conexao perdida; tentando reconectar\n");
}

/* main, ANTES de criar as threads: cria o cliente e liga os callbacks
 * (ainda sem conectar; quem conecta é a thread da ponte). */
void ponte_criar(void)
{
    const char *endereco_configurado = getenv("ATR_MQTT_HOST");

    if (endereco_configurado != NULL && endereco_configurado[0] != '\0')
        broker_endereco = endereco_configurado;
    mosquitto_lib_init();
    cliente = mosquitto_new("atr_logistica", true, NULL);
    if (cliente == NULL) { perror("mosquitto_new"); exit(1); }
    mosquitto_connect_callback_set(cliente, ao_conectar);
    mosquitto_disconnect_callback_set(cliente, ao_desconectar);
    mosquitto_message_callback_set(cliente, ao_receber);
    mosquitto_reconnect_delay_set(cliente, 1, 10, true); /* 1 s, 2 s, 4 s... até 10 s */
}

/* Publica um texto num tópico. Pode ser chamada de qualquer thread do
 * processo central (o Coletor usa para atr/eventos). Sem conexão, a
 * mensagem é descartada. */
void ponte_publicar(const char *topico, const char *texto)
{
    mosquitto_publish(cliente, NULL, topico, strlen(texto), texto, 0, false);
}

/* main, DEPOIS de todas as threads terminarem: desconecta, para a thread
 * de rede e libera o cliente. */
void ponte_destruir(void)
{
    if (rede_iniciada) {
        mosquitto_disconnect(cliente);
        mosquitto_loop_stop(cliente, false); /* espera a thread de rede terminar */
    }
    mosquitto_destroy(cliente);
    mosquitto_lib_cleanup();
}

/* Tenta a primeira conexão. Se der certo, inicia a thread de rede da
 * biblioteca (que daí em diante cuida das reconexões) e retorna true. */
static bool tentar_conectar(struct mosquitto *mosq, bool *avisou)
{
    if (mosquitto_connect(mosq, broker_endereco, BROKER_PORTA, MANTER_VIVO_S)
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
    sem_t *semaforos[N_PASSAGENS];
    struct timespec proximo;
    bool avisou = false;
    int i, ciclo = 0;

    (void)arg;
    abrir_semaforos(semaforos);

    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        if (!rede_iniciada && ciclo % CICLOS_ENTRE_TENTATIVAS == 0)
            rede_iniciada = tentar_conectar(cliente, &avisou);
        if (rede_iniciada)
            publicar_status(cliente, semaforos);
        ciclo++;
        somar_ms(&proximo, PERIODO_PONTE_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }

    /* A conexão continua aberta: o Coletor ainda publica os últimos
     * eventos. Quem desconecta é ponte_destruir(), no fim do main. */
    for (i = 0; i < N_PASSAGENS; i++)
        sem_close(semaforos[i]);
    return NULL;
}
