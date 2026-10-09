# Explicação dos mecanismos de sincronização — Logística com AMRs (Etapa 1)

Este documento explica, para o relatório e a entrevista, **cada mecanismo de
sincronização** do sistema: onde ele está, que problema resolve e o que
aconteceria sem ele. No fim estão as **simplificações assumidas**.

---

## 1. Visão geral: processos e threads

```
Processo central "logistica"                 Processo "robo <id>" (um por robô)
+--------------------------------------+     +------------------------------+
| thread principal: Console (stdin)    |     | thread principal: só cria e  |
| thread Gestor de Robôs               |     |   espera as duas abaixo      |
| thread Organizador da Logística (T3) |     | thread Navegação  (T1=100ms) |
| thread Coletor de Dados              |     | thread Status     (T2=200ms) |
| thread Simulador-stub (100 ms)       |     +------------------------------+
+--------------------------------------+
        |           |            |                    |        |        |
        |   fila /atr_robo_<id>  +----------------->  |        |        |
        |   fila /atr_coletor  <--------------------- (todos enviam eventos)
        |   semáforos /atr_passagem_1..3 <-------------------- (Navegação)
        +---- memória compartilhada /atr_shm  <---- todos os processos ---+
```

| Recurso | Tipo | Criado em | Usado por |
|---|---|---|---|
| `/atr_shm` | memória compartilhada POSIX | `memoria.c: criar_memoria` | todos |
| `Robo.mutex` (um por robô) | mutex `PTHREAD_PROCESS_SHARED` | `memoria.c: iniciar_mutex_compartilhado` | todos |
| `mutex_caixas` | mutex `PTHREAD_PROCESS_SHARED` | `memoria.c` | Organizador, Console |
| `encerrar` | `atomic_bool` | `memoria.c` | todos os laços |
| `/atr_coletor` | fila de mensagens POSIX | `main.c: criar_fila_coletor` | todos → Coletor |
| `/atr_robo_<id>` | fila de mensagens POSIX | `gestor.c: criar_fila_robo` | Organizador/Console → Navegação |
| `/atr_passagem_1..3` | semáforos nomeados contadores | `main.c: criar_semaforos` | Navegação |
| fila de pedidos | fila circular + mutex + variável de condição | `gestor.c` | Console → Gestor |
| `robos_novos` | contador + mutex | `gestor.c` | Gestor → Organizador |
| `estado_local` | struct + mutex comum | `robo_main.c` | Navegação ↔ Status |

**Regra geral contra deadlock:** em nenhum ponto do código uma thread segura
dois mutexes ao mesmo tempo. Cada `pthread_mutex_lock` é seguido do
`unlock` antes do próximo `lock` (conferido em todos os arquivos `src/*.c`).
Sem "segurar um e pedir outro", não existe espera circular entre mutexes.
A única espera com mutex travado é o `pthread_cond_wait` do Gestor, que
**solta** o mutex enquanto dorme.

---

## 2. Mecanismos entre processos (IPC)

### 2.1 Memória compartilhada `/atr_shm`
- **Onde:** `memoria.c` (`criar_memoria` no central, `abrir_memoria` no robô).
  Contém `MemoriaCompartilhada`: os slots `robos[]`, a fila de caixas e `encerrar`.
- **Problema que resolve:** processos diferentes têm memórias separadas. O
  simulador (central) precisa escrever a posição que a Status (processo do
  robô) lê; o Organizador precisa ler o estado que a Status publica.
- **Sem ela:** cada processo veria só a sua cópia dos dados; seria preciso
  trocar tudo por mensagens, a cada ciclo, para cada robô.
- **Detalhe:** o central faz `shm_unlink` antes de criar (remove sobra de uma
  execução interrompida) e `O_EXCL` para garantir que criou um objeto novo.

