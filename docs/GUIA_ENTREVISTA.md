# Guia para a entrevista — 40 perguntas prováveis sobre o código

Cada resposta é curta e em linguagem simples. "Mostrar" indica o
**arquivo:linha** para abrir no editor durante a apresentação (linhas do
commit desta entrega). Detalhes de cada mecanismo: `docs/EXPLICACAO.md`.

---

## A. Processos, threads e IPC

**1. Por que cada robô é um processo, e não uma thread?**
O enunciado pede ("cada robô será gerenciado por um processo diferente", com
pelo menos duas threads). Também isola os robôs: um robô que trava não
corrompe a pilha de outro. O custo é ter de usar IPC (memória compartilhada,
filas, semáforos nomeados) em vez de variáveis globais.
Mostrar: `src/robo_main.c:46-79` (main do robô cria Navegação e Status).

**2. Por que `fork` seguido imediatamente de `execl`?**
O processo central tem várias threads. Depois do `fork`, o filho só tem a
thread que chamou; um mutex que outra thread segurava naquele instante fica
travado para sempre no filho. O `execl` troca o programa inteiro: o robô
começa limpo e abre o IPC pelo nome. Se o `exec` falhar, `_exit(1)` (não roda
nada herdado, nem os `atexit` do pai).
Mostrar: `src/gestor.c:200-219`.

**3. Por que o mutex na memória compartilhada é `PTHREAD_PROCESS_SHARED`?**
Um mutex comum só vale entre threads do mesmo processo. Esses mutexes moram
em `/atr_shm` e são travados por threads de processos diferentes (simulador
no central, Status no robô). Sem o atributo o comportamento é indefinido.
Mostrar: `src/memoria.c:23-34` e o comentário `include/comum.h:151-156`.

**4. Por que um mutex por robô e não um só para todos?**
Para robôs diferentes não esperarem uns pelos outros: o simulador mexendo no
robô 3 não atrasa a Status do robô 7. A seção crítica de cada um é curta.
Mostrar: `include/comum.h:150-156` (`Robo.mutex`).

**5. Como é criada a memória compartilhada? Por que `shm_unlink` antes?**
`shm_open(O_CREAT | O_EXCL)` + `ftruncate` + `mmap(MAP_SHARED)` no central;
o robô só faz `shm_open` + `mmap`. O `shm_unlink` antes remove a sobra de uma
execução interrompida (Ctrl+C), e o `O_EXCL` garante que o objeto é novo.
Mostrar: `src/memoria.c:37-77`.

**6. Por que o mapa fica na memória compartilhada sem mutex?**
Ele é escrito uma vez, por `gerar_mapa`, **antes** de qualquer
`pthread_create` e `fork`; depois só é lido. Condição de corrida exige alguém
escrevendo enquanto outro lê. A criação de thread/processo garante que o
novo fluxo vê o que foi escrito antes.
Mostrar: `src/main.c:130-131`.

**7. Por que filas de mensagens para mandar comandos ao robô, e não um campo na memória compartilhada?**
Com um campo, um segundo comando poderia sobrescrever o primeiro antes de ser
lido. A fila guarda até 10 mensagens em ordem, e `mq_send` é atômico.
Mostrar: `src/gestor.c:182-198` (`enviar_para_robo`), `src/navegacao.c:612-626`.

**8. A Navegação é periódica. Como ela lê a fila sem ficar bloqueada?**
`mq_timedreceive` com prazo = agora: com a fila vazia volta na hora com
`ETIMEDOUT` e o ciclo de 100 ms continua. (A interface POSIX exige
`CLOCK_REALTIME` no prazo.)
Mostrar: `src/navegacao.c:612-626`.

**9. Por que o Coletor usa uma fila e não um mutex no arquivo?**
Muitos produtores (todas as threads, de todos os processos) e um consumidor
que escreve em disco, que é lento. Com a fila, ninguém espera o disco dentro
do seu ciclo, as mensagens nunca se misturam e só o Coletor escreve nos
arquivos (então eles não precisam de mutex). `mq_receive` bloqueia com a fila
vazia: o Coletor dorme sem gastar CPU.
Mostrar: `src/eventos.c:21-36`, `src/coletor.c:87-127`.

**10. O timestamp do evento é de quando?**
De quando o evento aconteceu: quem envia faz `clock_gettime(CLOCK_REALTIME)`
antes do `mq_send`. O Coletor só formata (data, hora e milissegundos).
Mostrar: `src/eventos.c:27`, `src/coletor.c:63-73`.

