/*
 * ponte_mqtt.c — Ponte MQTT entre o núcleo em C e a Tela de Operação.
 *
 * Threads:
 *   - tarefa_ponte_mqtt (nossa, no processo central), periódica com período
 *     PERIODO_PONTE_MS (200 ms): monta o JSON do status e publica em
 *     "atr/status".
 *   - thread de rede da libmosquitto, criada por mosquitto_loop_start DEPOIS
 *     da primeira conexão: envia e recebe os pacotes MQTT e reconecta
 *     sozinha se o broker cair. Os callbacks (ao_conectar, ao_receber,
 *     ao_desconectar) rodam NELA.
 *
 * Tópicos:
 *   atr/mapa    (publica) JSON do mapa DESTA execução, gerado a partir de
 *               mem->mapa e de mapa.h (fonte única da verdade): largura,
 *               altura, semente, corredor_y, baias, zonas, estacoes,
 *               paredes (retângulos), obstaculos ([x,y]) e passagens
 *               (nome, capacidade, celulas do túnel e regiao). "Retained":
 *               o broker guarda a última cópia e entrega a quem assinar
 *               depois. Publicado a cada conexão.
 *   atr/status  (publica) JSON com robôs (com "bloqueado": esperando célula
 *               ou passagem), caixas, passagens (com "dentro" e "sentido"
 *               do monitor de mão única), a caixa que o
 *               Organizador está esperando (e quantos robôs livres havia) e
 *               contadores.
 *   atr/comando (assina) uma linha de comando igual à do Console; é
 *               executada por executar_comando() (console.c) na thread de
 *               rede da biblioteca, e o resultado vai para
 *               atr/comando/resposta (abaixo).
 *   atr/robo/novo (assina) pedido de novo robô vindo do simulador (painel
 *               "Simulação" da tela). Payload "x y" ou vazio (posição
 *               padrão). Validado com posicao_valida e entregue ao Gestor
 *               por pedir_novo_robo() (mesma fila do comando "novo").
 *   atr/comando/resposta (publica) "ok", a saída do comando, ou
 *               "erro: <mensagem>" (para atr/comando e atr/robo/novo).
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
 *   - mem->mapa: só leitura, sem mutex (não muda depois de gerado).
 *   - monitor de mão única das passagens (ler_monitor, ocupacao.c): copia
 *     dentro/sentido com o mutex do monitor, um por vez, só para exibir.
 *   - sem_getvalue nos semáforos das passagens (sem_passagem[], abertos
 *     pelo main antes das threads): é só uma leitura instantânea
 *     para EXIBIÇÃO (o valor pode mudar logo depois); não serve para decidir
 *     nada, e por isso não precisa de proteção extra.
 *   - libmosquitto: mosquitto_publish pode ser chamado desta thread, do
 *     Coletor e da thread de rede ao mesmo tempo; a biblioteca protege as
 *     filas internas dela com mutexes próprios.
 *   - "cliente", "broker_endereco" e "rede_iniciada" são globais deste
 *     arquivo, mas sem mutex: "cliente" e "broker_endereco" são escritos só
 *     em ponte_criar(), antes de existirem as outras threads
 *     (pthread_create garante que elas veem o valor); "cliente" só é
 *     destruído depois de todas terminarem; "rede_iniciada" é escrito só
 *     pela thread da ponte e lido pelo main depois do pthread_join dela.
 *   - Comandos recebidos: executar_comando() é segura para ser chamada ao
 *     mesmo tempo pelo Console e por aqui (ver console.c).
 *   - mem->mutex_tempos (PROCESS_SHARED): no fim de cada ciclo grava a
 *     medição (C, atraso, perdas) em mem->med_ponte; o comando "tempos" lê/
 *     zera ao mesmo tempo. Travado sozinho.
 *   - Buffers JSON estáticos (TAMANHO_JSON = 256 KB, grande demais para a
 *     pilha): json_status é usado SÓ por esta thread e json_mapa SÓ pela
 *     thread de rede (publicar_mapa roda no callback ao_conectar). Cada um
 *     tem um único usuário, então não precisam de mutex.
 *   - Com ATR_TEMPO_REAL=1: SCHED_FIFO prioridade 70 (tempo_real.c). A
 *     thread de rede da libmosquitto continua SCHED_OTHER.
 *   - Novo robô (atr/robo/novo): pedir_novo_robo() usa a fila de pedidos do
 *     Gestor (mutex_pedidos + cond_pedidos, gestor.c); esta thread é só mais
 *     um produtor, como o Console. Se a fila está cheia, recusa sem bloquear
 *     a thread de rede.
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
#define TAMANHO_JSON    262144  /* cabe 128 robôs (~600 B cada, com a rota de 32 cantos)
                                 * + 256 caixas (~110 B cada) com folga */
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

