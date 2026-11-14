# Explicação dos mecanismos de sincronização — Logística com AMRs

Este documento explica, para o relatório e a entrevista, **cada mecanismo de
sincronização** do sistema: onde ele está, que problema resolve e o que
aconteceria sem ele. A seção 7 explica o **ponto extra** (mapa aleatório e
IA de rotas com A*). No fim estão as **simplificações assumidas**.

---

## 1. Visão geral: processos e threads

```
Processo central "logistica"                 Processo "robo <id>" (um por robô)
+--------------------------------------+     +------------------------------+
| thread principal: Console (stdin)    |     | thread principal: só cria e  |
| thread Gestor de Robôs               |     |   espera as duas abaixo      |
| thread Organizador da Logística (T3) |     | thread Navegação  (T1=100ms) |
| thread Coletor de Dados              |     | thread Status     (T2=200ms) |
| thread Simulador do hardware (100 ms)|     +------------------------------+
| thread Ponte MQTT (200 ms) + thread  |
|   de rede da libmosquitto            |
+--------------------------------------+
        |           |            |                    |        |        |
        |   fila /atr_robo_<id>  +----------------->  |        |        |
        |   fila /atr_coletor  <--------------------- (todos enviam eventos)
        |   semáforos /atr_passagem_1..4 <-------------------- (Navegação)
        +---- memória compartilhada /atr_shm  <---- todos os processos ---+
        |
   MQTT (broker mosquitto) <----> Tela de Operação em Python (NiceGUI),
                                  que também é a tela do simulador
```

| Recurso | Tipo | Criado em | Usado por |
|---|---|---|---|
| `/atr_shm` | memória compartilhada POSIX | `memoria.c: criar_memoria` | todos |
| `Robo.mutex` (um por robô) | mutex `PTHREAD_PROCESS_SHARED` | `memoria.c: iniciar_mutex_compartilhado` | todos |
| `mutex_caixas` | mutex `PTHREAD_PROCESS_SHARED` | `memoria.c` | Organizador, Console |
| `encerrar` | `atomic_bool` | `memoria.c` | todos os laços |
| `/atr_coletor` | fila de mensagens POSIX | `main.c: criar_fila_coletor` | todos → Coletor |
| `/atr_robo_<id>` | fila de mensagens POSIX | `gestor.c: criar_fila_robo` | Organizador/Console → Navegação |
| `/atr_passagem_1..4` | semáforos nomeados contadores | `main.c: criar_semaforos` | Navegação (entra/sai); A* e Ponte (só `sem_getvalue`) |
| `mem->mapa` | dados só de leitura (sem mutex) | `mapa.c: gerar_mapa` (no `main`, antes das threads) | todos (A*, validação de posição, tela) |
| fila de pedidos | fila circular + mutex + variável de condição | `gestor.c` | Console / Ponte MQTT → Gestor |
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
  Contém `MemoriaCompartilhada`: os slots `robos[]`, o `mapa` desta execução,
  a fila de caixas e `encerrar`.
- **Problema que resolve:** processos diferentes têm memórias separadas. O
  simulador (central) precisa escrever a posição que a Status (processo do
  robô) lê; o Organizador precisa ler o estado que a Status publica.
- **Sem ela:** cada processo veria só a sua cópia dos dados; seria preciso
  trocar tudo por mensagens, a cada ciclo, para cada robô.
- **Detalhe:** o central faz `shm_unlink` antes de criar (remove sobra de uma
  execução interrompida) e `O_EXCL` para garantir que criou um objeto novo.

### 2.1.1 O mapa na memória compartilhada, **sem mutex**
- **Onde:** `mem->mapa` (struct `Mapa`, `mapa.h`): tipo de cada célula
  (`LIVRE`, `PAREDE`, `OBSTACULO`), índice da passagem de cada célula (ou -1)
  e os dados das 4 passagens (capacidade, parede, y, x das bocas).
- **Problema:** todos precisam do mesmo mapa sorteado: o Organizador (A* para
  escolher robôs), a Navegação de cada robô (A* e passagens), a Status
  (estação mais próxima), o Console/ponte (validar posições) e a tela.
- **Como:** `gerar_mapa` escreve o mapa **uma vez**, na thread principal do
  central, **antes** de qualquer `pthread_create` e de qualquer `fork/exec`
  de robô. Depois disso **ninguém escreve** nele.
