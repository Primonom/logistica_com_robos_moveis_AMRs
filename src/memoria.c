/*
 * memoria.c — Criação e abertura da memória compartilhada POSIX (/atr_shm).
 *
 * Não cria threads. É usado por dois tipos de processo:
 *   - processo central (logistica): criar_memoria() e destruir_memoria();
 *   - processo de cada robô (robo):  abrir_memoria() e fechar_memoria().
 *
 * Sincronização: inicializa os mutexes que ficam dentro da memória
 * compartilhada com o atributo PTHREAD_PROCESS_SHARED. Sem esse atributo,
 * um mutex só funciona entre threads do MESMO processo; como o processo
 * central e os processos robo acessam os mesmos dados, ele é obrigatório.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "comum.h"

/* Inicializa um mutex que pode ser usado por vários processos. */
static void iniciar_mutex_compartilhado(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t atributo;
    int ret;

    pthread_mutexattr_init(&atributo);
    ret = pthread_mutexattr_setpshared(&atributo, PTHREAD_PROCESS_SHARED);
    if (ret != 0) { fprintf(stderr, "setpshared falhou\n"); exit(1); }
    ret = pthread_mutex_init(mutex, &atributo);
    if (ret != 0) { fprintf(stderr, "pthread_mutex_init falhou\n"); exit(1); }
    pthread_mutexattr_destroy(&atributo);
}

/* Processo central: cria /atr_shm, zera e inicializa os mutexes. */
MemoriaCompartilhada *criar_memoria(void)
{
    MemoriaCompartilhada *m;
    int fd, ret, i;

    /* Remove sobra de uma execução anterior que travou (pode não existir). */
    shm_unlink(NOME_SHM);

    fd = shm_open(NOME_SHM, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) { perror("shm_open"); exit(1); }

    ret = ftruncate(fd, sizeof(MemoriaCompartilhada));
    if (ret == -1) { perror("ftruncate"); exit(1); }

    m = mmap(NULL, sizeof(MemoriaCompartilhada), PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd); /* o mapeamento continua valendo após fechar o descritor */

    memset(m, 0, sizeof(MemoriaCompartilhada));
    for (i = 0; i < MAX_ROBOS; i++) {
        iniciar_mutex_compartilhado(&m->robos[i].mutex);
        m->robos[i].ativo = false;
        m->robos[i].e_estado = DESLIGADO;
        m->robos[i].carga_id = -1;
    }
    iniciar_mutex_compartilhado(&m->mutex_caixas);
    m->n_caixas = 0;
    atomic_init(&m->encerrar, false);
    return m;
}

/* Processo robo: abre a memória já criada pelo processo central.
 * Não inicializa nada (os mutexes já foram inicializados pelo central). */
MemoriaCompartilhada *abrir_memoria(void)
{
    MemoriaCompartilhada *m;
    int fd;

    fd = shm_open(NOME_SHM, O_RDWR, 0);
    if (fd == -1) { perror("shm_open"); exit(1); }

    m = mmap(NULL, sizeof(MemoriaCompartilhada), PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    return m;
}

/* Desfaz o mapeamento (usado pelo robô ao terminar). */
void fechar_memoria(MemoriaCompartilhada *m)
{
    if (munmap(m, sizeof(MemoriaCompartilhada)) == -1) {
        perror("munmap");
        exit(1);
    }
}

/* Processo central: destrói os mutexes, desmapeia e remove /atr_shm.
 * Deve ser chamado só depois que todos os robôs terminaram. */
void destruir_memoria(MemoriaCompartilhada *m)
{
    int i;

    for (i = 0; i < MAX_ROBOS; i++)
        pthread_mutex_destroy(&m->robos[i].mutex);
    pthread_mutex_destroy(&m->mutex_caixas);

    fechar_memoria(m);
    if (shm_unlink(NOME_SHM) == -1) { perror("shm_unlink"); exit(1); }
}
