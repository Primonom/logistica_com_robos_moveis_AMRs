"""
tempos.py — Card "Tempo real": tabela do comando "tempos" do C.

  - A cada 1 s envia o comando "tempos" (atr/comando). A resposta chega em
    atr/comando/resposta e o estado.py a guarda à parte (ver
    guardar_resposta), já convertida em linhas.
  - Uma linha por tarefa periódica: T, ciclos, C médio, C máximo, atraso
    máximo, perdas de prazo e U = Cmax/T. Navegação e Status mostram o
    pior caso entre todos os robôs (quem calcula é o C).
  - Perdas > 0 ficam em vermelho.

Só desenha: atualizar(dados) recebe a CÓPIA do estado (ver estado.py).
"""
from nicegui import ui

PERIODO_TEMPOS_S = 1.0

COLUNAS = [
    {"name": "tarefa",     "label": "Tarefa",          "field": "tarefa",     "align": "left"},
    {"name": "T",          "label": "T (ms)",          "field": "T",          "align": "right"},
    {"name": "Cmed",       "label": "C méd (µs)",      "field": "Cmed",       "align": "right"},
    {"name": "Cmax",       "label": "C máx (µs)",      "field": "Cmax",       "align": "right"},
    {"name": "atraso_max", "label": "Atraso máx (µs)", "field": "atraso_max", "align": "right"},
    {"name": "perdas",     "label": "Perdas",          "field": "perdas",     "align": "right"},
    {"name": "U",          "label": "U = Cmax/T",      "field": "U",          "align": "right"},
]

CELULA_PERDAS = """
<q-td :props="props" :style="{color: props.value > 0 ? '#f87171' : 'inherit',
                              fontWeight: props.value > 0 ? 700 : 400}">{{ props.value }}</q-td>
"""


def _linha(t):
    return {"tarefa": t["tarefa"], "T": t["T"], "Cmed": f'{t["Cmed"]:.1f}',
            "Cmax": f'{t["Cmax"]:.1f}', "atraso_max": f'{t["atraso_max"]:.1f}',
            "perdas": t["perdas"], "U": f'{t["U"]:.4f}'}


class PainelTempos:
    def __init__(self, enviar):
        self.linhas = None
        with ui.card().classes("painel w-full"):
            with ui.row().classes("w-full items-center justify-between"):
                ui.label("Tempo real").classes("painel-titulo")
                ui.button("Zerar", on_click=lambda: enviar("tempos zerar")).props("flat dense")
            self.tabela = ui.table(columns=COLUNAS, rows=[], row_key="tarefa").props(
                "dense flat").classes("w-full")
            self.tabela.add_slot("body-cell-perdas", CELULA_PERDAS)
            ui.label("Navegação e Status: pior caso entre os robôs. "
                     "Atraso = início real − instante planejado.").classes("dica")
        ui.timer(PERIODO_TEMPOS_S, lambda: enviar("tempos"))

    def atualizar(self, dados):
        linhas = [_linha(t) for t in dados["tempos"]]
        if linhas != self.linhas:      # só reenvia ao navegador se mudou
            self.linhas = linhas
            self.tabela.rows = linhas
            self.tabela.update()
