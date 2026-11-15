#!/bin/bash
# rodar.sh — Sobe o sistema inteiro (nativo, WSL/Ubuntu) com UM comando.
#
#   1) compila (make) se preciso;
#   2) broker MQTT: se ninguém escuta na porta 1883, inicia o mosquitto em
#      segundo plano (senão usa o que já está rodando);
#   3) Tela de Operação (Python/NiceGUI) em segundo plano, saída em
#      logs/interface.log; abrir http://localhost:8080 no navegador;
#   4) ./logistica em PRIMEIRO plano: o terminal vira o Console (digite
#      "status", "mapa", "novo", ... e "sair" para encerrar).
# Ao sair (comando "sair", fim da entrada ou Ctrl+C) mata só o que ESTE
# script iniciou (a interface e, se foi ele que iniciou, o mosquitto).
#
# Uso (da raiz do repositório ou de qualquer pasta):
#   ./rodar.sh                      # caixas de config/caixas.txt, mapa pela hora
#   ATR_SEMENTE=42 ./rodar.sh       # mapa fixo (o do vídeo e dos testes)
#   ./rodar.sh testes/sem_caixas.txt  # começa sem caixas (argumento repassado)
# Pré-requisitos: ver README.md ("Como rodar nativo").

cd "$(dirname "$0")" || exit 1   # caminhos relativos à raiz do repositório
mkdir -p logs

if [ ! -x interface/.venv/bin/python ]; then
    echo "Falta o ambiente Python da interface. Rode uma vez:"
    echo "  python3 -m venv interface/.venv"
    echo "  interface/.venv/bin/pip install -r interface/requirements.txt"
    exit 1
fi
make -s || exit 1

PID_MOSQUITTO=""
PID_INTERFACE=""

# Mata o que este script iniciou (chamada uma vez, na saída).
encerrar() {
    [ -n "$PID_INTERFACE" ] && kill "$PID_INTERFACE" 2>/dev/null
    [ -n "$PID_MOSQUITTO" ] && kill "$PID_MOSQUITTO" 2>/dev/null
    echo "rodar.sh: encerrado o que este script iniciou (interface${PID_MOSQUITTO:+ e mosquitto})."
}
trap encerrar EXIT
trap 'exit 130' INT TERM   # Ctrl+C: sai pelo caminho normal (roda o EXIT)

if ss -ltn 2>/dev/null | grep -q ':1883 '; then
    echo "rodar.sh: broker MQTT já rodando na porta 1883."
else
    mosquitto -p 1883 > logs/mosquitto.log 2>&1 &
    PID_MOSQUITTO=$!
    echo "rodar.sh: mosquitto iniciado (pid $PID_MOSQUITTO)."
fi

interface/.venv/bin/python interface/app.py > logs/interface.log 2>&1 &
PID_INTERFACE=$!
echo "rodar.sh: Tela de Operação iniciada (pid $PID_INTERFACE): http://localhost:8080"
echo "rodar.sh: Console do sistema abaixo (digite 'sair' para encerrar tudo)."

./logistica "$@"
