/*
 * comum.h — Definições comuns a todo o sistema (processo central e robôs).
 *
 * Contém:
 *   - constantes (limites, períodos das tarefas periódicas);
 *   - nomes dos objetos de IPC POSIX (memória compartilhada, filas, semáforos);
 *   - enums e structs dos dados compartilhados e das mensagens;
 *   - protótipos das funções de memoria.c.
 *
 * Este arquivo não cria threads nem processos. Ele só descreve os dados.
 * O único mecanismo de sincronização declarado aqui são os mutexes que
 * ficam DENTRO da memória compartilhada (ver comentários nas structs).
 * O mapa (mem->mapa, struct Mapa de mapa.h) também fica na memória
 * compartilhada, mas sem mutex: é só leitura depois de gerado (ver mapa.h).
 */
#ifndef COMUM_H
#define COMUM_H

#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include <mqueue.h>
#include "mapa.h"

/* ---------- Limites ---------- */
/* MAX_ROBOS = 128 para o teste de escalabilidade (testes/escalabilidade.sh).
 * O que depende dele (conferido para 128, ver docs/ESCALABILIDADE.md):
 *   - o tamanho de /atr_shm (128 slots de ~0,4 KB; /atr_shm inteira
 *     com o mapa, a grade e as caixas = ~100 KB, medido);
 *   - o JSON da ponte MQTT (TAMANHO_JSON) e a resposta do "status"
 *     (TAMANHO_RESPOSTA);
 *   - a fila de pedidos do Gestor (MAX_PEDIDOS = MAX_ROBOS);
 *   - as filas POSIX: 128 filas /atr_robo_* + /atr_coletor = 129 filas,
 *     abaixo do limite do Linux (/proc/sys/fs/mqueue/queues_max = 256).
 *     Cada fila de robô custa ~1,1 KB do RLIMIT_MSGQUEUE (800 KB por
 *     usuário): 128 x 1,1 KB + 2,5 KB (coletor) = ~145 KB.
 *   - o limite real seguinte seria queues_max: com MAX_ROBOS > 255 o
 *     Gestor não conseguiria criar a fila do robô (mq_open: EMFILE/ENOSPC). */
#define MAX_ROBOS  128
#define MAX_CAIXAS 256   /* o benchmark coloca 2 caixas por robô (no mínimo 64) */
#define MAX_ROTA_STATUS 32    /* "cantos" da rota publicados no buffer de status */

/* ---------- Períodos das tarefas periódicas (em milissegundos) ---------- */
#define PERIODO_NAVEGACAO_MS    100   /* T1 */
#define PERIODO_STATUS_MS       200   /* T2 */
#define PERIODO_ORGANIZADOR_MS  500   /* T3 */
#define PERIODO_SIMULADOR_MS    100   /* simulador do hardware (simulador.c) */
#define PERIODO_PONTE_MS        200   /* ponte MQTT: publica atr/status */

/* ---------- Prioridades de tempo real (só com ATR_TEMPO_REAL=1) ----------
 * Rate Monotonic: menor período = maior prioridade (SCHED_FIFO, 1..99).
 * Coletor e Gestor não são periódicos (acordam por evento): ficam abaixo de
 * todas as periódicas. Ver ativar_tempo_real() em tempo_real.c. */
#define PRIO_NAVEGACAO    80   /* T = 100 ms */
#define PRIO_SIMULADOR    80   /* T = 100 ms */
#define PRIO_STATUS       70   /* T = 200 ms */
#define PRIO_PONTE        70   /* T = 200 ms */
#define PRIO_ORGANIZADOR  60   /* T = 500 ms */
#define PRIO_COLETOR      50   /* esporádica */
#define PRIO_GESTOR       50   /* esporádica */

/* Avança o instante "t" em "ms" milissegundos.
 * Usada nos laços periódicos: cada tarefa guarda o instante da PRÓXIMA
 * ativação e dorme até ele com clock_nanosleep(..., TIMER_ABSTIME, ...).
 * Com tempo absoluto o período não "escorrega": o tempo gasto no
 * trabalho de cada ciclo não se soma ao período (não há deriva). */
static inline void somar_ms(struct timespec *t, int ms)
{
    t->tv_nsec += (long)ms * 1000000;
    while (t->tv_nsec >= 1000000000) {
        t->tv_nsec -= 1000000000;
        t->tv_sec++;
    }
}