/* Buffers estáticos (ver "Sincronização" no topo: um usuário cada). */
static Json json_mapa;    /* só a thread de rede (ao_conectar) */
static Json json_status;  /* só a thread da ponte (publicar_status) */

/* ---------- atr/mapa ---------- */

/* Retângulos de parede: cada parede é cortada pelas suas passagens, então
 * vira um retângulo por trecho contínuo de células PAREDE na coluna. */
static void json_paredes(Json *j)
{
    int w, y, y0, x, n = 0;

    json_add(j, ",\"paredes\":[");
    for (w = 0; w < N_PAREDES; w++) {
        x = PAREDE_X[w];
        y0 = 0;
        for (y = 0; y <= MAPA_ALTURA; y++) {
            if (y < MAPA_ALTURA && mem->mapa.tipo[y][x] == PAREDE)
                continue;
            if (y > y0)   /* fim de um trecho y0..y-1 */
                json_add(j, "%s{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}", n++ ? "," : "",
                         x, y0, x + ESPESSURA_PAREDE - 1, y - 1);
            y0 = y + 1;
        }
    }
    json_add(j, "]");
}

static void json_obstaculos(Json *j)
{
    int x, y, n = 0;

    json_add(j, ",\"obstaculos\":[");
    for (y = 0; y < MAPA_ALTURA; y++)
        for (x = 0; x < MAPA_LARGURA; x++)
            if (mem->mapa.tipo[y][x] == OBSTACULO)
                json_add(j, "%s[%d,%d]", n++ ? "," : "", x, y);
    json_add(j, "]");
}

/* Passagens: "celulas" = túnel (dentro da parede); "regiao" = túnel + as
 * duas bocas (onde vale o limite de capacidade). */
static void json_passagens_mapa(Json *j)
{
    const Passagem *p;
    int i, x;

    json_add(j, ",\"passagens\":[");
    for (i = 0; i < N_PASSAGENS; i++) {
        p = &mem->mapa.passagens[i];
        json_add(j, "%s{\"nome\":\"P%d\",\"capacidade\":%d,\"celulas\":[",
                 i ? "," : "", i + 1, p->capacidade);
        for (x = p->x_entrada + 1; x < p->x_saida; x++)
            json_add(j, "%s[%d,%d]", x > p->x_entrada + 1 ? "," : "", x, p->y);
        json_add(j, "],\"regiao\":[");
        for (x = p->x_entrada; x <= p->x_saida; x++)
            json_add(j, "%s[%d,%d]", x > p->x_entrada ? "," : "", x, p->y);
        json_add(j, "]}");
    }
    json_add(j, "]");
}