- **Por que não precisa de mutex:** condição de corrida só existe quando
  alguém escreve enquanto outro lê. Aqui, depois de criado, só há leituras.
  E `pthread_create`/`fork` garantem que a thread ou o processo novo enxerga
  tudo o que foi escrito antes da criação.
- **Sem essa ordem** (se o mapa fosse gerado com as threads já rodando): um
  robô poderia ler o mapa pela metade (paredes novas, passagens antigas).
  Aí seria preciso um mutex (ou um sinal de "mapa pronto").

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
- **Disponível para a operação remota:** depois de gravar, o Coletor publica
  o registro em `atr/eventos` (ao vivo). O que já está em disco é lido pelo
  comando `historico [n]` (ver 2.6.1).

#### 2.6.1 Leitura do histórico (`historico [n]`, sem mutex)
- **Onde:** `console.c: comando_historico` / `anexar_ultimas_linhas`.
  Chamado pelo Console ou pela Tela (botão "Carregar histórico do disco" no
  painel Eventos → `atr/comando` → ponte MQTT).
- **Problema:** o enunciado pede que os registros sejam armazenados em disco
  **e** disponibilizados para a operação remota. A tela só recebe ao vivo o
  que acontece enquanto ela está aberta.
- **Como:** abre `logs/eventos.csv` e `logs/cargas.csv` em modo `"r"` (um
  `FILE` próprio), guarda as linhas num vetor circular local
  (`ultimas[k % n]`) e devolve as últimas n (padrão 30, máximo 100) como texto.
- **Por que não precisa de mutex:** o Coletor é o único escritor, só
  **acrescenta** linhas no fim e dá `fflush` depois de cada uma; nada já
  escrito muda. No pior caso a leitura pega a última linha pela metade (sem
  `\n`), e ela é descartada. Os vetores são locais (variáveis de pilha),
  então Console e ponte podem chamar ao mesmo tempo.
- **Sem esse cuidado:** mostrar uma linha cortada; com vetores `static`,
  duas chamadas simultâneas misturariam as respostas.

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

### 2.8 Semáforos contadores nomeados `/atr_passagem_1..4`
- **Onde:** criados em `main.c: criar_semaforos` com valor inicial = a
  capacidade **sorteada** de cada passagem (as capacidades 1, 2, 3 e 4,
  embaralhadas a cada execução); usados em `navegacao.c` (`tentar_entrar`,
  `liberar_se_saiu`). Abertos uma vez por processo
  (`abrir_semaforos_passagens`, antes das threads).
- **Problema:** limitar quantos robôs ficam dentro de cada passagem. A
  **região** da passagem = o túnel de 4 células que atravessa a parede + 1
  célula de "boca" de cada lado.
- **Como:** o robô anda uma célula por vez. Quando a **próxima** célula é da
  região de `p` e a atual não, faz `sem_timedwait` **antes** de mandar o robô
  entrar; quando ele chega numa célula fora da região, `sem_post`.
- **Sem eles:** mais robôs que a capacidade dentro da passagem (colisão).
- **Por que `sem_timedwait` com prazo curto (10 ms):** se a passagem está
  cheia, o robô desiste **neste ciclo** e tenta no próximo. A thread não fica
  presa e continua lendo mensagens (`PAUSAR`) e a flag `parar` (defeito).
- **Contra deadlock:** um robô nunca segura duas passagens ao mesmo tempo. As
  regiões ficam separadas por mais de 1 célula (passagens de paredes
  diferentes têm 9 colunas entre si; na mesma parede, 5 ou mais linhas),
  então entre uma região e outra sempre há uma célula livre, onde ele faz o
  `sem_post`. E se ainda segura uma (equipe saindo), ele **não pede** a
  próxima: espera. Isso quebra a condição "segura e espera".
- **Equipe = uma vaga:** no transporte só o líder faz `sem_timedwait`/`sem_post`.
  Ele só faz `sem_post` quando **ele e todos os membros** já saíram da região
  (ver 7.6).
- **Robô em DEFEITO dentro da passagem continua ocupando a vaga** (não faz
  `sem_post`), porque fisicamente está bloqueando o caminho. Os outros
  esperam no semáforo, e depois de 3 s replanejam (ver 7.5; cenário 6).