### 2.2 Mutex por robô (`Robo.mutex`, `PTHREAD_PROCESS_SHARED`)
- **Onde:** dentro de cada slot em `/atr_shm`. Usado no simulador, Status,
  Navegação, Organizador, Console e Gestor.
- **Problema:** o slot é lido e escrito ao mesmo tempo por tarefas de
  processos diferentes. Ex.: o simulador escreve `i_posicao_x` e
  `i_posicao_y` enquanto a Status os lê.
- **Sem ele:** leituras "pela metade" — x novo com y antigo, um buffer de
  status meio atualizado (posição nova com estado antigo), carga_id
  inconsistente. Erros raros e difíceis de reproduzir.
- **Por que `PTHREAD_PROCESS_SHARED`:** um mutex comum só funciona entre
  threads do mesmo processo. Como ele fica na memória compartilhada e é
  usado por vários processos, o atributo é obrigatório.
- **Por que um mutex por robô (e não um só para todos):** tarefas de robôs
  diferentes não se atrapalham (menos espera).

### 2.3 Buffer de status (Tabela 2) com escritor único
- **Onde:** campos `e_posicao_x/y`, `j_sp_posicao_x/y`, `bateria`,
  `e_estado`, `distancia_total` do slot; escritos só por `status.c:
  atualizar_buffer`.
- **Problema:** separar a interface com o hardware (`hw`, usada só pelo robô
  e pelo simulador) da visão que o resto do sistema tem do robô.
- **Regra:** só a thread Status escreve no buffer (cópia do `hw` a cada
  ciclo, tudo sob o mesmo lock). O Organizador e o Console leem **só** o
  buffer, nunca o `hw`.
- **Sem a regra:** cada leitor poderia ver o robô num instante diferente e
  decidir com dados misturados; com um único escritor, o buffer é sempre um
  retrato consistente de um instante.
- **Exceção:** o Gestor preenche os valores iniciais antes de o processo do
  robô existir (ninguém mais está usando o slot).

### 2.4 `mutex_caixas`
- **Onde:** `/atr_shm`. Usado em `organizador.c` (`adicionar_caixa`,
  `primeira_da_fila`, mudanças de status) e `console.c` (`listar_caixas`).
- **Problema:** o Console acrescenta caixas (comando `caixa`) enquanto o
  Organizador percorre e altera a fila.
- **Sem ele:** dois `n_caixas++` simultâneos poderiam perder uma caixa, ou o
  Organizador ler uma caixa ainda não totalmente preenchida.

### 2.5 `atomic_bool encerrar`
- **Onde:** `/atr_shm`. Escrito por `main.c` (`atomic_store`), lido em todos
  os laços (`atomic_load`).
- **Problema:** avisar todas as tarefas, em todos os processos, que devem
  terminar.
- **Sem atomic:** com `bool` comum, ler e escrever ao mesmo tempo é condição
  de corrida (comportamento indefinido em C11) e o compilador pode manter o
  valor num registrador: o `while (!encerrar)` nunca terminaria.
- **Por que não precisa de mutex:** é uma variável só, que só vai de `false`
  para `true`.
- **Entre processos:** só funciona se o atômico for *lock-free*; um
  `_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2)` em `comum.h` garante isso na
  compilação.

### 2.6 Fila de mensagens `/atr_coletor` (produtor-consumidor)
- **Onde:** criada em `main.c`; consumida em `coletor.c: tarefa_coletor`;
  produzida por `registrar_evento`/`registrar_carga` em qualquer thread de
  qualquer processo.
- **Problema:** muitos produtores e um consumidor que grava em disco
  (lento). As tarefas periódicas não podem esperar o disco.
- **Como resolve:** `mq_send`/`mq_receive` são atômicos (mensagens nunca se
  misturam); `mq_receive` bloqueia com a fila vazia (o Coletor dorme sem
  gastar CPU); `mq_send` bloqueia com a fila cheia (10 mensagens).
