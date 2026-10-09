"""
robos.py — Painel dos robôs.

Passo 11: uma TABELA simples, atualizada ao vivo, para provar que os dados
chegam do C. (Os cards com controles entram no passo 13.)

Só desenha: atualizar(dados) recebe a cópia do estado (ver estado.py) e
troca as linhas da tabela.
"""
from nicegui import ui

import cores

COLUNAS = [
    {"name": "id",        "label": "Robô",      "field": "id",        "align": "center"},
    {"name": "estado",    "label": "Estado",    "field": "estado",    "align": "left"},
    {"name": "modo",      "label": "Modo",      "field": "modo",      "align": "center"},
    {"name": "posicao",   "label": "Posição",   "field": "posicao",   "align": "center"},
    {"name": "setpoint",  "label": "Setpoint",  "field": "setpoint",  "align": "center"},
    {"name": "destino",   "label": "Destino",   "field": "destino",   "align": "center"},
    {"name": "bateria",   "label": "Bateria",   "field": "bateria",   "align": "left"},
    {"name": "distancia", "label": "Distância", "field": "distancia", "align": "right"},
    {"name": "carga",     "label": "Carga",     "field": "carga",     "align": "center"},
    {"name": "equipe",    "label": "Equipe",    "field": "equipe",    "align": "left"},
]

# Como desenhar duas células especiais (template Vue/Quasar do NiceGUI):
# "props.row" é a linha montada em _linha() abaixo.
CELULA_ESTADO = """
<q-td :props="props">
  <q-badge :style="{background: props.row.cor}" class="text-white q-pa-xs">
    <q-icon :name="props.row.icone" size="16px" class="q-mr-xs"/>{{ props.row.estado }}
  </q-badge>
</q-td>
"""
CELULA_BATERIA = """
<q-td :props="props">
  <div class="row items-center no-wrap" style="gap: 6px">
    <q-linear-progress :value="props.row.bateria / 100" size="8px" rounded
        style="width: 70px" :style="{color: props.row.cor_bateria}" track-color="grey-9"/>
    <span :style="{color: props.row.cor_bateria}">{{ props.row.bateria }}%</span>
  </div>
</q-td>
"""


def _ponto(x, y):
    return "—" if x < 0 else f"({x}, {y})"


def _linha(r):
    """Converte um robô do atr/status numa linha da tabela."""
    estilo = cores.do_estado(r["estado"])
    if r["lider"] == -1:
        equipe = "—"
    elif r["lider"] == r["id"]:
        equipe = "líder"
    else:
        equipe = f"equipe do {r['lider']}"
    return {
        "id": r["id"],
        "estado": estilo["rotulo"],
        "cor": estilo["cor"],
        "icone": estilo["icone"],
        "modo": "Auto" if r["modo"] == "auto" else "Manual",
        "posicao": _ponto(r["x"], r["y"]),
        "setpoint": _ponto(r["sp_x"], r["sp_y"]),
        "destino": _ponto(r["destino_x"], r["destino_y"]),
        "bateria": r["bateria"],
        "cor_bateria": cores.da_bateria(r["bateria"]),
        "distancia": f"{r['distancia']:.0f}",
        "carga": "—" if r["carga"] == -1 else f"#{r['carga']}",
        "equipe": equipe,
    }


class TabelaRobos:
    def __init__(self):
        with ui.card().classes("painel w-full"):
            ui.label("Robôs").classes("painel-titulo")
            self.tabela = ui.table(columns=COLUNAS, rows=[], row_key="id") \
                .classes("w-full tabela").props("flat dense dark hide-no-data")
            self.tabela.add_slot("body-cell-estado", CELULA_ESTADO)
            self.tabela.add_slot("body-cell-bateria", CELULA_BATERIA)
            self.vazio = ui.label("Nenhum robô ativo. Crie um pelo Console: novo [x y]") \
                .classes("texto-fraco")

    def atualizar(self, dados):
        status = dados["status"]
        robos = status["robos"] if status else []
        self.tabela.rows = [_linha(r) for r in robos]
        self.tabela.update()
        self.vazio.set_visibility(len(robos) == 0)