/* ---------- Medição de tempo das tarefas periódicas ----------
 * Uma Medicao por tarefa (Simulador, Organizador e Ponte na memória
 * compartilhada; Navegação e Status no slot de cada robô). Mostrada pelo
 * comando "tempos" (console.c) e usada na análise de escalabilidade
 * (docs/ESCALABILIDADE.md). Em cada ciclo:
 *   atraso = início real - instante planejado ("proximo")  (jitter de liberação)
 *   C      = fim - início                                   (tempo de execução)
 *   perda de prazo se fim > proximo + período (deadline = período). */
typedef struct {
    long   n_ciclos;
    double c_media_us, c_max_us;
    double atraso_max_us;
    long   perdas;
} Medicao;

/* b - a, em microssegundos. */
static inline double diferenca_us(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec - a.tv_sec) * 1e6 + (double)(b.tv_nsec - a.tv_nsec) / 1e3;
}

/* Acumula um ciclo na medição. "proximo" = instante em que o ciclo DEVIA
 * começar; inicio/fim = clock_gettime(CLOCK_MONOTONIC) logo depois de
 * acordar e logo depois do trabalho. Quem chama trava o mutex que protege
 * "m" (ver onde cada Medicao fica). Média incremental: não guarda amostras. */
static inline void medir(Medicao *m, struct timespec proximo, struct timespec inicio,
                         struct timespec fim, int periodo_ms)
{
    double c = diferenca_us(inicio, fim);
    double atraso = diferenca_us(proximo, inicio);

    m->n_ciclos++;
    m->c_media_us += (c - m->c_media_us) / (double)m->n_ciclos;
    if (c > m->c_max_us)
        m->c_max_us = c;
    if (atraso > m->atraso_max_us)
        m->atraso_max_us = atraso;
    if (diferenca_us(proximo, fim) > periodo_ms * 1000.0)
        m->perdas++;
}

/* ---------- Nomes dos objetos de IPC POSIX ---------- */
#define NOME_SHM              "/atr_shm"       /* memória compartilhada */
#define PREFIXO_FILA_ROBO     "/atr_robo_"     /* + id do robô */
#define NOME_FILA_COLETOR     "/atr_coletor"
#define PREFIXO_SEM_PASSAGEM  "/atr_passagem_" /* + número da passagem */
#define MAX_MSG_FILA          10  /* limite padrão do Linux para usuário comum */

/* ---------- Estados de um robô ---------- */
typedef enum {
    DESLIGADO,
    DISPONIVEL,
    DESLOCANDO,
    AGUARDANDO_EQUIPE,
    OPERANDO_CARGA,
    RECARREGANDO,
    DEFEITO,
    MANUAL
} Estado;

/* Tabela 1 do enunciado — interface com o hardware do robô.
 * i_ = entrada (hardware -> sistema), o_ = saída (sistema -> hardware). */
typedef struct {
    int  i_posicao_x, i_posicao_y;
    bool i_carga_atracada;
    int  i_percentual_bateria;
    bool i_falha;
    int  o_sp_posicao_x, o_sp_posicao_y;
    bool o_acoplar_e_erguer;
} Hardware;