- **Sem ela:** cada tarefa escreveria nos arquivos, precisando de um mutex
  entre processos para os arquivos e esperando o disco dentro do seu ciclo.
- **Arquivos sem mutex:** só a thread Coletor escreve neles.
- **Timestamp:** tirado por quem envia, no momento do evento (não na hora em
  que o Coletor grava).
- **Fim:** `pedir_fim_coletor` envia `COLETOR_FIM`; como a fila é FIFO, tudo
  o que foi enviado antes ainda é gravado.

### 2.7 Filas de mensagens `/atr_robo_<id>`
- **Onde:** criadas pelo Gestor; enviadas por `gestor.c: enviar_para_robo`
  (usada pelo Organizador e pelo Console); lidas em `navegacao.c: ler_mensagens`.
- **Problema:** mandar comandos (`TAREFA_IR_BAIA`, `ACOPLAR`, `IR_ZONA`,
  `SOLTAR`, `PAUSAR`, `CONTINUAR`, `GOTO`, `MODO_MANUAL/AUTO`) de outro
  processo, sem perder nenhum e na ordem certa.
- **Sem ela:** seria preciso um "campo de comando" na memória compartilhada;
  um segundo comando poderia sobrescrever o primeiro antes de ser lido.
- **Leitura sem bloquear o ciclo:** `mq_timedreceive` com prazo = agora;
  com a fila vazia retorna na hora com `ETIMEDOUT` e o ciclo de 100 ms segue.

### 2.8 Semáforos contadores nomeados `/atr_passagem_1..3`
- **Onde:** criados em `main.c: criar_semaforos` com valor inicial = capacidade
  (P1 = 3, P2 = 2, P3 = 1); usados em `navegacao.c` (`tentar_entrar`,
  `sem_post` em `seguir_rota`).
- **Problema:** limitar quantos robôs ficam dentro de cada passagem estreita.
- **Como:** antes de seguir para o waypoint depois da passagem, `sem_timedwait`;
  ao chegar nele (já fora), `sem_post`.
- **Sem eles:** mais robôs que a capacidade dentro da passagem (colisão).
- **Por que `sem_timedwait` com prazo curto (10 ms):** se a passagem está
  cheia, o robô desiste **neste ciclo** e tenta no próximo. A thread não fica
  presa e continua lendo mensagens (`PAUSAR`) e a flag `parar` (defeito).
- **Contra deadlock:** um robô nunca segura duas passagens ao mesmo tempo
  (libera uma antes de pedir a próxima) — quebra a condição "segura e
  espera". Ex.: de (45,1) a (45,28) ele libera P3 em y = 14 e pede de novo em y = 16.
- **Equipe = uma vaga:** no transporte só o líder faz `sem_timedwait`/`sem_post`;
  os seguidores só copiam o setpoint dele.
- **Robô em DEFEITO dentro da passagem continua ocupando a vaga** (não faz
  `sem_post`), porque fisicamente está bloqueando o caminho. Os outros
  esperam no semáforo (cenário testado: líder parado em P2, um robô passa
  na vaga que sobrou e o outro espera).

### 2.9 `fork()` seguido imediatamente de `execl()`
- **Onde:** `gestor.c: criar_processo_robo`.
- **Problema:** o processo central tem várias threads. Depois de `fork`, só a
  thread que chamou existe no filho; um mutex que outra thread segurava no
  momento ficaria travado para sempre no filho.
- **Como resolve:** o `exec` troca todo o programa; o filho começa limpo e
  abre o IPC pelo nome. Se o `exec` falhar, `_exit(1)` (não roda nada
  herdado do central).

---

## 3. Mecanismos dentro do processo central

### 3.1 Fila circular + mutex + variável de condição (pedidos de novo robô)
- **Onde:** `gestor.c` (`pedidos[]`, `mutex_pedidos`, `cond_pedidos`).
  Produtor: `pedir_novo_robo` (Console). Consumidor: `tarefa_gestor`.
