# Roteiro do vídeo (máximo 10 minutos)

Mapa fixo: **semente 42** (o mesmo dos testes). Nela as passagens são:

| Passagem | Parede | Linha | Capacidade |
|---|---|---|---|
| P1 | 1 | y = 12 | 4 |
| P2 | 1 | y = 20 | 2 |
| P3 | 2 | y = 13 | 1 |
| P4 | 3 | y = 24 | 3 |

## Preparação (antes de gravar, fora do vídeo)

- Compilar: `make`. Conferir que o mosquitto está rodando (`pgrep mosquitto`;
  se não, `mosquitto &`). Apagar logs antigos se quiser: `rm -f logs/*.csv`.
- **Terminal 1** (fica aberto o vídeo todo): `interface/.venv/bin/python interface/app.py`.
  Usamos a tela separada (e não o `./rodar.sh`) porque nas partes 9 a 11 o
  núcleo é reiniciado com outros roteiros e outra semente, e a tela continua
  aberta.
- **Terminal 2**: na raiz do repositório, pronto para os comandos abaixo.
- Navegador em <http://localhost:8080>, zoom 80–90% para caber mapa e painéis.
- Abrir também `docs/imagens/arquitetura.png` e `docs/ESCALABILIDADE.md`
  (pré-visualização do Markdown) em abas.
- Ensaiar uma vez o "Cenário de demonstração" (botão no painel Simulação):
  ele tem 5 atos e **pausa** no fim de cada um até clicar "Próximo ato ▶".

## Tabela de tempos

| # | Tempo | Parte |
|---|---|---|
| 1 | 0:00–0:40 | Abertura e arquitetura |
| 2 | 0:40–1:15 | Subir o sistema e o mapa da semente 42 |
| 3 | 1:15–2:35 | Criação de robôs, equipe e acoplamento síncrono (ato 1) |
| 4 | 2:35–3:30 | Passagem com capacidade (ato 2) |
| 5 | 3:30–4:05 | Caixa de tamanho 4 esperando (ato 3) |
| 6 | 4:05–4:55 | Falha e PAUSAR/CONTINUAR (ato 4) |
| 7 | 4:55–5:35 | Bateria e recarga (ato 5) |
| 8 | 5:35–6:30 | Modo manual e goto |
| 9 | 6:30–7:35 | Mão única e anti-colisão com desvio (cenários 9 e 8) |
| 10 | 7:35–8:10 | Mapa aleatório: duas sementes |
| 11 | 8:10–8:50 | Logs CSV e histórico na tela |
| 12 | 8:50–9:45 | Tempo real e escalabilidade |
| 13 | 9:45–10:00 | Encerramento |

---

## 1. Abertura e arquitetura (0:00–0:40)

**Tela:** `docs/imagens/arquitetura.png`, passando o mouse pelas partes.

**Narração:** "Este é o nosso sistema de logística com robôs móveis. O núcleo
é em C: o processo central `logistica` tem as threads Gestor de Robôs,
Organizador, Coletor de Dados, Simulador do hardware e a Ponte MQTT. Cada robô
é um processo separado, criado com fork e exec, com duas threads: Navegação, a
cada 100 milissegundos, e Status, a cada 200. Eles conversam por memória
compartilhada com mutexes entre processos, filas de mensagens POSIX e
semáforos nomeados nas passagens. A Tela de Operação, que também é a tela do
simulador, é em Python e fala com o C só por MQTT. Tudo sobe com um comando:
`./rodar.sh` ou `docker compose up`."

## 2. Subir o sistema e o mapa (0:40–1:15)

**Tela:** Terminal 2:

```bash
ATR_SEMENTE=42 ./logistica testes/sem_caixas.txt
```

Mostrar a saída "Mapa gerado com semente 42" e as 4 passagens; depois o
navegador (cabeçalho "Mapa: semente 42", "sistema central online").

**Narração:** "O mapa é sorteado a cada execução; com a semente 42 ele se
repete. São três paredes e quatro passagens com capacidades 1, 2, 3 e 4
embaralhadas — cada uma é um semáforo contador com esse valor inicial — e 45
obstáculos. À esquerda as baias, à direita as zonas de entrega, e as duas
estações de recarga. Começamos sem caixas, para o roteiro de demonstração."

## 3. Criação de robôs, equipe e acoplamento síncrono (1:15–2:35)

**Tela:** painel Simulação → "Cenário de demonstração". Ato 1. Alternar entre
o mapa e o Terminal 2 (linhas "Gestor: robo N criado (pid …)" e
"Organizador: caixa … robos … indo para a baia", "ACOPLAR", "IR_ZONA").