/* Um slot por robô, dentro da memória compartilhada. */
typedef struct {
    /* POR QUE: este slot é lido e escrito por várias tarefas ao mesmo tempo
     * (simulador, Organizador e Console no processo central; Navegação e
     * Status no processo do robô). Sem o mutex, uma tarefa poderia ler o
     * slot pela metade (ex.: x novo com y antigo). Como as tarefas estão em
     * PROCESSOS diferentes, o mutex usa o atributo PTHREAD_PROCESS_SHARED. */
    pthread_mutex_t mutex;
    bool     ativo;            /* slot em uso? */
    pid_t    pid;              /* pid do processo "robo" */

    /* Interface com o hardware (Tabela 1): escrita pelo simulador (i_*) e
     * pelo processo robo (o_*). */
    Hardware hw;

    /* ---- Buffer de status (Tabela 2, Figura 3 do enunciado) ----
     * REGRA: só a thread Status do Robô ESCREVE nestes campos; a cada ciclo
     * ela copia do hw (e do estado interno do processo robo).
     * O Organizador e o Console LEEM só estes campos, NUNCA o hw.
     * Assim quem está fora do robô enxerga um retrato consistente,
     * atualizado num único ponto, e o hw fica restrito ao robô e ao
     * simulador. (Única exceção: o Gestor preenche os valores iniciais
     * antes de o processo robo existir.) */
    int      e_posicao_x, e_posicao_y;        /* posição atual */
    int      j_sp_posicao_x, j_sp_posicao_y;  /* setpoint atual */
    int      bateria;                         /* em % */
    Estado   e_estado;
    double   distancia_total;
    int      destino_x, destino_y;  /* fim da rota atual (-1 = sem rota) */
    int      n_rota;                /* próximos cantos do caminho (até 32): */
    int      rota_x[MAX_ROTA_STATUS], rota_y[MAX_ROTA_STATUS]; /* a tela desenha o caminho */

    bool     bloqueado;        /* esperando célula (grade) ou passagem (monitor) */

    bool     modo_automatico;
    int      carga_id;         /* -1 se nenhuma */
    /* Grupo anti-colisão (ver struct Ocupacao): id do robô, ou 1000 +
     * carga_id numa equipe de transporte com mais de 1 robô. Escrito pela
     * Navegação quando muda (como modo_automatico) e lido pelo simulador,
     * que confere se houve colisão (ver simulador.c). */
    int      grupo;
    /* Célula da grade onde o robô está (a última que a Navegação
     * confirmou). Escrita pela Navegação ao chegar numa célula (como o
     * grupo) e lida pelos SEGUIDORES da equipe, que dão o passo para a
     * célula do líder. POR QUE não usar e_posicao: o buffer de status é
     * atualizado a cada 200 ms; com ele cada passo da equipe em bloco
     * levaria até 300 ms a mais (ver "Equipe em bloco" em EXPLICACAO.md). */
    int      celula_x, celula_y;

    /* Medição de tempo das duas tarefas periódicas deste robô. Cada uma
     * escreve a sua no fim do ciclo, com o mutex deste slot (o mesmo que
     * ela já usa); o comando "tempos" lê/zera com o mesmo mutex. */
    Medicao  med_navegacao, med_status;
} Robo;

/* Situação de uma caixa. */
typedef enum {
    CAIXA_ESPERANDO,     /* na fila, sem equipe */
    CAIXA_EM_ANDAMENTO,  /* equipe formada, sendo levada */
    CAIXA_ENTREGUE
} StatusCaixa;

#define TAMANHO_MAX_CAIXA 4   /* = máximo de robôs numa equipe */

/* Bateria (em %). O simulador gasta 1% a cada CELULAS_POR_PORCENTO células
 * andadas; o Organizador e a Status usam o MESMO valor para estimar quanto
 * um caminho A* vai gastar: custo = passos / CELULAS_POR_PORCENTO.
 *   - Organizador: só escolhe o robô se bateria >= BATERIA_MINIMA_TAREFA
 *     (piso) E >= custo da tarefa INTEIRA (robô -> baia -> zona -> estação
 *     mais próxima da zona) + MARGEM_BATERIA;
 *   - Status: robô DISPONIVEL e sem carga vai recarregar se bateria <
 *     BATERIA_RECARREGAR OU < custo (robô -> estação mais próxima) +
 *     MARGEM_BATERIA.
 * O custo da tarefa termina numa estação, então (quase sempre) é maior que
 * o custo de ir direto à estação: as duas decisões não se sobrepõem. O caso
 * raro está em docs/EXPLICACAO.md ("Bateria suficiente para a tarefa"). */
#define CELULAS_POR_PORCENTO  5   /* bateria cai 1% a cada 5 células andadas */
#define MARGEM_BATERIA        10  /* folga (desvios, seguidores fora da linha) */
#define BATERIA_MINIMA_TAREFA 30  /* piso: Organizador nunca escolhe com < 30% */
#define BATERIA_RECARREGAR    20  /* piso: Status sempre manda recarregar com < 20% */

/* Uma caixa a ser transportada de uma baia até uma zona. */
typedef struct {
    int id;
    int tamanho;   /* 1 a 4 = número de robôs necessários */
    int baia;      /* 1 a 4 */
    int zona;      /* 0 a 3 = zonas A a D */
    int status;    /* um valor de StatusCaixa */
    /* Equipe (preenchida pelo Organizador ao formar a equipe; fica
     * guardada depois da entrega, como histórico). */
    int robos[TAMANHO_MAX_CAIXA];
    int n_robos;   /* 0 = ainda sem equipe */
    int lider;     /* -1 = sem equipe */
} Caixa;