- **Problema:** produtor-consumidor clássico entre duas threads.
- **Mutex:** sem ele, Console e Gestor alterariam `inicio`/`quantidade` ao
  mesmo tempo (pedido perdido ou lido duas vezes).
- **Variável de condição:** o Gestor **dorme** com a fila vazia, sem gastar
  CPU. Sem ela, o Gestor teria de ficar testando a fila em laço (espera ocupada).
- **`while` e não `if`:** ao acordar, a condição é testada de novo (o sinal
  pode ser de encerramento ou um despertar espúrio).
- **Encerramento sem aviso perdido:** `acordar_gestor` faz o `broadcast` com o
  mutex travado; assim ele não cai entre o "testa a condição" e o "dorme" do Gestor.
- **Fila cheia:** o pedido é recusado (Console avisa), em vez de travar o Console.
- **Trabalho fora do mutex:** criar o processo (`fork`/`exec`) é feito depois
  de soltar o mutex, para o Console não esperar.

### 3.2 Contador `robos_novos` + `mutex_novos` (aviso assíncrono)
- **Onde:** `gestor.c` (incrementa) e `pegar_robos_novos` (Organizador lê e zera).
- **Problema:** o Gestor precisa avisar o Organizador sem que um espere pelo outro.
- **Sem o mutex:** `robos_novos++` e "ler e zerar" simultâneos perderiam avisos.

### 3.3 `pthread_join` e `waitpid` no encerramento
- **Onde:** `main.c`, depois que o Console retorna (`sair` ou fim da entrada).
- **Ordem:** `encerrar = true` → acorda e espera o Gestor (não cria robô
  durante o fim) → espera Organizador e Simulador → `SIGTERM` + `waitpid` em
  cada robô → Coletor grava o que falta e termina → `mq_unlink`,
  `sem_unlink`, `shm_unlink`.
- **Sem isso:** destruir mutexes ou desmapear a memória enquanto alguém ainda
  a usa; processos zumbis; eventos finais perdidos.

---

## 4. Mecanismos dentro do processo `robo`

### 4.1 `estado_local` + mutex comum (Navegação ↔ Status)
- **Onde:** definido em `robo_main.c`; usado em `navegacao.c` e `status.c`.
- **Conteúdo:** `estado`, `estado_antes`, flags `parar` e `ir_recarregar`,
  estação de recarga escolhida.
- **Problema:** as duas threads do robô trocam informação: a Navegação decide
  estados (DESLOCANDO, AGUARDANDO_EQUIPE...), a Status decide DEFEITO e
  RECARREGANDO e avisa a Navegação pelas flags.
- **Por que mutex comum:** as duas threads são do mesmo processo.
- **"Testar e mudar" juntos:** em `navegacao.c: definir_estado`, se o robô
  está em DEFEITO, o novo estado vai para `estado_antes` (vale quando o
  defeito sair). Teste e escrita ficam sob o mesmo lock; sem isso, a
  Navegação poderia apagar um DEFEITO que a Status acabou de colocar.

### 4.2 Flags `parar` e `ir_recarregar`
- `parar` (Status → Navegação): com `i_falha` ou bateria 0%, a Navegação só
  mantém o setpoint na posição atual. Sem a flag, a Navegação continuaria
  mandando o robô andar.
- `ir_recarregar` (Status → Navegação): robô `DISPONIVEL`, sem carga e com
  bateria < 20% vai para a estação mais próxima. A Navegação lê e zera a
  flag sob o mutex (o pedido é atendido uma vez só).

---

## 5. Tarefas periódicas sem deriva

- **Onde:** laços da Navegação (100 ms), Status (200 ms), Organizador
  (500 ms) e Simulador (100 ms); função `somar_ms` em `comum.h`.
- **Como:** cada tarefa guarda o instante da **próxima** ativação e dorme até
  ele com `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`.
