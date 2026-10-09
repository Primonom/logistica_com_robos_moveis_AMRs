"""
cores.py — Tabela ÚNICA de cores, ícones e rótulos por estado do robô.

Usada em todo lugar (tabela, mapa, cards, legenda, gráficos), para o mesmo
estado ter sempre a mesma cor. Cada estado tem também um ícone e um texto:
a tela nunca depende só da cor.
Ícones: nomes do Material Icons (já vêm com o NiceGUI).
"""

# Cores um pouco mais saturadas/escuras: ficam bonitas sobre o piso claro do
# mapa e continuam legíveis no tema escuro do resto da tela.
ESTADOS = {
    "DISPONIVEL":        {"cor": "#16a34a", "icone": "check_circle",  "rotulo": "Disponível"},
    "DESLOCANDO":        {"cor": "#2563eb", "icone": "navigation",    "rotulo": "Deslocando"},
    "AGUARDANDO_EQUIPE": {"cor": "#d97706", "icone": "groups",        "rotulo": "Aguardando equipe"},
    "OPERANDO_CARGA":    {"cor": "#9333ea", "icone": "inventory_2",   "rotulo": "Operando carga"},
    "RECARREGANDO":      {"cor": "#0891b2", "icone": "battery_charging_full", "rotulo": "Recarregando"},
    "DEFEITO":           {"cor": "#dc2626", "icone": "warning",       "rotulo": "Defeito"},
    "MANUAL":            {"cor": "#ea580c", "icone": "sports_esports", "rotulo": "Manual"},
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


# ---------- Paleta do mapa (estilo cartoon, "tabuleiro claro") ----------
CONTORNO = "#2B2A33"          # contorno grosso de tudo (nunca preto puro)
SOMBRA = "#2B2A33"            # sombra projetada (com opacidade baixa)
PISO = "#E9E4DA"
PISO_PLACA = "#E2DCD0"        # placas de piso alternadas
PAREDE = "#5B5F6B"
PILAR = "#474A54"
FAIXA = "#F6C945"             # corredor / faixas de circulação
FAIXA_BORDA = "#E0A800"
PAPELAO = "#C99A5B"
PAPELAO_FITA = "#E8D3A6"
PAPELAO_SOMBRA = "#A87B42"
MADEIRA = "#B98250"           # paletes
MADEIRA_ESCURA = "#8C5E34"
BAIAS = ["#CFE6F5", "#D6EDC9", "#F7EBB5", "#F9D6B5"]   # azul, verde, amarelo, laranja
ZONAS = ["#F2A7A0", "#BFE3A8", "#A9CFEF", "#CDB8E8"]   # vermelho, verde, azul, lilás
RECARGA = "#3A6F86"
RAIO = "#FFD23F"
ADVERTENCIA = ("#F6C945", "#2B2A33")                  # listras amarelo/preto
TEXTO_MAPA = "#2B2A33"
EQUIPE = "#7c3aed"            # contorno da equipe indo para a baia
