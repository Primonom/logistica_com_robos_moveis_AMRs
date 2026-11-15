#!/bin/bash
# Benchmark de escalabilidade (análise de tempo real, docs/ESCALABILIDADE.md).
#
# Para cada N em 1 2 4 8 12 16 24 32 48 64 96 128 (semente 42):
#   - gera na hora um roteiro de Console que cria N robôs e coloca
#     max(2*N, 64) caixas de tamanhos variados (1..min(4,N), baias e zonas
#     alternadas). Com no mínimo 64 caixas a fila nunca esvazia nos 60 s
#     (a vazão máxima medida é bem menor), então a vazão medida é a do
#     sistema e não a falta de trabalho;
#   - zera as medições ("tempos zerar"), espera 60 s com o sistema
#     trabalhando e roda "tempos" e "status";
#   - mede 10 vezes a latência ponta a ponta de um evento crítico
#     ("medir_falha <id>": liga a falha e espera o buffer mostrar DEFEITO),
#     com esperas de 0,30 s, 0,32 s, ..., 0,48 s entre as amostras. Com
#     espera fixa a falha cairia sempre na MESMA fase do ciclo de 200 ms da
#     Status (ex.: sempre ~30 ms antes dela acordar); somando 20 ms a cada
#     amostra, a fase anda 20 ms e as 10 amostras cobrem o ciclo inteiro.
# Saídas (testes/saidas/):
#   escalabilidade_N.txt     (rt: escalabilidade_rt_N.txt) saída completa,
#                            com o hardware no começo
#   escalabilidade.csv       modo,N,tarefa,T_ms,Cmed_us,Cmax_us,atraso_max_us,perdas,U
#   escalabilidade_falha.csv modo,N,amostra,latencia_ms
#   escalabilidade_N_eventos.csv / _cargas.csv  (rt: escalabilidade_rt_N_*)
#                            linhas que a rodada acrescentou aos logs do
#                            Coletor, só as usadas na vazão: início do
#                            sistema, atribuição ("indo para a baia"),
#                            pedidos recusados e COLISAO; e as cargas
#                            entregues (registradas no SOLTAR)
#
# Uso (de qualquer pasta): testes/escalabilidade.sh [normal|rt|ambos]
# No Docker (modo rt, com SYS_NICE; ver docs/ESCALABILIDADE.md):
#   ATR_PORTA_MQTT=1884 docker compose run --rm -T -v ./testes:/app/testes \
#       logistica testes/escalabilidade.sh rt   (1884: se o host já usa a 1883)
#   normal: SCHED_OTHER (padrão do Linux)
#   rt:     ATR_TEMPO_REAL=1 (SCHED_FIFO, Rate Monotonic); só roda se o
#           usuário tem permissão (testado com "chrt -f 1 true")
#   ambos (padrão): normal e depois rt
# Leva ~16 minutos por modo. Depois: python testes/analisar_escalabilidade.py

cd "$(dirname "$0")/.." || exit 1   # raiz do repositório
# No container não há Makefile nem compilador: os executáveis já vêm prontos.
if [ -f Makefile ]; then make || exit 1; fi
mkdir -p testes/saidas

VALORES_N=${VALORES_N:-"1 2 4 8 12 16 24 32 48 64 96 128"}   # pode trocar (teste rápido)
DURACAO=${DURACAO:-60}                                # segundos de medição
ZONAS=(A B C D)
CSV=testes/saidas/escalabilidade.csv
CSV_FALHA=testes/saidas/escalabilidade_falha.csv
ROTEIRO=$(mktemp)

# Hardware e ambiente (vai no começo de cada arquivo de saída).
hardware() {
    echo "# data: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "# nproc: $(nproc)"
    echo "# cpu: $(lscpu | grep 'Model name' | sed 's/Model name: *//')"
    echo "# uname: $(uname -a)"
    echo "# modo: $1 (ATR_TEMPO_REAL=${2:-0}); ulimit -r = $(ulimit -r)"
    # Limites do SO que dependem do número de robôs (docs/ESCALABILIDADE.md).
    echo "# queues_max: $(cat /proc/sys/fs/mqueue/queues_max)"
    echo "# msg_max: $(cat /proc/sys/fs/mqueue/msg_max)"
    echo "# rlimit_msgqueue: $(ulimit -q)"
    echo "# max_processos: $(ulimit -u)"
    echo "# memoria_mb: $(awk '/^MemTotal/ {print int($2 / 1024)}' /proc/meminfo)"
    echo "# rt_runtime_us: $(cat /proc/sys/kernel/sched_rt_runtime_us)"
}

