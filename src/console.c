/*
 * console.c — Comandos de operação (Console de testes + Tela de Operação).
 *
 * Não cria thread. Duas formas de usar os mesmos comandos:
 *   - executar_console(): roda na THREAD PRINCIPAL do processo central. Lê
 *     comandos do stdin, uma linha por vez, e retorna quando chega "sair"
 *     ou o fim da entrada; aí o main faz o encerramento. Como lê do stdin,
 *     dá para rodar testes automáticos com:
 *       ./logistica testes/sem_caixas.txt < testes/cenarioN.txt
 *   - executar_comando(): executa UMA linha e devolve a resposta em texto.
 *     Usada pelo laço acima e pela ponte MQTT (tópico atr/comando, na
 *     thread de rede da libmosquitto). Uma regra, um lugar: a Tela de
 *     Operação manda exatamente as mesmas linhas que se digitam aqui.
 *
 * Comandos (executar_comando):
 *   novo [x y]        pede um novo robô ao Gestor
 *   status            tabela dos robôs ativos e das caixas pendentes
 *   bateria <id> <pct>  força o nível de bateria (emula o hardware)
 *   falha <id> / ok <id>  liga/desliga a falha (emula o hardware)
 *   goto <id> <x> <y> envia GOTO à Navegação do robô (só em modo manual)
 *   manual <id> / auto <id>  envia MODO_MANUAL / MODO_AUTO ao robô
 *   caixa <tam> <baia> <zona>  coloca uma caixa no fim da fila (zona A-D)
 * Só no Console (stdin) — pela ponte respondem "não aceito":
 *   espera <seg>      pausa a leitura de comandos (para os cenários de teste)
 *   aguarda <id> <ESTADO> [timeout_s]  pausa até o robô estar no estado
 *                     pedido (padrão 30 s); deixa os cenários determinísticos
 *   sair              encerra tudo (fim do arquivo também encerra)
 *   Linhas começando com '#' são comentários (usados nos cenários).
 *
 * Seguro para DUAS threads ao mesmo tempo (Console e callback do MQTT):
 * os comandos só usam variáveis locais e a resposta de quem chamou; não há
 * nenhuma variável "static" que mude; e os dados compartilhados só são
 * acessados pelos mecanismos que já existem:
 *   - pedir_novo_robo(): fila de pedidos do Gestor (mutex + cond, gestor.c).
 *   - "status" lê SÓ o buffer de status (nunca o hw), copiando cada slot
 *     com o mutex do robô travado, e as caixas com mem->mutex_caixas.
 *   - bateria/falha/ok fazem o papel do HARDWARE (emulam o simulador) e
 *     por isso escrevem no hw, com o mutex do robô travado, porque o
 *     simulador e o processo robo usam esses campos ao mesmo tempo.
 *   - goto/manual/auto: fila de mensagens do robô (enviar_para_robo abre e
 *     fecha a fila a cada envio; mq_send é atômico). Antes de enviar, goto
 *     confere modo_automatico e manual confere e_estado (buffer de status),
 *     com o mutex do robô, só para dar uma resposta clara ao operador; a
 *     Navegação do robô continua conferindo de novo ao receber (o estado
 *     pode mudar entre a conferência e a chegada da mensagem).
 *   - caixa: adicionar_caixa() usa mem->mutex_caixas (o Organizador mexe na
 *     mesma fila de caixas).
 *   - "aguarda" (só Console) lê o e_estado do buffer de status a cada
 *     100 ms, com o mutex do robô travado.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "comum.h"
#include "mapa.h"

const char *nome_estado(Estado e)
{
    switch (e) {
    case DESLIGADO:         return "DESLIGADO";
    case DISPONIVEL:        return "DISPONIVEL";
    case DESLOCANDO:        return "DESLOCANDO";
    case AGUARDANDO_EQUIPE: return "AGUARDANDO_EQUIPE";
    case OPERANDO_CARGA:    return "OPERANDO_CARGA";
    case RECARREGANDO:      return "RECARREGANDO";
    case DEFEITO:           return "DEFEITO";
    case MANUAL:            return "MANUAL";
    }
    return "?";
}

/* Escreve a resposta (como printf) e retorna "resultado" — atalho para
 * "escreve a mensagem e retorna" numa linha só. */
static bool responder(bool resultado, char *resp, int tam, const char *formato, ...)
{
    va_list args;

    va_start(args, formato);
    vsnprintf(resp, tam, formato, args);
    va_end(args);
    return resultado;
}