/* Monta o JSON do mapa a partir de mem->mapa e das constantes de mapa.h. */
static void publicar_mapa(struct mosquitto *mosq)
{
    Json *j = &json_mapa;
    int i;

    j->usado = 0;
    json_add(j, "{\"largura\":%d,\"altura\":%d,\"semente\":%u,\"corredor_y\":%d,\"baias\":[",
             MAPA_LARGURA, MAPA_ALTURA, mem->mapa.semente, CORREDOR_Y);
    for (i = 0; i < N_BAIAS; i++)
        json_add(j, "{\"id\":%d,\"x\":%d,\"y\":%d}%s", i + 1, BAIA_X[i], BAIA_Y[i],
                 i < N_BAIAS - 1 ? "," : "");
    json_add(j, "],\"zonas\":[");
    for (i = 0; i < N_ZONAS; i++)
        json_add(j, "{\"id\":\"%c\",\"x\":%d,\"y\":%d}%s", 'A' + i, ZONA_X[i], ZONA_Y[i],
                 i < N_ZONAS - 1 ? "," : "");
    json_add(j, "],\"estacoes\":[");
    for (i = 0; i < N_ESTACOES; i++)
        json_add(j, "{\"x\":%d,\"y\":%d}%s", ESTACAO_X[i], ESTACAO_Y[i],
                 i < N_ESTACOES - 1 ? "," : "");
    json_add(j, "]");
    json_paredes(j);
    json_obstaculos(j);
    json_passagens_mapa(j);
    json_add(j, "}");

    publicar(mosq, "atr/mapa", j, true);  /* retained */
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

/* Termina o robô com ,"rota":[[x,y],...]} (próximos cantos do caminho). */
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
                 "\"bateria\":%d,\"distancia\":%.0f,\"carga\":%d,\"lider\":%d,"
                 "\"bloqueado\":%s",
                 n > 0 ? "," : "", i, nome_estado(r.e_estado),
                 r.modo_automatico ? "auto" : "manual", r.e_posicao_x, r.e_posicao_y,
                 r.j_sp_posicao_x, r.j_sp_posicao_y, r.destino_x, r.destino_y,
                 r.bateria, r.distancia_total, r.carga_id,
                 r.carga_id == -1 ? -1 : lider_da_carga(r.carga_id, caixas, n_caixas),
                 r.bloqueado ? "true" : "false");
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

/* Ocupação das passagens: livres = valor atual do semáforo; dentro e
 * sentido (0 = para a direita, 1 = para a esquerda) = monitor de mão única. */
static void json_passagens(Json *j)
{
    int i, livres, dentro, sentido;

    json_add(j, ",\"passagens\":[");
    for (i = 0; i < N_PASSAGENS; i++) {
        /* Leitura instantânea, só para exibir (ver comentário do topo). */
        if (sem_getvalue(sem_passagem[i], &livres) == -1)
            livres = -1;
        ler_monitor(i, &dentro, &sentido);
        json_add(j, "%s{\"nome\":\"P%d\",\"capacidade\":%d,\"livres\":%d,"
                 "\"dentro\":%d,\"sentido\":%d}",
                 i > 0 ? "," : "", i + 1, mem->mapa.passagens[i].capacidade, livres,
                 dentro, sentido);
    }
    json_add(j, "]");
}

static void publicar_status(struct mosquitto *mosq)
{
    Json *j = &json_status;
    Caixa caixas[MAX_CAIXAS];
    struct timespec agora;
    char data_hora[48];
    int n_caixas, ativos, pendentes, andamento, entregues, em_espera, livres;

    clock_gettime(CLOCK_REALTIME, &agora);
    formatar_data_hora(agora, data_hora, sizeof(data_hora));

    j->usado = 0;
    json_add(j, "{\"t\":\"%s\",", data_hora);
    n_caixas = copiar_caixas(caixas, &em_espera, &livres);
    ativos = json_robos(j, caixas, n_caixas);
    json_caixas(j, caixas, n_caixas, &pendentes, &andamento, &entregues);
    json_passagens(j);
    json_add(j, ",\"organizador\":{\"caixa_em_espera\":%d,\"robos_livres\":%d}",
             em_espera, livres);
    json_add(j, ",\"contadores\":{\"robos_ativos\":%d,\"entregues\":%d,"
             "\"pendentes\":%d,\"em_andamento\":%d}}",
             ativos, entregues, pendentes, andamento);

    publicar(mosq, "atr/status", j, false);
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
    mosquitto_subscribe(mosq, NULL, "atr/robo/novo", 0);
    publicar_mapa(mosq);
}

