/*
 * console.c — Console de testes (só Etapa 1; substitui as telas gráficas).
 *
 * Não cria thread: executar_console() roda na THREAD PRINCIPAL do processo
 * central. Lê comandos do stdin, uma linha por vez, e retorna quando chega
 * "sair" ou o fim da entrada; aí o main faz o encerramento.
 * Como lê do stdin, dá para rodar testes automáticos com:
 *   ./logistica < testes/cenarioN.txt
 *
 * Comandos já implementados:
 *   novo [x y]        pede um novo robô ao Gestor
 *   status            imprime os robôs ativos
 *   espera <seg>      pausa a leitura de comandos (para os cenários de teste)
 *   aguarda <id> <ESTADO> [timeout_s]  pausa até o robô estar no estado
 *                     pedido (padrão 30 s); deixa os cenários determinísticos
 *   bateria <id> <pct>  força o nível de bateria (emula o hardware)
 *   falha <id> / ok <id>  liga/desliga a falha (emula o hardware)
 *   goto <id> <x> <y> envia GOTO à Navegação do robô (só em modo manual)
 *   manual <id> / auto <id>  envia MODO_MANUAL / MODO_AUTO ao robô
 *   caixa <tam> <baia> <zona>  coloca uma caixa no fim da fila (zona A-D)
 *   sair              encerra tudo (fim do arquivo também encerra)
 *   Linhas começando com '#' são comentários (usados nos cenários).
 *
 * Sincronização:
 *   - pedir_novo_robo() usa a fila de pedidos do Gestor (ver gestor.c).
 *   - "status" lê SÓ o buffer de status (nunca o hw), copiando cada slot
 *     com o mutex do robô travado, para não imprimir um slot pela metade
 *     enquanto a thread Status o atualiza.
 *   - bateria/falha/ok fazem o papel do HARDWARE (emulam o simulador) e
 *     por isso escrevem no hw, com o mutex do robô travado, porque o
 *     simulador e o processo robo usam esses campos ao mesmo tempo.
 *   - "aguarda" lê o e_estado do BUFFER DE STATUS a cada 100 ms, com o mutex
 *     do robô travado (a Status de outro processo escreve nele).
 *   - goto/manual/auto usam a fila de mensagens do robô (enviar_para_robo).
 *   - caixa e a listagem de caixas usam mem->mutex_caixas, porque o
 *     Organizador mexe na mesma fila de caixas.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Comando "novo [x y]". */
static void comando_novo(const char *linha)
{
    int x = -1, y = -1;

    if (sscanf(linha, "novo %d %d", &x, &y) != 2) {
        x = -1;  /* sem posição: o Gestor usa a posição padrão */
        y = -1;
    } else if (!posicao_valida(x, y)) {
        printf("Console: (%d,%d) fora do mapa ou dentro de passagem\n", x, y);
        return;
    }
    if (!pedir_novo_robo(x, y))
        printf("Console: fila de pedidos cheia, tente de novo\n");
}

/* Lista as caixas ainda não entregues, na ordem da fila. */
static void listar_caixas(void)
{
    static const char *nome_status[] = {"esperando", "em andamento", "entregue"};
    Caixa c;
    int i, entregues = 0;

    pthread_mutex_lock(&mem->mutex_caixas);
    for (i = 0; i < mem->n_caixas; i++) {
        c = mem->caixas[i];
        if (c.status == CAIXA_ENTREGUE) {
            entregues++;
            continue;
        }
        /* printf com o mutex travado: a lista é curta e isto é só teste. */
        printf("caixa %-3d tam %d  baia %d -> zona %c  %s\n", c.id, c.tamanho,
               c.baia, 'A' + c.zona, nome_status[c.status]);
    }
    pthread_mutex_unlock(&mem->mutex_caixas);
    printf("caixas entregues: %d\n", entregues);
}