- **`sem_getvalue` (leitura sem esperar):** o A* lê quantas vagas estão livres
  para calcular o custo dinâmico, e a ponte MQTT para exibir. É só uma
  estimativa do instante: pode mudar logo depois. Nada de segurança depende
  dela; quem garante a capacidade é o `sem_timedwait`.

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
  Produtores: `pedir_novo_robo`, chamado pelo Console (comando `novo`, nos
  cenários de teste) e pela ponte MQTT (tópico `atr/robo/novo`, publicado
  pelo simulador). Consumidor: `tarefa_gestor`.
- **Problema:** produtor-consumidor clássico (dois produtores, um consumidor).
- **Mutex:** sem ele, Console, ponte e Gestor alterariam `inicio`/`quantidade`
  ao mesmo tempo (pedido perdido ou lido duas vezes).
- **Variável de condição:** o Gestor **dorme** com a fila vazia, sem gastar
  CPU. Sem ela, o Gestor teria de ficar testando a fila em laço (espera ocupada).
- **`while` e não `if`:** ao acordar, a condição é testada de novo (o sinal
  pode ser de encerramento ou um despertar espúrio).
- **Encerramento sem aviso perdido:** `acordar_gestor` faz o `broadcast` com o
  mutex travado; assim ele não cai entre o "testa a condição" e o "dorme" do Gestor.
- **Fila cheia:** o pedido é recusado (Console avisa; a ponte responde
  `erro:` em `atr/comando/resposta`), em vez de travar quem pediu.
- **Trabalho fora do mutex:** criar o processo (`fork`/`exec`) é feito depois
  de soltar o mutex, para o Console não esperar.

### 3.2 Contador `robos_novos` + `mutex_novos` (aviso assíncrono)
- **Onde:** `gestor.c` (incrementa) e `pegar_robos_novos` (Organizador lê e zera).
- **Problema:** o Gestor precisa avisar o Organizador sem que um espere pelo outro.
- **Sem o mutex:** `robos_novos++` e "ler e zerar" simultâneos perderiam avisos.

### 3.3 `pthread_join` e `waitpid` no encerramento
- **Onde:** `main.c`, depois que o Console retorna (`sair` ou fim da entrada).
- **Ordem:** `encerrar = true` → acorda e espera o Gestor (não cria robô
  durante o fim) → espera Organizador, Simulador e Ponte MQTT → `SIGTERM` + `waitpid` em
  cada robô → Coletor grava o que falta e termina → `mq_unlink`,
  `sem_unlink`, `shm_unlink`.
- **Sem isso:** destruir mutexes ou desmapear a memória enquanto alguém ainda
  a usa; processos zumbis; eventos finais perdidos.

### 3.4 Simulador do hardware (`simulador.c`)
- **Onde:** `simulador.c: tarefa_simulador`, uma thread do processo central,
  período 100 ms (`PERIODO_SIMULADOR_MS`).
- **O que faz (item 8 do enunciado):** lê o setpoint que a Navegação escreve
  no `hw` (`o_sp_posicao_x/y`) e anda 1 célula por ciclo em direção a ele
  (`i_posicao_x/y`); gasta bateria ao andar e recarrega na estação; a garra
  (`i_carga_atracada`) segue `o_acoplar_e_erguer`. Com `i_falha` ou 0% o
  robô não anda.
- **Interface gráfica do simulador (itens 8a–8d):** é o painel "Simulação"
  da Tela de Operação (o enunciado permite que seja a mesma tela). Gerar
  defeito, baixar a bateria e incluir caixas viram comandos em `atr/comando`
  (`falha`, `ok`, `bateria`, `caixa`); um novo robô é publicado em
  `atr/robo/novo` (ver 3.5).
- **Problema:** o simulador escreve no `hw` de cada robô enquanto a Status e
  a Navegação (outro processo) leem e escrevem nos mesmos campos.
- **Como:** trava o `Robo.mutex` (PROCESS_SHARED) de cada robô durante o
  ciclo daquele robô; `clock_nanosleep` absoluto para o período.
  `celulas_andadas[]` só é usado por esta thread (sem mutex).
- **Sem o mutex:** a Status poderia ler x novo com y antigo, ou a bateria no
  meio de uma atualização.
- **Por que em C, no processo central:** usa a memória compartilhada
  diretamente, sem traduzir o `hw` de cada robô em mensagens MQTT a cada 100 ms.

### 3.5 Ponte MQTT (`ponte_mqtt.c`)
- **Onde:** `tarefa_ponte_mqtt` (200 ms, publica `atr/status`) e a thread de
  rede da libmosquitto (callbacks `ao_conectar`, `ao_receber`, `ao_desconectar`).