- **Sem tempo absoluto:** com `sleep(periodo)` relativo, o tempo gasto no
  trabalho de cada ciclo somaria ao período e o atraso acumularia (deriva).
- **`CLOCK_MONOTONIC`:** não pula se o relógio do sistema for ajustado.
- (`mq_timedreceive` e `sem_timedwait` usam `CLOCK_REALTIME` porque a
  interface POSIX deles exige.)

---

## 6. Regras do Organizador (coordenação)

### 6.1 Acoplamento síncrono
- Cada robô, ao chegar na baia, muda para `AGUARDANDO_EQUIPE`. Quando o
  Organizador vê **todos** da equipe nesse estado, envia `ACOPLAR` a todos no
  **mesmo ciclo**. Cada robô liga `o_acoplar_e_erguer` e, quando o hardware
  confirma `i_carga_atracada`, muda para `OPERANDO_CARGA`. Só com **todos**
  em `OPERANDO_CARGA` o Organizador envia `IR_ZONA`.
- Funciona como uma **barreira** coordenada pelo Organizador: ninguém ergue
  sozinho e ninguém sai antes de todos erguerem.

### 6.2 Sem equipe parcial (evita deadlock)
- A caixa só é atribuída quando **todos** os robôs necessários estão
  `DISPONIVEL`, automáticos e com bateria >= 30% no mesmo ciclo.
- **Sem a regra:** reservando 2 robôs para uma caixa de 3 e outros 2 para
  outra caixa de 3, as duas esperariam para sempre (cada uma segura parte e
  espera o resto) — "segura e espera" + espera circular. Sem reserva
  parcial, a condição "segura e espera" não existe.

### 6.3 Ordem de chegada (evita inanição)
- A caixa considerada é a **primeira da fila com a baia livre**. Se ela não
  tem equipe completa, a fila **para**.
- **Sem parar a fila:** caixas pequenas passariam sempre na frente e os robôs
  talvez nunca ficassem livres ao mesmo tempo em número suficiente para uma
  caixa de tamanho 4 (inanição). Cenário 4 mostra a caixa de tamanho 4
  esperando e sendo atendida quando o 4º robô aparece.
- **Pular a baia ocupada não causa inanição:** a baia libera quando a entrega
  em andamento termina (tempo limitado); a busca é em ordem de chegada, então
  nenhuma caixa mais nova da mesma baia passa na frente; quando a baia libera,
  a caixa pulada é a primeira candidata.

### 6.4 Bateria: limites 30% e 20%
- O Organizador só escolhe robôs com >= 30%; a Status só manda recarregar com
  < 20% e sem carga. Como 30 > 20, um robô recém-escolhido nunca é mandado
  recarregar ao mesmo tempo (não há corrida entre as duas decisões).
- A recarga só é disparada para robô **sem carga**: no cenário 3 o líder fica
  com 25% no meio do transporte, entrega a caixa e só depois vai recarregar.

### 6.5 PAUSAR / CONTINUAR
- Se qualquer membro da equipe está em `DEFEITO`, o Organizador envia
  `PAUSAR` a todos (seguram a posição) e a equipe não muda de fase; quando
  nenhum está mais em `DEFEITO`, envia `CONTINUAR`.
- **Sem isso:** o líder seguiria viagem e a caixa (carregada por todos) seria
  "arrastada" deixando um robô para trás.

---

## 7. Simplificações assumidas (citar no relatório)

**Mapa e movimento**
1. Colisão fora das passagens **não** é tratada: robôs podem ocupar a mesma célula.
2. P1 e P2 são tratadas como colunas inteiras (x 14–16 e 39–41), como
   "paredes com porta": nenhum trecho vertical entra nelas. Posições dentro
   das passagens são recusadas por `novo` e `goto`.
3. P3 usa **um** semáforo para as duas metades (acima e abaixo do corredor):
   um robô indo à estação de cima bloqueia outro indo à de baixo.