**11. Como os registros ficam "disponíveis para a operação remota"?**
Ao vivo: depois de gravar, o Coletor publica cada registro em `atr/eventos`.
Do disco: o comando `historico [n]` lê as últimas linhas dos CSV (botão
"Carregar histórico do disco" na tela).
Mostrar: `src/coletor.c:110-121`, `src/console.c:327-361`.

**12. O `historico` lê o arquivo enquanto o Coletor escreve. Precisa de mutex?**
Não. O Coletor é o único escritor, só acrescenta no fim e dá `fflush` a cada
linha; nada já escrito muda. No pior caso a última linha vem pela metade (sem
`\n`) e é descartada. Os vetores são locais, então duas chamadas simultâneas
(Console e MQTT) não se misturam.
Mostrar: `src/console.c:327-349`.

## B. Mutex, variável de condição, deadlock e inanição

**13. Como funciona a fila de pedidos de novo robô?**
Produtor-consumidor clássico: vetor circular + mutex + variável de condição.
Produtores: Console (`novo`) e a thread de rede do MQTT (`atr/robo/novo`).
Consumidor: o Gestor, que dorme com a fila vazia.
Mostrar: `src/gestor.c:52-62`, `86-101` (produtor), `265-292` (consumidor).

**14. Por que `while` e não `if` antes do `pthread_cond_wait`?**
Ao acordar, a condição pode não valer: despertar espúrio (permitido pelo
POSIX) ou o sinal ter sido de encerramento. O `while` testa de novo.
Mostrar: `src/gestor.c:273-276`.

**15. Por que o `broadcast` do encerramento é feito com o mutex travado?**
Para não cair entre o "testa a condição" e o "dorme" do Gestor: se o aviso
viesse nesse intervalo, ele seria perdido e o Gestor dormiria para sempre.
Mostrar: `src/gestor.c:103-111`.

**16. Por que o Gestor cria o processo fora do mutex?**
`fork`/`exec` é demorado; com o mutex travado o Console e a thread do MQTT
ficariam esperando para colocar outro pedido.
Mostrar: `src/gestor.c:282-289`.

**17. Como vocês evitaram deadlock entre mutexes?**
Regra do projeto: **nenhuma thread segura dois mutexes ao mesmo tempo**.
Sempre `unlock` antes do próximo `lock`; dados de dois lugares são copiados
para variáveis locais. Sem "segurar um e pedir outro" não há espera circular.
Exemplo: o seguidor copia a célula do líder com o mutex do líder, solta, e só
depois trava o próprio.
Mostrar: `src/navegacao.c:658-676` (`seguir_lider`), `src/status.c:160-195`.

**18. E deadlock na formação das equipes?**
O Organizador nunca reserva equipe parcial: a caixa só é atribuída quando
**todos** os robôs necessários estão livres no mesmo ciclo. Reservar 2 de 3
para uma caixa e 2 de 3 para outra criaria "segura e espera" + espera
circular.
Mostrar: `src/organizador.c:486-512`.

**19. Como evitaram inanição da caixa de tamanho 4?**
Ordem de chegada: se a primeira caixa (com a baia livre) não tem equipe, a
fila **para**. Sem isso as caixas pequenas sempre passariam na frente e os 4
robôs nunca estariam livres juntos. Cenário 4.
Mostrar: `src/organizador.c:394-409` e `496-507`.

**20. O que acontece se tirar o mutex do robô?**
Leituras pela metade: a Status poderia ler x novo com y antigo enquanto o
simulador move o robô; o Organizador poderia ver posição nova com estado
antigo. Erros raros e difíceis de reproduzir.
Mostrar: `src/simulador.c:128-133`, `src/status.c:163-169`.

**21. E se tirar o `mutex_caixas`?**
O Console (`caixa`) e o Organizador mexem em `n_caixas` e `caixas[]` ao mesmo
tempo: dois `n_caixas++` simultâneos perdem uma caixa, ou o Organizador lê
uma caixa ainda não preenchida.
Mostrar: `src/organizador.c:112-142`, `394-409`.