- **Tópicos:** publica `atr/mapa` (retained: largura, altura, semente,
  baias, zonas, estações, paredes como retângulos, obstáculos `[x,y]` e as
  passagens com nome, capacidade, células do túnel e região), `atr/status`,
  `atr/comando/resposta`; o Coletor publica `atr/eventos`; assina
  `atr/comando` (linha igual à do Console) e `atr/robo/novo` (novo robô).
- **Leitura do status:** copia cada slot com o `Robo.mutex` para uma variável
  local e monta o JSON fora do mutex, lendo só o **buffer de status** (nunca
  o `hw`); as caixas são copiadas de uma vez com `mutex_caixas`. Sem a cópia,
  o robô ficaria travado enquanto o texto é montado.
- **Comandos (`atr/comando`):** `executar_comando()` não tem variáveis
  `static` que mudem e só usa os mecanismos já existentes, então pode rodar
  na thread de rede ao mesmo tempo que o Console.
- **Novo robô (`atr/robo/novo`):** o enunciado diz que cada novo robô envia
  uma mensagem via MQTT (simulador) para o Gestor de Robôs. O payload é
  `"x y"` ou vazio (posição padrão). A ponte valida com `posicao_valida` e
  chama `pedir_novo_robo` — a mesma fila de pedidos do comando `novo` (3.1):
  a thread de rede é só mais um produtor. O resultado (`ok` ou `erro: ...`)
  vai para `atr/comando/resposta`. Sem a fila, a thread de rede teria de
  fazer `fork`/`exec` ela mesma e ficaria parada durante a criação.
- **`sem_getvalue`:** leitura instantânea da ocupação das passagens só para
  exibir; o valor pode mudar logo depois, mas nada é decidido com ele.
- **`mem->mapa`:** só leitura (2.1.1). A tela desenha tudo a partir do
  `atr/mapa`; nada do mapa é fixo no Python.
- **Broker fora do ar:** a ponte tenta a primeira conexão a cada 2 s; o resto
  do sistema não depende dela (publicações sem conexão são descartadas).

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
  bateria < 20% vai para a estação de **menor caminho A*** (não a mais perto
  em linha reta: pode haver parede no meio). A Navegação lê e zera a
  flag sob o mutex (o pedido é atendido uma vez só).

---

## 5. Tarefas periódicas sem deriva

- **Onde:** laços da Navegação (100 ms), Status (200 ms), Organizador
  (500 ms), Simulador (100 ms) e Ponte MQTT (200 ms); função `somar_ms` em `comum.h`.
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
  com 28% no começo do transporte (cerca de 23% no meio), entrega a caixa e só depois vai recarregar.

### 6.5 PAUSAR / CONTINUAR
- Se qualquer membro da equipe está em `DEFEITO`, o Organizador envia
  `PAUSAR` a todos (seguram a posição) e a equipe não muda de fase; quando
  nenhum está mais em `DEFEITO`, envia `CONTINUAR`.
- **Sem isso:** o líder seguiria viagem e a caixa (carregada por todos) seria
  "arrastada" deixando um robô para trás.

---

## 7. Ponto extra +3: mapa aleatório e IA

O enunciado pede "um sistema de Inteligência Artificial que faça a
otimização das rotas de cada robô e de cada carga, com um mapa com
obstáculos aleatórios a cada nova execução, incluindo pontos de passagem
limitada (apenas 1, 2, 3 ou 4 robôs por vez)". A solução tem três partes:
o **mapa sorteado** (`mapa.c: gerar_mapa`), o **A*** para os caminhos
(`mapa.c: calcular_caminho`) e o uso do A* nas **decisões** (Navegação e
Organizador).

### 7.1 Geração do mapa (`gerar_mapa`)
- **Semente:** variável de ambiente `ATR_SEMENTE` (inteiro); sem ela,
  `time(NULL)`. O programa imprime "Mapa gerado com semente N" (e as 4
  passagens) e registra o evento no Coletor. Mesma semente = mesmo mapa
  (`srand(semente)`), por isso os testes e o vídeo usam sementes fixas.
- **Fixo:** grade 60 x 30; baias em x = 3 e zonas em x = 56 (y = 4, 11, 18,
  25); estações (22, 1) e (37, 28); posição padrão do robô novo (8, 15);
  3 paredes verticais de 4 colunas: x = 13..16, 28..31 e 43..46.