# Roteiro de Console para N robôs.
gerar_roteiro() {
    local n=$1 k tam max_tam n_caixas
    max_tam=$(( n < 4 ? n : 4 ))
    n_caixas=$(( 2 * n > 64 ? 2 * n : 64 ))
    {
        for ((k = 0; k < n; k++)); do echo "novo"; done
        echo "espera 5"
        for ((k = 0; k < n_caixas; k++)); do
            tam=$(( 1 + k % max_tam ))
            echo "caixa $tam $(( 1 + k % 4 )) ${ZONAS[(k / 4) % 4]}"
        done
        echo "espera 2"
        echo "tempos zerar"
        echo "espera $DURACAO"
        echo "tempos"
        echo "status"
        for ((k = 0; k < 10; k++)); do
            echo "medir_falha $(( k % n ))"
            echo "ok $(( k % n ))"
            echo "espera 0.$(( 30 + 2 * k ))"   # 0,30 s, 0,32 s, ... 0,48 s
        done
        echo "sair"
    } > "$ROTEIRO"
}

# Número de linhas de um arquivo (0 se ainda não existe).
linhas() {
    if [ -f "$1" ]; then wc -l < "$1"; else echo 0; fi
}

# rodar <modo> <valor de ATR_TEMPO_REAL>
rodar_modo() {
    local modo=$1 rt=$2 n saida
    for n in $VALORES_N; do
        if [ "$modo" = rt ]; then saida=testes/saidas/escalabilidade_rt_$n.txt
        else saida=testes/saidas/escalabilidade_$n.txt; fi
        echo "== $modo: N = $n =="
        gerar_roteiro "$n"
        hardware "$modo" "$rt" > "$saida"
        ev=$(linhas logs/eventos.csv)
        ca=$(linhas logs/cargas.csv)
        ATR_SEMENTE=42 ATR_TEMPO_REAL=$rt timeout 200 ./logistica testes/sem_caixas.txt \
            < "$ROTEIRO" >> "$saida" 2>&1
        echo "   codigo de saida: $?"
        grep -h "AVISO" "$saida"
        # Vazão: só as linhas novas dos logs do Coletor (como em rodar_todos.sh).
        tail -n +$((ev + 1)) logs/eventos.csv \
            | grep -E "sistema iniciado|indo para a baia|recusado|COLISAO" > "${saida%.txt}_eventos.csv"
        tail -n +$((ca + 1)) logs/cargas.csv > "${saida%.txt}_cargas.csv"
        grep -h "recusado" "${saida%.txt}_eventos.csv"
        # Linhas da tabela do "tempos": tarefa T ciclos Cmed Cmax atraso perdas U
        awk -v m="$modo" -v n="$n" '
            $1 ~ /^(navegacao|status|simulador|organizador|ponte_mqtt)$/ && NF == 8 {
                print m "," n "," $1 "," $2 "," $4 "," $5 "," $6 "," $7 "," $8 }' \
            "$saida" >> "$CSV"
        awk -v m="$modo" -v n="$n" '
            /^latencia_falha/ { k++; print m "," n "," k "," $4 }' "$saida" >> "$CSV_FALHA"
    done
}

MODO=${1:-ambos}
# "ambos" recomeça os CSVs; um modo só acrescenta (cria o cabeçalho se faltar).
if [ "$MODO" = ambos ] || [ ! -f "$CSV" ]; then
    echo "modo,N,tarefa,T_ms,Cmed_us,Cmax_us,atraso_max_us,perdas,U" > "$CSV"
    echo "modo,N,amostra,latencia_ms" > "$CSV_FALHA"
fi

if [ "$MODO" = normal ] || [ "$MODO" = ambos ]; then
    rodar_modo normal 0
fi
if [ "$MODO" = rt ] || [ "$MODO" = ambos ]; then
    if chrt -f 1 true 2>/dev/null; then
        rodar_modo rt 1
    else
        echo "== rt: sem permissao para SCHED_FIFO (ulimit -r = $(ulimit -r)); modo rt nao executado =="
        echo "   (rode como root, ou no Docker: o compose.yaml da cap_add SYS_NICE)"
    fi
fi
rm -f "$ROTEIRO"
echo "CSV: $CSV e $CSV_FALHA"
