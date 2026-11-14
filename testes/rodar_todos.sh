#!/bin/bash
# Roda os cenários 1 a 10 e salva os resultados em testes/saidas/:
#   cenarioN.txt          saída da tela (Console, Gestor, Organizador...)
#   cenarioN_eventos.csv  linhas que o cenário acrescentou a logs/eventos.csv
#   cenarioN_cargas.csv   linhas que o cenário acrescentou a logs/cargas.csv
# O mapa é sorteado a cada execução; para os cenários serem reprodutíveis
# a semente é fixa (ATR_SEMENTE):
#   cenários 1-4: semente 42 (a mesma do vídeo de demonstração);
#   cenário 5:    rodado duas vezes, sementes 1 e 2 (mostra o mapa mudando);
#   cenário 6:    semente 5 (passagem de capacidade 1 dividindo a parede
#                 com outra passagem, para o replanejamento ter opção);
#   cenário 7:    semente 42 (bateria suficiente para a tarefa inteira;
#                 roda ~3 minutos e confere que nenhum robô chegou a 0%);
#   cenários 8-10: semente 42 (anti-colisão, ponto extra +2: frente a
#                 frente, mão única na passagem e 6 robôs ao mesmo tempo).
#                 Conferem que o simulador não registrou nenhuma COLISAO.
# Uso (de qualquer pasta): testes/rodar_todos.sh

cd "$(dirname "$0")/.." || exit 1   # raiz do repositório
make || exit 1
mkdir -p testes/saidas

# Número de linhas de um arquivo (0 se ainda não existe).
linhas() {
    if [ -f "$1" ]; then wc -l < "$1"; else echo 0; fi
}

# rodar <cenário> <semente> <nome da saída>
rodar() {
    echo "== cenario $1 (semente $2) =="
    ev=$(linhas logs/eventos.csv)
    ca=$(linhas logs/cargas.csv)

    ATR_SEMENTE=$2 timeout 240 ./logistica testes/sem_caixas.txt < testes/cenario$1.txt \
        > testes/saidas/$3.txt 2>&1
    echo "   codigo de saida: $?"

    tail -n +$((ev + 1)) logs/eventos.csv > testes/saidas/${3}_eventos.csv
    tail -n +$((ca + 1)) logs/cargas.csv  > testes/saidas/${3}_cargas.csv
    grep -h "AVISO" testes/saidas/$3.txt
    if ls /dev/shm /dev/mqueue 2>/dev/null | grep -q atr; then
        echo "   ATENCAO: sobrou objeto de IPC"
    fi
}

for n in 1 2 3 4; do
    rodar $n 42 cenario$n
done
rodar 5 1 cenario5_semente1
rodar 5 2 cenario5_semente2
rodar 6 5 cenario6
rodar 7 42 cenario7
# No cenário 7 nenhum robô pode entrar em DEFEITO (bateria 0%).
if grep -q "DEFEITO" testes/saidas/cenario7_eventos.csv; then
    echo "   AVISO: robo chegou a 0% de bateria (DEFEITO) no cenario 7"
fi
for n in 8 9 10; do
    rodar $n 42 cenario$n
    # Anti-colisão: o simulador registra "COLISAO" se dois robôs de grupos
    # diferentes ficarem na mesma célula (fora de baia/zona/estação).
    if grep -q "COLISAO" testes/saidas/cenario${n}_eventos.csv; then
        echo "   AVISO: COLISAO registrada no cenario $n"
    fi
done
# Cenário 8: um dos dois desviou e os dois chegaram.
grep -q "desviando de robo" testes/saidas/cenario8_eventos.csv ||
    echo "   AVISO: ninguem desviou no cenario 8"
[ "$(grep -c "chegou ao destino" testes/saidas/cenario8_eventos.csv)" -eq 2 ] ||
    echo "   AVISO: algum robo nao chegou no cenario 8"
# Cenário 9: os dois atravessaram a P1 e chegaram.
[ "$(grep -c "saiu da passagem P1" testes/saidas/cenario9_eventos.csv)" -eq 2 ] ||
    echo "   AVISO: algum robo nao atravessou a P1 no cenario 9"
[ "$(grep -c "chegou ao destino" testes/saidas/cenario9_eventos.csv)" -eq 2 ] ||
    echo "   AVISO: algum robo nao chegou no cenario 9"
# Cenário 10: as 6 caixas entregues.
[ "$(wc -l < testes/saidas/cenario10_cargas.csv)" -eq 6 ] ||
    echo "   AVISO: nem todas as 6 caixas foram entregues no cenario 10"
