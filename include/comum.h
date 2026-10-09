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
 */
#ifndef COMUM_H
#define COMUM_H

#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include <mqueue.h>

/* ---------- Limites ---------- */
#define MAX_ROBOS  16
#define MAX_CAIXAS 64
#define MAX_ROTA_STATUS 8     /* waypoints da rota publicados no buffer de status */

/* ---------- Períodos das tarefas periódicas (em milissegundos) ---------- */
#define PERIODO_NAVEGACAO_MS    100   /* T1 */
#define PERIODO_STATUS_MS       200   /* T2 */
#define PERIODO_ORGANIZADOR_MS  500   /* T3 */
#define PERIODO_SIMULADOR_MS    100   /* simulador-stub (Etapa 1) */
#define PERIODO_PONTE_MS        200   /* ponte MQTT: publica atr/status */

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
    int      n_rota;                /* próximos waypoints da rota (até 8): */
    int      rota_x[MAX_ROTA_STATUS], rota_y[MAX_ROTA_STATUS]; /* a tela desenha o caminho */

    bool     modo_automatico;
    int      carga_id;         /* -1 se nenhuma */
} Robo;

/* Situação de uma caixa. */
typedef enum {
    CAIXA_ESPERANDO,     /* na fila, sem equipe */
    CAIXA_EM_ANDAMENTO,  /* equipe formada, sendo levada */
    CAIXA_ENTREGUE
} StatusCaixa;

#define TAMANHO_MAX_CAIXA 4   /* = máximo de robôs numa equipe */

/* Limites de bateria (em %). Como 30 > 20, um robô que o Organizador
 * acabou de escolher nunca está, ao mesmo tempo, sendo mandado recarregar. */
#define BATERIA_MINIMA_TAREFA 30  /* Organizador só escolhe robô com >= 30% */
#define BATERIA_RECARREGAR    20  /* Status manda recarregar com < 20% */

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

/* Conteúdo completo de /atr_shm. */
typedef struct {
    Robo robos[MAX_ROBOS];

    /* POR QUE: a lista de caixas é alterada pelo Console (nova caixa) e lida/
     * alterada pelo Organizador (escolhe e marca caixas). O mutex evita que
     * as duas threads mexam em n_caixas/caixas[] ao mesmo tempo.
     * É PROCESS_SHARED por estar na memória compartilhada (e por coerência
     * com os mutexes dos robôs). */
    pthread_mutex_t mutex_caixas;
    Caixa caixas[MAX_CAIXAS];  /* em ORDEM DE CHEGADA (a fila do Organizador) */
    int   n_caixas;

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
    CONTINUAR    /* defeito resolvido: volta a andar */
} TipoMensagem;

/* Mensagem da fila /atr_robo_<id>. */
typedef struct {
    int tipo;       /* um valor de TipoMensagem */
    int x, y;       /* destino (quando fizer sentido) */
    int carga_id;
    int lider_id;
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
bool pedir_novo_robo(int x, int y);  /* produtor: chamado pelo Console */
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

/* ---------- sim_stub.c (processo central, só Etapa 1) ---------- */
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
    int    n_rota;                /* Navegação -> Status: próximos waypoints */
    int    rota_x[MAX_ROTA_STATUS], rota_y[MAX_ROTA_STATUS];
} EstadoLocal;

extern EstadoLocal estado_local;

/* ---------- status.c e navegacao.c (processo robo) ---------- */
void *tarefa_status(void *arg);      /* thread (arg não usado) */
void *tarefa_navegacao(void *arg);   /* thread (arg não usado) */

/* ---------- console.c ---------- */
#define TAMANHO_RESPOSTA 8192        /* cabe a tabela do "status" inteira */
void executar_console(void);         /* roda na thread principal */
bool executar_comando(const char *linha, char *resp, int tam); /* Console e ponte MQTT */
const char *nome_estado(Estado e);

#endif