/* Acrescenta texto ao fim da resposta (usado pelo "status", que tem várias
 * linhas). Retorna quantos caracteres já foram usados (no máximo tam). */
static int anexar(char *resp, int tam, int usado, const char *formato, ...)
{
    va_list args;
    int n;

    if (usado >= tam)
        return tam;
    va_start(args, formato);
    n = vsnprintf(resp + usado, tam - usado, formato, args);
    va_end(args);
    return (n < 0 || usado + n > tam) ? tam : usado + n;
}

/* Retorna o ponteiro do robô "id" se ele existe e está ativo; senão NULL. */
static Robo *robo_ativo(int id)
{
    bool ativo;

    if (id < 0 || id >= MAX_ROBOS)
        return NULL;
    pthread_mutex_lock(&mem->robos[id].mutex);
    ativo = mem->robos[id].ativo;
    pthread_mutex_unlock(&mem->robos[id].mutex);
    return ativo ? &mem->robos[id] : NULL;
}

/* Lê, com o mutex do robô, o modo e o estado (buffer de status). */
static void ler_modo_estado(Robo *r, bool *automatico, Estado *estado)
{
    pthread_mutex_lock(&r->mutex);
    *automatico = r->modo_automatico;
    *estado = r->e_estado;
    pthread_mutex_unlock(&r->mutex);
}

/* Comando "novo [x y]". */
static bool comando_novo(const char *linha, char *resp, int tam)
{
    int x = -1, y = -1;

    if (sscanf(linha, "novo %d %d", &x, &y) != 2) {
        x = -1;  /* sem posição: o Gestor usa a posição padrão */
        y = -1;
    } else if (!posicao_valida(x, y)) {
        return responder(false, resp, tam, "(%d,%d) fora do mapa ou dentro de passagem", x, y);
    }
    if (!pedir_novo_robo(x, y))
        return responder(false, resp, tam, "fila de pedidos cheia, tente de novo");
    return responder(true, resp, tam, "ok");
}

/* Acrescenta as caixas ainda não entregues, na ordem da fila. */
static int listar_caixas(char *resp, int tam, int usado)
{
    static const char *nome_status[] = {"esperando", "em andamento", "entregue"};
    Caixa caixas[MAX_CAIXAS];
    int i, n, entregues = 0;

    /* Copia com o mutex e monta o texto depois, fora dele. */
    pthread_mutex_lock(&mem->mutex_caixas);
    n = mem->n_caixas;
    memcpy(caixas, mem->caixas, n * sizeof(Caixa));
    pthread_mutex_unlock(&mem->mutex_caixas);

    for (i = 0; i < n; i++) {
        if (caixas[i].status == CAIXA_ENTREGUE) {
            entregues++;
            continue;
        }
        usado = anexar(resp, tam, usado, "caixa %-3d tam %d  baia %d -> zona %c  %s\n",
                       caixas[i].id, caixas[i].tamanho, caixas[i].baia,
                       'A' + caixas[i].zona, nome_status[caixas[i].status]);
    }
    return anexar(resp, tam, usado, "caixas entregues: %d\n", entregues);
}

/* Comando "status": uma linha por robô ativo e depois as caixas. */
static bool comando_status(char *resp, int tam)
{
    Robo copia;
    int i, usado = 0;

    usado = anexar(resp, tam, usado,
                   "id  pid     estado             pos      sp       bat   dist  modo  carga\n");
    for (i = 0; i < MAX_ROBOS; i++) {
        /* Copia o slot com o mutex travado e monta o texto depois, fora
         * do mutex (não vale a pena segurar o robô parado por isso). */
        pthread_mutex_lock(&mem->robos[i].mutex);
        copia = mem->robos[i];
        pthread_mutex_unlock(&mem->robos[i].mutex);

        if (!copia.ativo)
            continue;
        /* Só campos do buffer de status (nunca copia.hw). */
        usado = anexar(resp, tam, usado,
                       "%-3d %-7d %-18s (%2d,%2d)  (%2d,%2d)  %3d%%  %4.0f  %-4s  %d\n",
                       i, (int)copia.pid, nome_estado(copia.e_estado),
                       copia.e_posicao_x, copia.e_posicao_y,
                       copia.j_sp_posicao_x, copia.j_sp_posicao_y,
                       copia.bateria, copia.distancia_total,
                       copia.modo_automatico ? "auto" : "man", copia.carga_id);
    }
    listar_caixas(resp, tam, usado);
    return true;
}

