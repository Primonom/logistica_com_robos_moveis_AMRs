#!/bin/bash
# Roda os cenários 1 a 4 e salva os resultados em testes/saidas/:
#   cenarioN.txt          saída da tela (Console, Gestor, Organizador...)
#   cenarioN_eventos.csv  linhas que o cenário acrescentou a logs/eventos.csv
#   cenarioN_cargas.csv   linhas que o cenário acrescentou a logs/cargas.csv
# Uso (de qualquer pasta): testes/rodar_todos.sh

cd "$(dirname "$0")/.." || exit 1   # raiz do repositório
make || exit 1
mkdir -p testes/saidas

# Número de linhas de um arquivo (0 se ainda não existe).
linhas() {
    if [ -f "$1" ]; then wc -l < "$1"; else echo 0; fi
}

for n in 1 2 3 4; do
    echo "== cenario $n =="
    ev=$(linhas logs/eventos.csv)
    ca=$(linhas logs/cargas.csv)

    timeout 180 ./logistica testes/sem_caixas.txt < testes/cenario$n.txt \
        > testes/saidas/cenario$n.txt 2>&1
    echo "   codigo de saida: $?"

    tail -n +$((ev + 1)) logs/eventos.csv > testes/saidas/cenario${n}_eventos.csv
    tail -n +$((ca + 1)) logs/cargas.csv  > testes/saidas/cenario${n}_cargas.csv
    grep -h "AVISO" testes/saidas/cenario$n.txt
    if ls /dev/shm /dev/mqueue 2>/dev/null | grep -q atr; then
        echo "   ATENCAO: sobrou objeto de IPC"
    fi
done