- **Sorteado:**
  1. capacidades {1, 2, 3, 4} embaralhadas (Fisher–Yates);
  2. uma parede sorteada recebe 2 passagens e as outras 1 (toda parede
     precisa de pelo menos uma, senão o galpão ficaria dividido);
  3. o y de cada passagem em 2..27, com 5 ou mais linhas entre duas
     passagens da mesma parede; o túnel abre as 4 células da parede naquela
     linha, e a região inclui uma boca de cada lado;
  4. 45 obstáculos de 1 célula em células livres, **proibidos** em paredes,
     regiões de passagem, num quadrado de raio 2 em volta das bocas (para a
     porta nunca ficar bloqueada), baias, zonas, estações, posição padrão e
     na linha y = 15 inteira (corredor principal sempre largo).
- **Validação com BFS (busca em largura):** a partir de cada baia, visita
  todas as células livres alcançáveis (fila simples num vetor). Se alguma
  zona, estação ou a posição padrão não foi visitada, o mapa é descartado e
  sorteado de novo (até 100 tentativas).
- **Onde fica:** `mem->mapa`, sem mutex (2.1.1). Os semáforos são criados
  **depois** do mapa, com as capacidades sorteadas.
- **Comando `mapa`** (Console e `atr/comando`): desenha o mapa em texto
  (`#` parede, `o` obstáculo, `1`–`4` região das passagens, `B`/`Z`/`E`/`R`).

### 7.2 A* passo a passo (`calcular_caminho`)
- **Entrada/saída:** `bool calcular_caminho(x0, y0, xd, yd, Caminho *c)`;
  `Caminho` = lista de **células** (`c->x[0..n-1]`, `c->y[...]`, até
  `MAX_CAMINHO` = 512), da origem ao destino; `false` se não há caminho.
- **Vizinhança 4** (cima, baixo, esquerda, direita); parede e obstáculo não
  entram. **Custo de um passo** = 1 + custo extra (7.3).
- **Vocabulário:** `g` = custo real da origem até a célula; `h` = estimativa
  até o destino (Manhattan: |dx| + |dy|); `f = g + h`; `pai` = de onde veio;
  lista **aberta** = descobertas a expandir; **fechada** = já expandidas.
- **Laço:** (1) origem na aberta com g = 0; (2) tira da aberta a célula de
  **menor f** (empate: menor índice y·60+x); (3) se é o destino, remonta o
  caminho pelos pais; (4) senão fecha e, para cada vizinho livre não
  fechado, se `g(atual) + custo` é menor que o g que ele tinha, atualiza g,
  f e pai e o coloca na aberta; (5) volta a (2). Aberta vazia = sem caminho.
- **Exemplo pequeno** (grade 5 x 3, `#` = parede, S = origem (0,0),
  D = destino (4,1); índice = y·5 + x):

  ```
       x: 0 1 2 3 4
     y=0  S . # . .
     y=1  . . # . D
     y=2  . . . . .
  ```

  | passo | expande (f) | o que acontece |
  |---|---|---|
  | 1 | S (0,0) f=0+5 | abre (1,0) e (0,1), os dois com g=1, h=4, f=5 |
  | 2 | (1,0) f=5 | empate de f com (0,1): ganha o menor índice (1 < 5). (2,0) é parede; abre (1,1) g=2 f=5 |
  | 3 | (0,1) f=5 | (1,1) por aqui também daria g=2 (não melhora); abre (0,2) g=2 f=7 |
  | 4 | (1,1) f=5 | (2,1) é parede; abre (1,2) g=3 f=7 |
  | 5 | (0,2) f=7 | nada melhora |
  | 6 | (1,2) f=7 | abre (2,2) g=4 f=7 (o único buraco da parede) |
  | 7 | (2,2) f=7 | abre (3,2) g=5 f=7 |
  | 8 | (3,2) f=7 | abre (3,1) g=6 f=7 e (4,2) g=6 f=7 |
  | 9 | (3,1) f=7 | abre D g=7 f=7 e (3,0) g=7 f=9 |
  | 10 | D f=7 | chegou: D ← (3,1) ← (3,2) ← (2,2) ← (1,2) ← (1,1) ← (1,0) ← S (7 passos) |

  Repare que o f "promete" 5 no começo e sobe para 7 quando a parede obriga
  a dar a volta: o A* só expande o que pode levar a um caminho ótimo, e
  nunca expandiu (3,0), que tem f = 9.