4. Rota sempre pelo corredor `y = 15`, em trechos só horizontais ou só verticais.
5. Há uma pausa de um ciclo da Navegação (100 ms) em cada waypoint.

**Equipes**
6. A equipe ocupa a **mesma célula** (seguidores copiam o setpoint do líder sem deslocamento).
7. Os seguidores leem o setpoint do líder pelo buffer de status (atualizado a
   cada 200 ms) e andam até ~2 células atrás. Por isso o Organizador espera
   **todos** na zona antes do `SOLTAR`, e um seguidor pode estar dentro de
   uma passagem por alguns instantes depois que o líder já a liberou (ou
   parar lá dentro, em DEFEITO, sem segurar o semáforo — visto no cenário 3).
8. Uma carga por baia de cada vez.
9. Se um robô de uma equipe fica em DEFEITO para sempre, a equipe fica
   pausada para sempre (não há reatribuição da caixa).

**Robôs**
10. `goto` só funciona em modo manual; `manual` só é aceito com o robô `DISPONIVEL`.
11. Um `goto` recebido enquanto o robô está dentro de uma passagem só começa depois que ele sai.
12. Robô em modo manual não vai recarregar sozinho (a recarga automática só
    parte de `DISPONIVEL`); com 0% ele entra em DEFEITO.
13. O estado visto por fora (buffer) tem até 200 ms de atraso em relação ao
    estado interno do robô (ex.: logo depois do `SOLTAR` ainda aparece `OPERANDO_CARGA`).
14. A morte inesperada de um processo `robo` não é detectada (o slot continua ativo).
15. Na disputa por uma passagem cheia não há fila de ordem: quem conseguir o
    semáforo primeiro entra (o `sem_timedwait` tenta de novo a cada ciclo).

**Simulador-stub**
16. 1 célula por tick (100 ms); bateria cai 1% a cada 5 células e sobe 2% por
    tick na estação; `i_carga_atracada` segue `o_acoplar_e_erguer` no tick seguinte.

**Infraestrutura**
17. `mq_send` bloqueia se a fila estiver cheia (10 mensagens): uma tarefa
    poderia esperar o Coletor ou a Navegação de um robô esvaziar a fila.
18. Fila de pedidos de novo robô cheia (8) → pedido recusado.
19. Ctrl+C usa o comportamento padrão do `SIGINT` (mata tudo, sem limpeza);
    as sobras de IPC são removidas na próxima inicialização.

---

## 8. Cenários de teste

Rodar todos (salva em `testes/saidas/`):

```
testes/rodar_todos.sh
```

Ou um só: `./logistica testes/sem_caixas.txt < testes/cenarioN.txt`
(`sem_caixas.txt` é vazio, para as caixas de `config/caixas.txt` não
interferirem; cada cenário coloca as suas com o comando `caixa`).

| Cenário | O que testa |
|---|---|
| 1 | 1 robô, caixa de tamanho 1: ciclo completo baia → zona, passagens P1 e P2. |
| 2 | 4 robôs, caixa de tamanho 3: escolha dos mais próximos, `AGUARDANDO_EQUIPE`, `ACOPLAR` síncrono, só o líder usa passagens. |
| 3 | Falha e bateria baixa no meio do transporte: `PAUSAR`/`CONTINUAR`, recarga só depois de soltar a carga. |
| 4 | Caixa de tamanho 4 com 3 robôs: espera sem reservar ninguém, a fila para, o sistema continua respondendo; atendida quando o 4º robô é criado. |

Os cenários usam `aguarda <id> <ESTADO> [timeout_s]` (espera o robô chegar a
um estado, lendo o buffer de status a cada 100 ms) em vez de `espera` sempre
que o teste depende de um momento específico. Assim a sequência de decisões
é sempre a mesma; mudam só horários, pids e a ordem de impressão de linhas
de threads diferentes no mesmo instante.
