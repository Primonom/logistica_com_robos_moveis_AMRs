# Regras do projeto (CLAUDE.md)

Estas regras valem para TODAS as tarefas deste projeto.

**Idioma:** Todas as respostas, resumos e mensagens de commit em português.

## Contexto
Trabalho final da disciplina Automação em Tempo Real (UFMG, Prof. Leandro Freitas): sistema de logística com robôs móveis (AMRs) em C (POSIX: processos, pthreads, mutex, variável de condição, semáforos nomeados, filas de mensagens, memória compartilhada) + Tela de Operação em Python (NiceGUI) via MQTT (mosquitto). O aluno será ENTREVISTADO sobre cada linha, então:

## Regras de código
1. SIMPLICIDADE acima de tudo. Prefira a solução mais curta e óbvia que resolve o problema. Nada de abstrações, macros espertas, ponteiros para função desnecessários ou otimizações prematuras.
2. Mantenha o estilo existente: C11, nomes e comentários em português, sem acentos em identificadores, comentários de cabeçalho em cada arquivo explicando: o que o arquivo faz, quais threads existem, e uma seção "Sincronização" dizendo cada mecanismo usado, que problema resolve e o que aconteceria sem ele.
3. Todo mecanismo de sincronização novo precisa de um comentário "POR QUE:" no ponto onde é declarado.
4. Regra anti-deadlock do projeto: NENHUMA thread segura dois mutexes ao mesmo tempo. Sempre unlock antes do próximo lock. Se precisar de dados de dois lugares, copie para variáveis locais.
5. Regra do buffer de status: fora do processo do robô (Organizador, Console, Ponte MQTT), só se lê o buffer de status do robô (e_*, j_*, bateria, etc.), nunca o hw. Exceção documentada: o simulador e os comandos que emulam hardware (falha/ok/bateria).
6. Tarefas periódicas usam clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) com somar_ms (sem deriva).
7. Compilação sem NENHUM warning com o Makefile atual (-Wall -Wextra).
8. Caminhos sempre relativos ao diretório de execução (raiz do repositório).

## Regras de entrega de cada tarefa
- Ao terminar: `make clean && make` sem warnings; `testes/rodar_todos.sh` rodando todos os cenários sem "AVISO: timeout"; `interface/.venv/bin/python -m pytest -q interface` passando; `docker compose build` funcionando.
- Atualize docs/EXPLICACAO.md com todo mecanismo novo/alterado (mesmo formato: Onde / Problema / Como / Sem ele) e a lista de simplificações.
- Se mudou o comportamento, regenere as saídas em testes/saidas/.
- Faça um commit por tarefa com mensagem em português descrevendo o que mudou.
- No final, me mostre um resumo curto: arquivos alterados, o que cada mudança faz, e 3 perguntas que o professor provavelmente faria sobre essa parte, com a resposta.