- **Por que o caminho é ótimo:** a heurística de Manhattan é **admissível**:
  nunca é maior que o custo real, porque cada passo custa pelo menos 1 e
  anda 1 célula. Com heurística admissível, o A* devolve o menor custo. Ela
  também é **consistente** (de um vizinho a outro muda no máximo 1), então
  uma célula fechada nunca precisa ser reaberta.
- **Implementação didática:** vetores `g`, `f`, `pai` e `situacao` (não
  vista/aberta/fechada) do tamanho da grade (1800) e **busca linear** do
  menor f, sem heap. No pior caso 1800 x 1800 ≈ 3,2 milhões de comparações:
  poucos milissegundos, bem menos que o período de 100 ms da Navegação. Um
  heap só deixaria o código mais difícil para uma grade tão pequena.

#### Mecanismo: vetores do A* locais (reentrância, sem mutex)
- **Onde:** `mapa.c: calcular_caminho`: `g`, `f`, `pai`, `situacao` e
  `ocupados` são variáveis **locais** (na pilha de quem chama).
- **Problema:** no processo robo, a Navegação (caminho, seguidor) e a Status
  (estação mais próxima) chamam o A* ao mesmo tempo.
- **Como:** cada chamada tem os seus vetores (~23 KB de pilha; as threads
  têm 8 MB). Nenhuma variável `static` muda.
- **Sem isso** (vetores `static` compartilhados): uma thread sobrescreveria o
  `g`/`pai` da outra no meio da busca e os caminhos sairiam errados. Seria
  preciso um mutex só para o A*.

### 7.3 Custo dinâmico das passagens ("otimização")
- **Regra:** ao **entrar** na região de uma passagem (vindo de fora dela), o
  passo custa `1 + 3 × (robôs dentro da passagem agora)`. A ocupação é lida
  uma vez por cálculo com `sem_getvalue` (`ocupados = capacidade − vagas`).
- **Efeito:** com uma passagem vazia o A* escolhe o caminho mais curto; com
  ela cheia, prefere outra se o desvio custar menos que a espera estimada.
- **Estimativa, não garantia:** o valor pode estar desatualizado um instante
  depois (2.8). Se o robô chegar e a passagem estiver cheia, ele espera no
  semáforo normalmente e, depois de 3 s, replaneja (7.5).

### 7.4 Escolha da equipe e do líder (`organizador.c`)
- **Equipe:** os robôs livres de **menor caminho A*** até a baia (antes era a
  distância de Manhattan, que ignora paredes). Robô sem caminho não serve.
  No cenário 2, o robô em (22, 15) fica de fora: para chegar à baia 2 ele
  precisa atravessar a parede 1 pela P1 (23 passos no A*).
- **Líder:** o membro de menor caminho total "posição → baia → zona". Como
  baia → zona é igual para todos (todos saem da mesma baia), o líder é o
  **mais perto da baia**, que é o primeiro escolhido (`membros[0]`). Empate:
  menor id (a escolha percorre os ids em ordem e só troca por um caminho
  estritamente menor). No cenário 2 o líder é o robô 1, não o 0.
- **A ordem de chegada da fila continua** (6.3): a IA decide **quem** leva a
  caixa, não **qual** caixa vai primeiro (isso evita inanição).

### 7.5 Navegação célula a célula e replanejamento
- **Uma célula por vez:** o setpoint é sempre a **próxima célula** do
  caminho. Como ela é vizinha, o simulador (que anda primeiro em x, depois
  em y) nunca "corta o canto" de um obstáculo.
- **Replanejar:** se o robô espera uma passagem por mais de 3 s, recalcula o
  A* a partir de onde está (evento "replanejou rota: passagem Pn cheia").
  Com o custo dinâmico, pode escolher outra passagem; se não houver desvio
  melhor, sai o mesmo caminho e ele continua esperando.
- **Para a tela:** a Navegação publica no buffer de status só os **cantos**
  do caminho (onde a direção muda) a partir da posição atual, até 32
  (`MAX_ROTA_STATUS`); a tela liga os pontos com retas.

### 7.6 Equipes com o novo caminho
- **Seguidor:** a cada ciclo, A* da posição dele até a posição **atual** do
  líder (lida do buffer de status do líder) e um passo só (1ª célula do
  caminho). Não usa semáforo: a equipe ocupa 1 vaga.
