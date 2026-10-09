"""
cores.py — Tabela ÚNICA de cores, ícones e rótulos por estado do robô.

Usada em todo lugar (tabela, mapa, cards, legenda, gráficos), para o mesmo
estado ter sempre a mesma cor. Cada estado tem também um ícone e um texto:
a tela nunca depende só da cor.
Ícones: nomes do Material Icons (já vêm com o NiceGUI).
"""

ESTADOS = {
    "DISPONIVEL":        {"cor": "#22c55e", "icone": "check_circle",  "rotulo": "Disponível"},
    "DESLOCANDO":        {"cor": "#3b82f6", "icone": "navigation",    "rotulo": "Deslocando"},
    "AGUARDANDO_EQUIPE": {"cor": "#f59e0b", "icone": "groups",        "rotulo": "Aguardando equipe"},
    "OPERANDO_CARGA":    {"cor": "#a855f7", "icone": "inventory_2",   "rotulo": "Operando carga"},
    "RECARREGANDO":      {"cor": "#06b6d4", "icone": "battery_charging_full", "rotulo": "Recarregando"},
    "DEFEITO":           {"cor": "#ef4444", "icone": "warning",       "rotulo": "Defeito"},
    "MANUAL":            {"cor": "#f97316", "icone": "sports_esports", "rotulo": "Manual"},
    "DESLIGADO":         {"cor": "#6b7280", "icone": "power_off",     "rotulo": "Desligado"},
}

# Para estados que não estão na tabela (não deveria acontecer).
DESCONHECIDO = {"cor": "#6b7280", "icone": "help", "rotulo": "?"}

# Cores gerais do tema escuro "sala de controle".
FUNDO = "#0b1220"
PAINEL = "#111a2e"
BORDA = "#1f2b45"
TEXTO = "#e5e7eb"
TEXTO_FRACO = "#94a3b8"
OK = "#22c55e"
ALERTA = "#f59e0b"
ERRO = "#ef4444"


def do_estado(estado):
    """Cor, ícone e rótulo de um estado (nome igual ao do C)."""
    return ESTADOS.get(estado, DESCONHECIDO)


def da_bateria(pct):
    """Cor da bateria: vermelho < 20% (recarga), âmbar < 30% (não pega tarefa)."""
    if pct < 20:
        return ERRO
    if pct < 30:
        return ALERTA
    return OK