/* ---------- Anti-colisão descentralizada (ponto extra +2, ocupacao.c) ---------- */

/* Grupo de uma equipe de transporte com mais de 1 robô = GRUPO_EQUIPE +
 * carga_id (os ids de robô vão de 0 a MAX_ROBOS - 1 = 127, então não se confundem). */
#define GRUPO_EQUIPE 1000

/* Grade de ocupação: quem está (ou reservou) cada célula.
 * Cada robô segura no máximo 2 células: a atual e a próxima reservada. */
typedef struct {
    /* POR QUE: as threads de Navegação de TODOS os robôs (processos
     * diferentes) testam e marcam células ao mesmo tempo; sem o mutex, dois
     * robôs poderiam ver a mesma célula livre e entrar os dois (colisão).
     * Um mutex só para a grade inteira: a seção crítica é minúscula (testar
     * e marcar 1 ou 2 células); um mutex por célula seriam 1800 mutexes sem
     * ganho real. PROCESS_SHARED por estar na memória compartilhada. */
    pthread_mutex_t mutex;
    int n[MAPA_ALTURA][MAPA_LARGURA];      /* quantos robôs estão/reservaram */
    int grupo[MAPA_ALTURA][MAPA_LARGURA];  /* grupo de quem está (vale se n > 0) */
} Ocupacao;

/* Monitor de uma passagem (mão única: problema da "ponte de uma via").
 * Sentido 0 = para a direita (entrou pela boca esquerda), 1 = para a esquerda.
 * A CAPACIDADE continua sendo o semáforo /atr_passagem_<n>; o monitor só
 * decide o SENTIDO. (O nome "Passagem" já é a geometria, em mapa.h.) */
typedef struct {
    /* POR QUE: dentro, sentido e esperando[] são lidos e mudados juntos
     * pelos robôs que chegam às duas bocas; sem o mutex, dois robôs de
     * sentidos opostos poderiam ver dentro == 0 ao mesmo tempo e entrar os
     * dois (travando no meio do túnel). PROCESS_SHARED (memória compartilhada). */
    pthread_mutex_t mutex;
    int  dentro;         /* robôs (unidades) dentro agora */
    int  sentido;        /* sentido de quem está dentro (vale se dentro > 0) */
    bool esperando[2];   /* há alguém esperando para entrar neste sentido? */
} MonitorPassagem;

/* Conteúdo completo de /atr_shm. */
typedef struct {
    Robo robos[MAX_ROBOS];

    /* Mapa desta execução (paredes, passagens sorteadas, obstáculos).
     * POR QUE fica sem mutex: é escrito só por gerar_mapa() antes de
     * existirem as outras threads e os robôs; depois só é lido, e leituras
     * simultâneas não são condição de corrida (ver a struct Mapa em mapa.h). */
    Mapa mapa;

    /* POR QUE: a lista de caixas é alterada pelo Console (nova caixa) e lida/
     * alterada pelo Organizador (escolhe e marca caixas). O mutex evita que
     * as duas threads mexam em n_caixas/caixas[] ao mesmo tempo.
     * É PROCESS_SHARED por estar na memória compartilhada (e por coerência
     * com os mutexes dos robôs). */
    pthread_mutex_t mutex_caixas;
    Caixa caixas[MAX_CAIXAS];  /* em ORDEM DE CHEGADA (a fila do Organizador) */
    int   n_caixas;
    /* Caixa que o Organizador está tentando atender e não conseguiu (falta
     * equipe), e quantos robôs livres havia — só para a tela mostrar o
     * motivo da espera. -1 = nenhuma esperando. Também protegidos por
     * mutex_caixas (são escritos junto com a fila). */
    int   caixa_em_espera;
    int   robos_livres_espera;

    /* Pedido de encerramento: escrito pelo processo central e lido por
     * várias threads (de vários processos) em seus laços.
     * POR QUE atomic_bool e não bool:
     *   - com bool comum, ler e escrever ao mesmo tempo em threads diferentes
     *     é uma "condição de corrida" (comportamento indefinido em C11), e o
     *     compilador pode guardar o valor num registrador e nunca reler a
     *     memória, fazendo o laço "while (!encerrar)" rodar para sempre;
     *   - com atomic_bool, toda leitura (atomic_load) vai à memória e toda
     *     escrita (atomic_store) é vista por inteiro pelos outros.
     * Não precisa de mutex porque é uma única variável que só vai de false
     * para true (não há várias variáveis que precisam mudar juntas). */
    atomic_bool encerrar;

    /* Anti-colisão: grade de ocupação e monitores das passagens (os
     * mutexes e o POR QUE estão nas structs acima). */
    Ocupacao        ocupacao;
    MonitorPassagem monitor[N_PASSAGENS];

    /* Medição das tarefas periódicas do processo central (ver Medicao).
     * POR QUE mutex: cada tarefa escreve a sua no fim do ciclo enquanto o
     * comando "tempos" (Console ou thread de rede do MQTT) lê ou zera as
     * três; sem ele a tabela poderia sair com n_ciclos de um ciclo e a
     * média de outro (campos double não são atômicos). Um mutex só para as
     * três: a seção crítica é copiar/atualizar 5 números. Fica na memória
     * compartilhada como pedido no projeto, por isso PROCESS_SHARED. */
    pthread_mutex_t mutex_tempos;
    Medicao med_simulador, med_organizador, med_ponte;
} MemoriaCompartilhada;