**Narração:** "O simulador publica um novo robô no tópico `atr/robo/novo`; a
Ponte MQTT coloca o pedido numa fila com mutex e variável de condição, e o
Gestor, que estava dormindo, acorda e cria o processo — aqui o pid de cada
um. O Organizador escolhe a equipe pelos robôs de menor caminho A* até a baia,
e o líder é o mais próximo. Cada robô que chega fica laranja, aguardando
equipe. Só quando todos chegaram o Organizador manda ACOPLAR para todos no
mesmo ciclo: ninguém ergue a caixa sozinho. Só com todos confirmando a carga
atracada sai o IR_ZONA. A equipe anda em bloco: o líder só dá o próximo passo
com todos na célula dele."

## 4. Passagem com capacidade (2:35–3:30)

**Tela:** "Próximo ato ▶" (ato 2). Os três robôs vão até a boca esquerda da
P2; o primeiro fica sem bateria dentro do túnel (DEFEITO) e o segundo para
atrás dele. Zoom no semáforo da P2 ficando vermelho (2/2) e no terceiro robô
parado na porta (borda tracejada amarela). No "Próximo ato ▶" a bateria do
primeiro é trocada, os dois saem e o terceiro atravessa.

**Narração:** "A P2 tem capacidade 2. Cada robô faz `sem_trywait` antes de
entrar: com a passagem cheia, o semáforo fica vermelho e o próximo espera na
porta, sem bloquear a thread — ele tenta de novo no próximo ciclo e continua
lendo as mensagens. Uma equipe ocupa uma vaga só, porque só o líder usa o
semáforo. Se a espera passar de 3 segundos, o robô replaneja: o A* dá custo
maior a passagem cheia e pode escolher outra."

## 5. Caixa de tamanho 4 esperando (3:30–4:05)

**Tela:** ato 3. Painel Caixas com a faixa âmbar "aguardando equipe".

**Narração:** "Esta caixa precisa de 4 robôs e não há 4 livres. O
Organizador não reserva ninguém — reservar parte da equipe poderia gerar
deadlock entre duas caixas grandes — e a fila para, em ordem de chegada,
para as caixas pequenas não passarem na frente para sempre: isso evita
inanição."

## 6. Falha e PAUSAR/CONTINUAR (4:05–4:55)