/* Comando "bateria <id> <pct>". */
static bool comando_bateria(const char *linha, char *resp, int tam)
{
    int id, pct;
    Robo *r;

    if (sscanf(linha, "bateria %d %d", &id, &pct) != 2 || pct < 0 || pct > 100)
        return responder(false, resp, tam, "uso: bateria <id> <0..100>");
    r = robo_ativo(id);
    if (r == NULL)
        return responder(false, resp, tam, "robo %d nao existe", id);
    pthread_mutex_lock(&r->mutex);
    r->hw.i_percentual_bateria = pct;
    pthread_mutex_unlock(&r->mutex);
    return responder(true, resp, tam, "ok");
}

/* Comandos "falha <id>" (valor = true) e "ok <id>" (valor = false). */
static bool comando_falha(const char *linha, bool valor, char *resp, int tam)
{
    char comando[16];
    int id;
    Robo *r;

    if (sscanf(linha, "%15s %d", comando, &id) != 2)
        return responder(false, resp, tam, "uso: falha <id> / ok <id>");
    r = robo_ativo(id);
    if (r == NULL)
        return responder(false, resp, tam, "robo %d nao existe", id);
    pthread_mutex_lock(&r->mutex);
    r->hw.i_falha = valor;
    pthread_mutex_unlock(&r->mutex);
    return responder(true, resp, tam, "ok");
}

/* Comando "goto <id> <x> <y>": envia GOTO à Navegação do robô. */
static bool comando_goto(const char *linha, char *resp, int tam)
{
    MensagemRobo msg;
    int id, x, y;
    Robo *r;
    bool automatico;
    Estado estado;

    if (sscanf(linha, "goto %d %d %d", &id, &x, &y) != 3)
        return responder(false, resp, tam, "uso: goto <id> <x> <y>");
    if (!posicao_valida(x, y))
        return responder(false, resp, tam, "(%d,%d) fora do mapa ou dentro de passagem", x, y);
    r = robo_ativo(id);
    if (r == NULL)
        return responder(false, resp, tam, "robo %d nao existe", id);
    ler_modo_estado(r, &automatico, &estado);
    if (automatico)
        return responder(false, resp, tam, "robo em modo automatico");

    memset(&msg, 0, sizeof(msg));
    msg.tipo = GOTO;
    msg.x = x;
    msg.y = y;
    enviar_para_robo(id, &msg);
    return responder(true, resp, tam, "ok");
}

/* Comandos "manual <id>" e "auto <id>": mensagem para a Navegação. */
static bool comando_modo(const char *linha, int tipo, char *resp, int tam)
{
    char comando[16];
    MensagemRobo msg;
    int id;
    Robo *r;
    bool automatico;
    Estado estado;

    if (sscanf(linha, "%15s %d", comando, &id) != 2)
        return responder(false, resp, tam, "uso: manual <id> / auto <id>");
    r = robo_ativo(id);
    if (r == NULL)
        return responder(false, resp, tam, "robo %d nao existe", id);
    ler_modo_estado(r, &automatico, &estado);
    if (tipo == MODO_MANUAL && estado != DISPONIVEL)
        return responder(false, resp, tam, "robo nao esta DISPONIVEL (estado %s)",
                         nome_estado(estado));
    memset(&msg, 0, sizeof(msg));
    msg.tipo = tipo;
    enviar_para_robo(id, &msg);
    return responder(true, resp, tam, "ok");
}

/* Comando "caixa <tam> <baia> <zona>". */
static bool comando_caixa(const char *linha, char *resp, int tam)
{
    int tamanho, baia;
    char zona;

    if (sscanf(linha, "caixa %d %d %c", &tamanho, &baia, &zona) != 3 ||
        !adicionar_caixa(-1, tamanho, baia, zona - 'A'))
        return responder(false, resp, tam, "uso: caixa <tam 1-4> <baia 1-4> <zona A-D>");
    return responder(true, resp, tam, "ok");
}

/* Executa UMA linha de comando e escreve a resposta em "resp": "ok", a
 * tabela do "status", ou a mensagem de erro (retorna false no erro).
 * Chamada pelo Console (thread principal) e pela ponte MQTT (thread de
 * rede da libmosquitto) — ver no topo por que é seguro. */