/* Um atômico só funciona entre PROCESSOS se for "lock-free" (implementado
 * com instruções da CPU, sem trava interna escondida). Valor 2 = sempre
 * lock-free. Se não for, a compilação para aqui. */
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "atomic_bool precisa ser lock-free");

/* ---------- Mensagens das filas ---------- */

/* Tipos de mensagem Organizador/Console -> Navegação do robô. */
typedef enum {
    TAREFA_IR_BAIA,
    ACOPLAR,
    IR_ZONA,
    SOLTAR,
    MODO_MANUAL,
    MODO_AUTO,
    GOTO,
    PAUSAR,      /* equipe com robô em DEFEITO: segura a posição */
    CONTINUAR,   /* defeito resolvido: volta a andar */
    RECARREGAR   /* Organizador: bateria não basta para a caixa, vá recarregar */
} TipoMensagem;

/* Mensagem da fila /atr_robo_<id>. */
typedef struct {
    int tipo;       /* um valor de TipoMensagem */
    int x, y;       /* destino (quando fizer sentido) */
    int carga_id;
    int lider_id;
    /* IR_ZONA: tamanho da equipe (o líder só avança quando os n_membros
     * robôs estão na célula dele, ver "equipe anda em bloco" em navegacao.c). */
    int n_membros;
} MensagemRobo;

/* Tipos de mensagem para o Coletor de Dados. */
typedef enum {
    COLETOR_EVENTO,   /* evento de robô  -> logs/eventos.csv */
    COLETOR_CARGA,    /* carga entregue  -> logs/cargas.csv  */
    COLETOR_FIM       /* pede para a thread Coletor terminar */
} TipoColetor;

/* Mensagem da fila /atr_coletor. O timestamp é tirado por quem envia,
 * no momento em que o evento acontece. */
typedef struct {
    int  tipo;               /* um valor de TipoColetor */
    struct timespec quando;
    int  robo_id;            /* -1 se o evento não é de um robô */
    char texto[128];
} MensagemColetor;

/* ---------- Variáveis globais de cada processo ----------
 * Cada executável (logistica e robo) DEFINE as suas próprias:
 * logistica em main.c e robo em robo_main.c. */
extern MemoriaCompartilhada *mem;  /* ponteiro para /atr_shm já mapeada */
extern mqd_t fila_coletor;         /* fila /atr_coletor já aberta */
extern int id_robo;                /* só no processo robo: o id dele */

/* ---------- memoria.c ---------- */
MemoriaCompartilhada *criar_memoria(void);    /* processo central */
MemoriaCompartilhada *abrir_memoria(void);    /* processo robo */
void fechar_memoria(MemoriaCompartilhada *m); /* só desmapeia */
void destruir_memoria(MemoriaCompartilhada *m); /* desmapeia e remove o nome */

/* ---------- coletor.c ---------- */
void *tarefa_coletor(void *arg);  /* thread (arg não usado) */
void registrar_evento(mqd_t fila, int robo_id, const char *texto);
void registrar_carga(mqd_t fila, const char *texto);
void pedir_fim_coletor(mqd_t fila);
void formatar_data_hora(struct timespec quando, char *saida, int tamanho);