**Tela:** ato 4. Robô seguidor fica vermelho (notificação "Robô N em
DEFEITO"); eventos "PAUSAR"; depois "OK" e "CONTINUAR".

**Narração:** "Geramos um defeito num seguidor pelo simulador. A thread
Status lê `i_falha`, muda o estado para DEFEITO e liga a flag que faz a
Navegação segurar a posição. O Organizador vê o defeito no buffer de status e
manda PAUSAR para toda a equipe, senão o líder arrastaria a caixa. Com o OK
o defeito some e ele manda CONTINUAR."

## 7. Bateria e recarga (4:55–5:35)

**Tela:** ato 5. Robô com bateria baixa ficando azul-petróleo (Recarregando),
indo à estação e voltando a verde com 100%.

**Narração:** "Baixamos a bateria pelo simulador. A Status calcula com o A*
o caminho até a estação mais próxima: se a bateria não basta para chegar com
margem, ela manda o robô recarregar e ele fica indisponível até 100%. O
Organizador também só escolhe um robô se a bateria basta para a tarefa
inteira, incluindo ir à estação depois."

## 8. Modo manual e goto (5:35–6:30)

**Tela:** clicar "Encerrar" na faixa do roteiro. Selecionar um robô
DISPONIVEL no mapa → painel de detalhe → "Manual" → "ir para…" → "Recarga
1 (22, 1)"; depois clicar numa célula livre do mapa para o goto. Mostrar a rota
pontilhada e a bandeirinha do destino.

**Narração:** "Em modo manual o Organizador não escolhe mais esse robô, e o
operador dá o setpoint: um local conhecido, como a estação de recarga ou uma
baia, ou qualquer posição clicando no mapa. A tela só envia a linha `goto`
pelo MQTT — a mesma do Console — e a Navegação calcula o caminho A*. Com
'Auto' ele volta para o Organizador."

## 9. Mão única e anti-colisão com desvio (6:30–7:35)

**Tela:** Terminal 2: `sair` (encerramento limpo). Depois, com a tela aberta:

```bash
ATR_SEMENTE=42 ./logistica testes/sem_caixas.txt < testes/cenario9.txt
ATR_SEMENTE=42 ./logistica testes/sem_caixas.txt < testes/cenario8.txt
```

No cenário 9, zoom na P1 (seta azul do sentido; o robô 1 esperando na boca
direita). No cenário 8, os dois robôs frente a frente na linha 15 e o desvio.

**Narração:** "A anti-colisão é descentralizada: cada robô decide na própria
thread de Navegação, sem o Organizador. Antes de cada passo ele reserva a
próxima célula numa grade na memória compartilhada. Na P1, que cabe 4, o robô
1 tem vaga no semáforo mas espera: a passagem é de mão única, controlada por
um monitor — um mutex com o sentido de quem está dentro. Ele só entra depois
que o robô 0 sai. No segundo teste, os dois vêm frente a frente; depois de um
segundo travado, o de maior id recalcula o A* desviando, e o outro espera mais
— assim os dois nunca desviam ao mesmo tempo. O simulador confere a cada
ciclo e não registrou nenhuma colisão."

## 10. Mapa aleatório: duas sementes (7:35–8:10)

**Tela:** Terminal 2:

```bash
ATR_SEMENTE=7 ./logistica testes/sem_caixas.txt
```

Digitar `mapa`; mostrar na tela o mapa novo (cabeçalho "semente 7").
Lado a lado com a captura `docs/imagens/tela_s42_mao_unica.png` (semente 42).

**Narração:** "Com outra semente, outro galpão: as passagens mudaram de lugar
e de capacidade — a de capacidade 1 agora está na parede 2 — e os obstáculos
são outros. Antes de aceitar o mapa, uma busca em largura confere que toda
baia alcança todas as zonas e estações. Nada do mapa está fixo na tela: ela
desenha o que o C publica em `atr/mapa`."

## 11. Logs CSV e histórico na tela (8:10–8:50)

**Tela:** Terminal 2: `sair`; depois

```bash
tail -n 8 logs/eventos.csv
tail -n 5 logs/cargas.csv
```

No navegador, painel Eventos → "Carregar histórico do disco" e o filtro por
robô.

**Narração:** "O Coletor de Dados recebe todos os eventos por uma fila de
mensagens — muitos produtores, um consumidor — e só ele escreve nos arquivos.
Cada linha tem data e hora com milissegundos, tirada no momento do evento.
Em `cargas.csv` ficam a caixa, a baia, a zona e os robôs. Cada registro vai
também para o MQTT, e o botão carrega o histórico gravado em disco."

## 12. Tempo real e escalabilidade (8:50–9:45)

**Tela:** card "Tempo real" da tela (antes do `sair` da parte 10, ou numa
captura); depois `docs/ESCALABILIDADE.md`: gráficos de utilização, atraso e
vazão, e a seção "Resposta".

**Narração:** "Todas as tarefas periódicas usam `clock_nanosleep` com tempo
absoluto, sem deriva, e medem o tempo de execução, o atraso e as perdas de
prazo. O benchmark rodou de 1 a 128 robôs: zero perdas de prazo e a falha
detectada em menos de 300 milissegundos até 128 robôs, o máximo testado,
também com SCHED_FIFO e prioridades Rate Monotonic. Pela análise de tempo de
resposta, num núcleo só garantiríamos 8 robôs; o primeiro limite rígido é o
do sistema operacional, 255 filas POSIX; e a vazão para de crescer em 64
robôs, porque o galpão tem 4 baias."

## 13. Encerramento (9:45–10:00)

**Tela:** relatório (`docs/RELATORIO.pdf`), capítulo 7.

**Narração:** "Os pontos extras — mapa aleatório com IA de rotas e
anti-colisão descentralizada — estão detalhados no capítulo 7 do relatório.
Obrigado."

---

### Dicas de gravação

- Se um ato demorar mais que o previsto, corte a espera na edição (a
  narração do ato já cobre o que aparece).
- Os cenários 8 e 9 duram ~10 s cada; dá para rodar os dois na mesma linha:
  `for n in 9 8; do ATR_SEMENTE=42 ./logistica testes/sem_caixas.txt < testes/cenario$n.txt; done`.
- O cenário de demonstração precisa do sistema **recém-iniciado e sem
  caixas** (`testes/sem_caixas.txt`).
- Se algo esperado num ato não acontecer em 30 s, a faixa mostra um aviso
  "⚠ não aconteceu em 30 s" e libera "Próximo ato ▶": dá para seguir para o
  próximo ato (e cortar o trecho na edição) sem reiniciar.