bool executar_comando(const char *linha, char *resp, int tam)
{
    char comando[16];

    if (sscanf(linha, "%15s", comando) != 1)
        return responder(false, resp, tam, "comando vazio");

    if (strcmp(comando, "novo") == 0)    return comando_novo(linha, resp, tam);
    if (strcmp(comando, "status") == 0)  return comando_status(resp, tam);
    if (strcmp(comando, "bateria") == 0) return comando_bateria(linha, resp, tam);
    if (strcmp(comando, "falha") == 0)   return comando_falha(linha, true, resp, tam);
    if (strcmp(comando, "ok") == 0)      return comando_falha(linha, false, resp, tam);
    if (strcmp(comando, "goto") == 0)    return comando_goto(linha, resp, tam);
    if (strcmp(comando, "manual") == 0)  return comando_modo(linha, MODO_MANUAL, resp, tam);
    if (strcmp(comando, "auto") == 0)    return comando_modo(linha, MODO_AUTO, resp, tam);
    if (strcmp(comando, "caixa") == 0)   return comando_caixa(linha, resp, tam);
    if (strcmp(comando, "espera") == 0 || strcmp(comando, "aguarda") == 0 ||
        strcmp(comando, "sair") == 0)
        return responder(false, resp, tam, "comando %s nao aceito aqui (so no Console)", comando);
    return responder(false, resp, tam, "comando desconhecido: %s", comando);
}

/* ---------- Comandos que só existem no Console (stdin) ---------- */

/* Comando "espera <segundos>" (aceita fração, ex.: espera 0.5).
 * Só a leitura de comandos para; as outras tarefas continuam rodando. */
static void comando_espera(const char *linha)
{
    double segundos;
    struct timespec t;

    if (sscanf(linha, "espera %lf", &segundos) != 1 || segundos < 0) {
        printf("Console: uso: espera <segundos>\n");
        return;
    }
    t.tv_sec  = (time_t)segundos;
    t.tv_nsec = (long)((segundos - (double)t.tv_sec) * 1e9);
    nanosleep(&t, NULL);
}

/* Comando "aguarda <id> <ESTADO> [timeout_s]": espera (consultando a cada
 * 100 ms) até o buffer de status do robô mostrar o estado pedido. Também
 * espera o robô existir (ativo). Se passar do timeout, avisa e segue. */
static void comando_aguarda(const char *linha)
{
    struct timespec passo = {0, 100 * 1000000}; /* 100 ms */
    char nome[32];
    double timeout = 30.0, esperado = 0.0;
    int id, n;
    Estado alvo = DESLIGADO, atual;
    bool ativo, achou = false;

    n = sscanf(linha, "aguarda %d %31s %lf", &id, nome, &timeout);
    if (n < 2 || id < 0 || id >= MAX_ROBOS) {
        printf("Console: uso: aguarda <id> <ESTADO> [timeout_s]\n");
        return;
    }
    for (atual = DESLIGADO; atual <= MANUAL; atual++) {
        if (strcmp(nome, nome_estado(atual)) == 0) {
            alvo = atual;
            achou = true;
        }
    }
    if (!achou) { printf("Console: estado desconhecido: %s\n", nome); return; }

    while (esperado < timeout) {
        pthread_mutex_lock(&mem->robos[id].mutex);
        ativo = mem->robos[id].ativo;
        atual = mem->robos[id].e_estado;
        pthread_mutex_unlock(&mem->robos[id].mutex);
        if (ativo && atual == alvo) {
            printf("Console: robo %d em %s\n", id, nome);
            return;
        }
        nanosleep(&passo, NULL);
        esperado += 0.1;
    }
    printf("Console: AVISO: timeout de %.0f s esperando robo %d em %s\n",
           timeout, id, nome);
}

/* Laço de comandos. Retorna em "sair" ou no fim da entrada. */
void executar_console(void)
{
    char linha[128];
    char comando[16];
    char resp[TAMANHO_RESPOSTA];

    while (fgets(linha, sizeof(linha), stdin) != NULL) {
        if (sscanf(linha, "%15s", comando) != 1 || comando[0] == '#')
            continue; /* linha vazia ou comentário */

        if (strcmp(comando, "sair") == 0)
            return;
        if (strcmp(comando, "espera") == 0) {
            comando_espera(linha);
        } else if (strcmp(comando, "aguarda") == 0) {
            comando_aguarda(linha);
        } else if (!executar_comando(linha, resp, sizeof(resp))) {
            printf("Console: %s\n", resp);          /* erro */
        } else if (strcmp(resp, "ok") != 0) {
            printf("%s", resp);                     /* ex.: tabela do status */
        }
    }
}