**22. E se tirar o mutex do `estado_local`?**
A Navegação poderia apagar um DEFEITO que a Status acabou de colocar ("testar
e mudar" deixaria de ser uma operação só), e as flags `parar`/`ir_recarregar`
poderiam ser perdidas.
Mostrar: `src/navegacao.c:194-205` (`definir_estado`), `include/comum.h:436-455`.

**23. Por que `encerrar` é `atomic_bool` e não `bool` com mutex?**
É uma variável só, que só vai de `false` para `true`. Com `bool` comum, ler e
escrever ao mesmo tempo é comportamento indefinido e o compilador pode
guardar o valor num registrador (o laço nunca terminaria). O atômico resolve
sem mutex; o `_Static_assert` garante que ele é lock-free (necessário entre
processos).
Mostrar: `include/comum.h:304-315` e `336`.

**24. Como é o encerramento?**
`encerrar = true` → acorda o Gestor → `pthread_join` em Gestor, Organizador,
Simulador e Ponte → `SIGTERM` + `waitpid` em cada robô → `COLETOR_FIM` (a fila
é FIFO, tudo o que veio antes é gravado) → `mq_unlink`, `sem_unlink`,
`shm_unlink`. Assim nada é destruído enquanto alguém ainda usa.
Mostrar: `src/main.c:150-175`, `101-120`.

## C. Passagens, semáforos e anti-colisão

**25. Como funciona o limite de robôs na passagem?**
Um semáforo contador nomeado por passagem, criado com valor inicial = a
capacidade sorteada. Entrar = `sem_trywait`; sair = `sem_post`.
Mostrar: `src/main.c:65-78`, `src/ocupacao.c:213-245`.

**26. Por que `sem_trywait` e não `sem_wait`?**
`sem_wait` prenderia a thread de Navegação: ela pararia de ler `PAUSAR` e a
flag de defeito. Além disso ele é chamado com o mutex do monitor travado, e
ninguém pode bloquear dentro de um mutex. Com `trywait` o robô tenta de novo
no próximo ciclo.
Mostrar: `src/ocupacao.c:218-232`.

**27. Como funciona a mão única?**
Monitor por passagem (mutex + `dentro`, `sentido`, `esperando[2]`): entra se
o túnel está vazio, ou se está no mesmo sentido **e** ninguém espera do outro
lado. O semáforo cuida da capacidade; o monitor, do sentido. Sem isso, dois
robôs em sentidos opostos se encontrariam no meio do túnel (deadlock).
Mostrar: `src/ocupacao.c:211-234`, `include/comum.h:264-277`.

**28. E a inanição de quem espera do outro lado?**
`esperando[oposto]`: assim que alguém espera do outro lado, quem chega no
sentido atual para de entrar; o túnel esvazia e o outro lado passa.
Mostrar: `src/ocupacao.c:219` e `230-231`.

**29. O robô pode segurar duas passagens? E se ficar em DEFEITO lá dentro?**
Não: as regiões ficam separadas por células livres, e se ele ainda segura uma
(equipe saindo) não pede a próxima. Em DEFEITO dentro da passagem ele continua
com a vaga (fisicamente está ali); os outros esperam e, depois de 3 s,
replanejam por outra passagem.
Mostrar: `src/navegacao.c:866-881`, `780-792`.

**30. Como funciona a anti-colisão fora das passagens? Por que é descentralizada?**
Grade de ocupação na memória compartilhada com um mutex. Antes de andar, a
Navegação **reserva** a próxima célula; ao chegar, **libera** a anterior
(cada robô segura no máximo 2). Tudo acontece na thread de Navegação de cada
robô; o Organizador nunca é consultado.
Mostrar: `src/ocupacao.c:107-139`, `src/navegacao.c:320-384`.

**31. E se dois robôs ficarem frente a frente?**
Depois de 1 s bloqueado, o robô recalcula o A* tratando como obstáculo as
células ocupadas por outros. Desempate: o de maior id desvia com 10 ciclos, o
outro só com 30 — os dois nunca desviam juntos (e se travariam de novo).
Cenário 8.
Mostrar: `src/navegacao.c:386-404` e `809-831`.

**32. O que é a "equipe em bloco" e que deadlock ela resolveu?**
O líder só avança quando a equipe toda está na célula dele (contagem na
grade). Antes, outra equipe entrou no buraco entre o líder e os seguidores
dele, enquanto o líder segurava a passagem seguinte: espera circular (P3 →
seguidores → equipe 2 → P3). Sem buraco, ninguém entra no meio da equipe.
Cenário 11.
Mostrar: `src/navegacao.c:716-721` e `859-864`.

**33. Como vocês sabem que não houve colisão?**
O simulador (que é o hardware) compara a posição real de todos os robôs a cada
100 ms; dois robôs de grupos diferentes na mesma célula (fora de baia, zona e
estação) geram o evento `COLISAO`. O `rodar_todos.sh` confere que não houve
nenhuma; o benchmark também (até 128 robôs).
Mostrar: `src/simulador.c:87-116`.

## D. IA, tempo real e escalabilidade

**34. Como funciona o A\*?**
Lista aberta começa com a origem (g = 0). A cada passo tira a célula de menor
f = g + h (h = distância de Manhattan até o destino), fecha, e para cada
vizinho livre calcula g novo; se melhorou, atualiza g, f e o pai. Ao tirar o
destino, remonta o caminho pelos pais. Manhattan nunca superestima (cada
passo custa ≥ 1), então o caminho é ótimo. Busca linear sem heap: 1800
células, poucos ms.
Mostrar: `src/mapa.c:258-335`, `227-230`.

**35. Como o A\* "otimiza" com as passagens cheias?**
Custo dinâmico: entrar numa passagem custa `1 + 3 × robôs dentro`, e `+30` se
ela está ocupada no sentido contrário. A ocupação é lida com `sem_getvalue`
(só estimativa; quem garante a capacidade é o `sem_trywait`).
Mostrar: `src/mapa.c:179-190` e `239-251`.

**36. Navegação e Status chamam o A\* ao mesmo tempo. Precisa de mutex?**
Não: os vetores `g`, `f`, `pai`, `situacao` são **locais** (pilha de cada
chamada). Se fossem `static`, uma thread estragaria a busca da outra.
Mostrar: `src/mapa.c:263-267`.

**37. Como o Organizador escolhe a equipe, o líder e confere a bateria?**
Robôs DISPONIVEL e automáticos ordenados pelo menor caminho A* até a baia; o
líder é o primeiro (o mais perto). Só serve quem tem bateria ≥ 30% e ≥ custo
da tarefa inteira (robô → baia → zona → estação, passos/5 + 10). Quem ficou
de fora só pela bateria recebe `RECARREGAR`.
Mostrar: `src/organizador.c:342-389` e `466-481`.

**38. Como as tarefas periódicas evitam deriva?**
Guardam o instante da próxima ativação e dormem até ele com
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`; `somar_ms` soma o período.
Com `sleep` relativo, o tempo do trabalho se somaria ao período.
Mostrar: `include/comum.h:64-76`, `src/navegacao.c:948-977`.

**39. Como mediram os tempos e o que é cada número do `tempos`?**
Em cada ciclo: atraso = início real − instante planejado; C = fim − início;
perda de prazo se fim > planejado + T. Guarda média, máximo e contagem, sob
um mutex (`mutex_tempos` para as centrais, o do robô para Navegação/Status).
U = Cmax/T.
Mostrar: `include/comum.h:86-117`, `src/navegacao.c:969-973`.

**40. Como foi feita a RTA e qual a resposta para o número máximo de robôs?**
R_i = C_i + Σ ⌈R_i/T_j⌉·C_j sobre as tarefas de maior prioridade (Rate
Monotonic), iterando até convergir, com os C máximos medidos; escalonável se
R ≤ T. Para o Organizador (menor prioridade) com N = 8: 144 → 192 ms ≤ 500 ms,
ok; com N = 12 passa de 500 ms. Então **num núcleo só** a RTA garante 8
robôs. **Medido nos 12 núcleos**: zero perdas de prazo e latência de falha
< 300 ms até **128 robôs** (o máximo testado); o primeiro limite rígido é o
do SO (255 filas POSIX), e a vazão para de crescer em ~64 robôs (4 baias).
Mostrar: `docs/ESCALABILIDADE.md` (seções "RTA" e "Resposta").

---

## Perguntas extras rápidas

- **Onde está o buffer de status e quem escreve?** `include/comum.h:164-180`;
  só `src/status.c:122-146` (`atualizar_buffer`).
- **Por que `parar_onde_esta` lê e escreve no mesmo lock?** Senão o simulador
  poderia mover o robô entre a leitura e a escrita, para uma célula que ele
  acabou de devolver. `src/navegacao.c:287-298`.
- **Como a Ponte monta o JSON sem segurar o robô?** Copia o slot com o mutex
  para uma variável local e monta o texto fora. `src/ponte_mqtt.c:263-274`.
- **Como a tela Python sincroniza a thread do MQTT com a interface?**
  `threading.Lock` na loja de estado; a interface lê uma cópia a cada 200 ms.
  `interface/estado.py:42` e `116-119`.
- **O que muda com `ATR_TEMPO_REAL=1`?** Cada thread pede SCHED_FIFO com
  prioridade Rate Monotonic; sem permissão avisa uma vez (`atomic_exchange`) e
  segue normal. `src/tempo_real.c:31-44`, `include/comum.h:52-62`.
- **Papel de cada integrante?** Ver o quadro do capítulo 9 do relatório
  ([PREENCHER] antes da entrega).