/* Comando "status": uma linha por robô ativo e depois as caixas. */
static void comando_status(void)
{
    Robo copia;
    int i;

    printf("id  pid     estado             pos      sp       bat   dist  modo  carga\n");
    for (i = 0; i < MAX_ROBOS; i++) {
        /* Copia o slot com o mutex travado e imprime depois, fora do mutex
         * (printf é lento; não vale a pena segurar o mutex durante ele). */
        pthread_mutex_lock(&mem->robos[i].mutex);
        copia = mem->robos[i];
        pthread_mutex_unlock(&mem->robos[i].mutex);

        if (!copia.ativo)
            continue;
        /* Só campos do buffer de status (nunca copia.hw). */
        printf("%-3d %-7d %-18s (%2d,%2d)  (%2d,%2d)  %3d%%  %4.0f  %-4s  %d\n",
               i, (int)copia.pid, nome_estado(copia.e_estado),
               copia.e_posicao_x, copia.e_posicao_y,
               copia.j_sp_posicao_x, copia.j_sp_posicao_y,
               copia.bateria, copia.distancia_total,
               copia.modo_automatico ? "auto" : "man", copia.carga_id);
    }
    listar_caixas();
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

/* Comando "bateria <id> <pct>". */
static void comando_bateria(const char *linha)
{
    int id, pct;
    Robo *r;

    if (sscanf(linha, "bateria %d %d", &id, &pct) != 2 || pct < 0 || pct > 100) {
        printf("Console: uso: bateria <id> <0..100>\n");
        return;
    }
    r = robo_ativo(id);
    if (r == NULL) { printf("Console: robo %d nao existe\n", id); return; }
    pthread_mutex_lock(&r->mutex);
    r->hw.i_percentual_bateria = pct;
    pthread_mutex_unlock(&r->mutex);
}

/* Comandos "falha <id>" (valor = true) e "ok <id>" (valor = false). */
static void comando_falha(const char *linha, bool valor)
{
    char comando[16];
    int id;
    Robo *r;

    if (sscanf(linha, "%15s %d", comando, &id) != 2) {
        printf("Console: uso: falha <id> / ok <id>\n");
        return;
    }
    r = robo_ativo(id);
    if (r == NULL) { printf("Console: robo %d nao existe\n", id); return; }
    pthread_mutex_lock(&r->mutex);
    r->hw.i_falha = valor;
    pthread_mutex_unlock(&r->mutex);
}

/* Comando "goto <id> <x> <y>": envia GOTO à Navegação do robô. */
static void comando_goto(const char *linha)
{
    MensagemRobo msg;
    int id, x, y;

    if (sscanf(linha, "goto %d %d %d", &id, &x, &y) != 3) {
        printf("Console: uso: goto <id> <x> <y>\n");
        return;
    }
    if (!posicao_valida(x, y)) {
        printf("Console: (%d,%d) fora do mapa ou dentro de passagem\n", x, y);
        return;
    }
    if (robo_ativo(id) == NULL) { printf("Console: robo %d nao existe\n", id); return; }

    memset(&msg, 0, sizeof(msg));
    msg.tipo = GOTO;
    msg.x = x;
    msg.y = y;
    enviar_para_robo(id, &msg);
}

/* Comandos "manual <id>" e "auto <id>": mensagem para a Navegação. */
static void comando_modo(const char *linha, int tipo)
{
    char comando[16];
    MensagemRobo msg;
    int id;

    if (sscanf(linha, "%15s %d", comando, &id) != 2) {
        printf("Console: uso: manual <id> / auto <id>\n");
        return;
    }
    if (robo_ativo(id) == NULL) { printf("Console: robo %d nao existe\n", id); return; }
    memset(&msg, 0, sizeof(msg));
    msg.tipo = tipo;
    enviar_para_robo(id, &msg);
}

/* Comando "caixa <tam> <baia> <zona>". */
static void comando_caixa(const char *linha)
{
    int tamanho, baia;
    char zona;

    if (sscanf(linha, "caixa %d %d %c", &tamanho, &baia, &zona) != 3 ||
        !adicionar_caixa(-1, tamanho, baia, zona - 'A')) {
        printf("Console: uso: caixa <tam 1-4> <baia 1-4> <zona A-D>\n");
    }
}

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

    while (fgets(linha, sizeof(linha), stdin) != NULL) {
        if (sscanf(linha, "%15s", comando) != 1 || comando[0] == '#')
            continue; /* linha vazia ou comentário */

        if (strcmp(comando, "novo") == 0)
            comando_novo(linha);
        else if (strcmp(comando, "status") == 0)
            comando_status();
        else if (strcmp(comando, "espera") == 0)
            comando_espera(linha);
        else if (strcmp(comando, "aguarda") == 0)
            comando_aguarda(linha);
        else if (strcmp(comando, "bateria") == 0)
            comando_bateria(linha);
        else if (strcmp(comando, "falha") == 0)
            comando_falha(linha, true);
        else if (strcmp(comando, "ok") == 0)
            comando_falha(linha, false);
        else if (strcmp(comando, "goto") == 0)
            comando_goto(linha);
        else if (strcmp(comando, "manual") == 0)
            comando_modo(linha, MODO_MANUAL);
        else if (strcmp(comando, "auto") == 0)
            comando_modo(linha, MODO_AUTO);
        else if (strcmp(comando, "caixa") == 0)
            comando_caixa(linha);
        else if (strcmp(comando, "sair") == 0)
            return;
        else
            printf("Console: comando desconhecido ou ainda nao implementado: %s\n",
                   comando);
    }
}