- **`IR_ZONA` leva a equipe:** `n_membros` e `membros[4]`, para o líder saber
  de quem esperar.

#### Mecanismo: o líder só libera a passagem quando a equipe inteira saiu
- **Onde:** `navegacao.c: liberar_se_saiu` / `equipe_saiu`.
- **Problema:** o seguidor anda 1–2 células atrás do líder. Se o líder fizesse
  `sem_post` assim que ele mesmo saísse, outro robô poderia entrar com o
  seguidor ainda dentro (mais robôs que a capacidade).
- **Como:** o líder lê a posição de cada membro **no buffer de status** (um
  mutex por vez, cópia local, regra anti-deadlock). Um membro só conta como
  "saiu" depois de ter sido **visto dentro** da região (flag
  `visto_dentro[k]`): o buffer tem até 200 ms de atraso, e um seguidor que
  acabou de entrar ainda pode aparecer do lado de fora, antes da boca.
- **Sem isso:** a passagem de capacidade 1 poderia ter o seguidor de uma
  equipe e outro robô ao mesmo tempo (era a simplificação antiga "seguidor
  dentro da passagem depois que o líder liberou", que deixou de existir).

### 7.7 Por que isto é "IA"
- O A* é o algoritmo clássico de **busca heurística informada** da IA: em
  vez de testar todos os caminhos (busca cega, como o BFS da validação), ele
  usa uma **heurística** (Manhattan) para expandir primeiro as células mais
  promissoras. Com heurística **admissível** ele é **ótimo** (acha o menor
  custo) e **completo** (se existe caminho, acha).
- A "otimização das rotas de cada robô" é o A* com custo dinâmico (desvia de
  passagens cheias e replaneja); a "de cada carga" é o Organizador usando o
  mesmo A* para escolher a equipe e o líder.

---

## 8. Simplificações assumidas (citar no relatório)

**Mapa e movimento**
1. Colisão fora das passagens **não** é tratada: robôs podem ocupar a mesma célula.
2. O mapa é sorteado só no início da execução; paredes, passagens e
   obstáculos não mudam enquanto o sistema roda.
3. Obstáculos são células isoladas (1x1), nunca no corredor y = 15 nem perto
   das bocas. Mapa que não passa no BFS é sorteado de novo (até 100 vezes;
   se nenhum servir, o programa termina).
4. `novo` e `goto` aceitam qualquer célula livre fora das passagens, sem
   conferir se ela é alcançável (ex.: cercada de obstáculos). Sem caminho, o
   robô registra "sem caminho ate o destino" e fica parado.
5. O custo dinâmico só conta robôs **dentro** das passagens, e com uma
   leitura instantânea (`sem_getvalue`) que pode ficar desatualizada; robôs
   no caminho fora das passagens não entram no custo.
6. O replanejamento só acontece depois de 3 s esperando na porta. Se não há
   desvio melhor, sai o mesmo caminho e ele replaneja de novo a cada 3 s
   (um evento "replanejou rota" a cada vez).
7. A* com busca linear na lista aberta (sem heap): suficiente para 1800
   células; numa grade grande seria preciso um heap.

**Equipes**
8. Os seguidores perseguem a posição do líder lida do buffer de status
   (atualizado a cada 200 ms) e andam 1–2 células atrás. Por isso o
   Organizador espera **todos** na zona antes do `SOLTAR`.
9. O líder é o mais perto da baia (7.4) e não muda durante o transporte.
   Ele só libera uma passagem depois de ver cada membro dentro dela e depois
   fora; um seguidor sempre passa pela mesma passagem que o líder (ele vai
   atrás do líder), então isso sempre acontece.
10. Uma carga por baia de cada vez.
11. Se um robô de uma equipe fica em DEFEITO para sempre, a equipe fica
    pausada para sempre (não há reatribuição da caixa).

**Robôs**
12. `goto` só funciona em modo manual; `manual` só é aceito com o robô `DISPONIVEL`.
13. Um `goto` recebido enquanto o robô está dentro de uma passagem só começa depois que ele sai.
14. Robô em modo manual não vai recarregar sozinho (a recarga automática só
    parte de `DISPONIVEL`); com 0% ele entra em DEFEITO.
15. O estado visto por fora (buffer) tem até 200 ms de atraso em relação ao
    estado interno do robô (ex.: logo depois do `SOLTAR` ainda aparece `OPERANDO_CARGA`).
16. A morte inesperada de um processo `robo` não é detectada (o slot continua ativo).
17. Na disputa por uma passagem cheia não há fila de ordem: quem conseguir o
    semáforo primeiro entra (o `sem_timedwait` tenta de novo a cada ciclo).

**Simulador**
18. 1 célula por tick (100 ms); bateria cai 1% a cada 5 células e sobe 2% por
    tick na estação; `i_carga_atracada` segue `o_acoplar_e_erguer` no tick seguinte.
    O simulador roda em C, dentro do processo central; a tela dele é o
    painel "Simulação" da Tela de Operação (mesma tela, como o enunciado permite).
19. O comando `novo` do Console continua existindo (os cenários de teste não
    dependem do broker); na operação normal o novo robô vem por `atr/robo/novo`.

**Infraestrutura**
20. `mq_send` bloqueia se a fila estiver cheia (10 mensagens): uma tarefa
    poderia esperar o Coletor ou a Navegação de um robô esvaziar a fila.
21. Fila de pedidos de novo robô cheia (8) → pedido recusado.
22. Ctrl+C usa o comportamento padrão do `SIGINT` (mata tudo, sem limpeza);
    as sobras de IPC são removidas na próxima inicialização.
23. `historico` lê o arquivo inteiro a cada pedido para pegar as últimas
    linhas (simples; os logs são pequenos). A resposta é truncada se passar
    de `TAMANHO_RESPOSTA` (32 KB).
24. Na tela, o histórico mostrado é a primeira resposta de `historico` que
    chega depois do clique; se duas respostas de comandos chegarem no mesmo
    ciclo de 200 ms da tela, a do `historico` pode ser perdida (basta clicar de novo).

---

## 9. Cenários de teste

Rodar todos (salva em `testes/saidas/`):

```
testes/rodar_todos.sh
```

Ou um só: `ATR_SEMENTE=42 ./logistica testes/sem_caixas.txt < testes/cenarioN.txt`
(`sem_caixas.txt` é vazio, para as caixas de `config/caixas.txt` não
interferirem; cada cenário coloca as suas com o comando `caixa`).

Como o mapa é sorteado, o script fixa a semente: **42** nos cenários 1–4 (a
mesma do vídeo), **1 e 2** no cenário 5 (que roda duas vezes:
`cenario5_semente1` e `cenario5_semente2`) e **5** no cenário 6 (na semente
42 a passagem de capacidade 1 fica sozinha numa parede, sem outra para o
replanejamento escolher; na 5 ela divide a parede 1 com a P1).

| Cenário | O que testa |
|---|---|
| 1 | 1 robô, caixa de tamanho 1: ciclo completo baia → zona pelo caminho A*, atravessando uma passagem de cada parede. |
| 2 | 4 robôs, caixa de tamanho 3: escolha pelo menor caminho A* (o robô atrás da parede fica de fora), líder = o mais perto da baia (robô 1), `AGUARDANDO_EQUIPE`, `ACOPLAR` síncrono, só o líder usa passagens. |
| 3 | Falha e bateria baixa no meio do transporte: `PAUSAR`/`CONTINUAR`, recarga só depois de soltar a carga, na estação de menor caminho. |
| 4 | Caixa de tamanho 4 com 3 robôs: espera sem reservar ninguém, a fila para, o sistema continua respondendo; atendida quando o 4º robô é criado. Depois a fila anda: um 5º robô leva a caixa 2. |
| 5 | Mesmo arquivo com sementes 1 e 2: o comando `mapa` mostra passagens (posição e capacidade) e obstáculos diferentes, e nos dois mapas o robô entrega a caixa da baia 2 na zona D. |
| 6 | Passagem de capacidade 1 (P2) disputada: o robô 0 fica sem bateria dentro dela (segura a vaga); o robô 1 espera na porta e, depois de 3 s, "replanejou rota: passagem P2 cheia" e atravessa pela P1. |

Os cenários usam `aguarda <id> <ESTADO> [timeout_s]` (espera o robô chegar a
um estado, lendo o buffer de status a cada 100 ms) em vez de `espera` sempre
que o teste depende de um momento específico. Assim a sequência de decisões
é sempre a mesma; mudam só horários, pids e a ordem de impressão de linhas
de threads diferentes no mesmo instante.