/* ---------- gestor.c ---------- */
void *tarefa_gestor(void *arg);      /* thread (arg não usado) */
bool pedir_novo_robo(int x, int y);  /* produtor: Console e ponte MQTT */
void acordar_gestor(void);           /* usado no encerramento */
void enviar_para_robo(int id, const MensagemRobo *msg); /* fila /atr_robo_<id> */
int pegar_robos_novos(void);         /* Organizador: quantos robôs novos desde a última vez */

/* ---------- organizador.c ---------- */
void *tarefa_organizador(void *arg); /* thread; arg = nome do arquivo de caixas */
bool adicionar_caixa(int id, int tamanho, int baia, int zona); /* id < 0: automático */

/* ---------- ponte_mqtt.c (processo central, Etapa 2) ---------- */
void ponte_criar(void);              /* main, ANTES de criar as threads */
void *tarefa_ponte_mqtt(void *arg);  /* thread (arg não usado) */
void ponte_publicar(const char *topico, const char *texto); /* qualquer thread */
void ponte_destruir(void);           /* main, DEPOIS de todas terminarem */

/* ---------- ocupacao.c (anti-colisão; processo robo e Gestor) ---------- */
bool estacionamento(int x, int y);   /* baia, zona, estação ou posição padrão: aceita vários robôs */
bool estacionamento_ocioso(int x, int y); /* zona, estação ou posição padrão (não baia) */
int  robos_do_grupo(int x, int y, int grupo); /* n da célula se o grupo é "grupo", senão 0 */
bool reservar_celula(int x, int y, int grupo, int *grupo_ocupante);
void liberar_celula(int x, int y);
bool reservar_livre_perto(int *x, int *y, int grupo);  /* Gestor: BFS */
void copiar_ocupadas(int meu_grupo, bool ocupada[]);   /* desvio: células de outros */
bool entrar_passagem(int p, int sentido);
void sair_passagem(int p);
void ler_monitor(int p, int *dentro, int *sentido);    /* A* e ponte MQTT */

/* ---------- tempo_real.c (os dois processos) ---------- */
void ativar_tempo_real(int prioridade, const char *nome); /* só com ATR_TEMPO_REAL=1 */

/* ---------- simulador.c (processo central) ---------- */
void *tarefa_simulador(void *arg);   /* thread (arg não usado) */

/* ---------- Estado interno do processo robo ----------
 * Struct LOCAL do processo robo (não está na memória compartilhada),
 * usada pelas suas duas threads: Navegação e Status.
 * Definida em robo_main.c. */
typedef struct {
    /* POR QUE: Navegação e Status (threads do MESMO processo) leem e
     * escrevem estes campos; o mutex impede que uma leia enquanto a outra
     * escreve. É um mutex comum (não precisa ser PROCESS_SHARED). */
    pthread_mutex_t mutex;
    Estado estado;           /* decidido aqui; a Status publica em e_estado */
    Estado estado_antes;     /* estado a voltar quando o DEFEITO sair */
    bool   parar;            /* Status -> Navegação: DEFEITO, segure a posição */
    bool   ir_recarregar;    /* Status -> Navegação: vá para a estação */
    int    recarga_x, recarga_y;  /* estação escolhida pela Status */
    int    destino_x, destino_y;  /* Navegação -> Status: fim da rota atual
                                   * (-1 = sem rota), publicado no buffer */
    int    n_rota;                /* Navegação -> Status: próximos cantos do caminho */
    int    rota_x[MAX_ROTA_STATUS], rota_y[MAX_ROTA_STATUS];
    bool   bloqueado;             /* Navegação -> Status: esperando célula/passagem */
} EstadoLocal;

extern EstadoLocal estado_local;

/* ---------- status.c e navegacao.c (processo robo) ---------- */
void *tarefa_status(void *arg);      /* thread (arg não usado) */
void *tarefa_navegacao(void *arg);   /* thread (arg não usado) */

/* ---------- console.c ---------- */
#define TAMANHO_RESPOSTA 32768       /* cabe o "status" e o "historico 100" */
void executar_console(void);         /* roda na thread principal */
bool executar_comando(const char *linha, char *resp, int tam); /* Console e ponte MQTT */
const char *nome_estado(Estado e);

#endif