/* atr/robo/novo: "x y" ou vazio (posição padrão). Escreve em "texto" a
 * resposta ("ok" ou "erro: ..."). */
static void novo_robo(const char *payload, char *texto, int tam)
{
    char resto;
    int x = -1, y = -1, n;

    n = sscanf(payload, "%d %d %c", &x, &y, &resto);
    if (n == EOF) {
        x = -1;  /* vazio: o Gestor usa a posição padrão */
        y = -1;
    } else if (n != 2) {
        snprintf(texto, tam, "erro: uso do atr/robo/novo: \"x y\" ou vazio");
        return;
    } else if (!posicao_valida(x, y)) {
        snprintf(texto, tam, "erro: (%d,%d) invalida: fora do mapa, parede, obstaculo ou passagem", x, y);
        return;
    }
    if (!pedir_novo_robo(x, y)) {
        snprintf(texto, tam, "erro: fila de pedidos cheia, tente de novo");
        return;
    }
    snprintf(texto, tam, "ok");
}

/* Callback (thread de rede da biblioteca): chegou uma mensagem.
 *   atr/comando:   executa a linha como se fosse o Console;
 *   atr/robo/novo: pede um novo robô ao Gestor.
 * Nos dois casos publica a resposta em atr/comando/resposta. */
static void ao_receber(struct mosquitto *mosq, void *dados,
                       const struct mosquitto_message *msg)
{
    char linha[128];
    char resp[TAMANHO_RESPOSTA];
    char texto[TAMANHO_RESPOSTA + 8];
    int n;
    bool ok;

    (void)dados;
    /* O payload não termina em '\0': copia com limite e termina. Payload
     * vazio (atr/robo/novo sem posição) pode vir com ponteiro NULL. */
    n = msg->payloadlen < (int)sizeof(linha) - 1 ? msg->payloadlen : (int)sizeof(linha) - 1;
    if (n > 0)
        memcpy(linha, msg->payload, n);
    linha[n] = '\0';

    if (atomic_load(&mem->encerrar)) {
        snprintf(texto, sizeof(texto), "erro: sistema encerrando");
    } else if (strcmp(msg->topic, "atr/robo/novo") == 0) {
        novo_robo(linha, texto, sizeof(texto));
        printf("Ponte MQTT: novo robo \"%s\" -> %s\n", linha, texto);
    } else if (strcmp(msg->topic, "atr/comando") != 0) {
        return;  /* tópico não assinado: ignora */
    } else {
        ok = executar_comando(linha, resp, sizeof(resp));
        snprintf(texto, sizeof(texto), "%s%s", ok ? "" : "erro: ", resp);
        /* "tempos" chega da tela a cada 1 s: não enche o terminal com ele. */
        if (strcmp(linha, "tempos") != 0)
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

void *tarefa_ponte_mqtt(void *arg)
{
    struct timespec proximo, inicio, fim;
    bool avisou = false;
    int ciclo = 0;

    (void)arg;
    ativar_tempo_real(PRIO_PONTE, "ponte_mqtt");

    clock_gettime(CLOCK_MONOTONIC, &proximo);
    while (!atomic_load(&mem->encerrar)) {
        clock_gettime(CLOCK_MONOTONIC, &inicio);
        if (!rede_iniciada && ciclo % CICLOS_ENTRE_TENTATIVAS == 0)
            rede_iniciada = tentar_conectar(cliente, &avisou);
        if (rede_iniciada)
            publicar_status(cliente);
        ciclo++;
        clock_gettime(CLOCK_MONOTONIC, &fim);
        pthread_mutex_lock(&mem->mutex_tempos);
        medir(&mem->med_ponte, proximo, inicio, fim, PERIODO_PONTE_MS);
        pthread_mutex_unlock(&mem->mutex_tempos);
        somar_ms(&proximo, PERIODO_PONTE_MS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    }

    /* A conexão continua aberta: o Coletor ainda publica os últimos
     * eventos. Quem desconecta é ponte_destruir(), no fim do main. */
    return NULL;
}
