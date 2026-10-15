"""
graficos.py — Gráficos da frota (ui.echart, do próprio NiceGUI).

  1. Robôs por estado (barras, nas cores da tabela única de cores.py).
  2. Bateria de todos os robôs ao longo do tempo (uma linha por robô).
  3. Entregas acumuladas ao longo do tempo.

Os dados vêm da CÓPIA do estado (ver estado.py): o histórico tem uma amostra
por segundo dos últimos 5 minutos. Os gráficos só são redesenhados quando
chega amostra nova (uma vez por segundo), não a cada 200 ms.
"""
from nicegui import ui

import cores

TEXTO = {"color": "#94a3b8"}
GRADE = {"lineStyle": {"color": "#1f2b45"}}
# Uma cor por robô no gráfico de bateria (distintas das cores de estado não é
# necessário: aqui a cor identifica o robô, e a legenda diz qual é qual).
CORES_ROBOS = ["#60a5fa", "#f472b6", "#34d399", "#fbbf24", "#a78bfa", "#f87171", "#22d3ee",
               "#a3e635", "#fb923c", "#e879f9", "#2dd4bf", "#facc15", "#818cf8", "#4ade80",
               "#fca5a5", "#93c5fd"]


def _eixo_tempo():
    return {"type": "value", "name": "s", "max": 0, "nameGap": 4,
            "axisLabel": dict(TEXTO, showMinLabel=False), "splitLine": {"show": False}}


def _base(titulo):
    return {"backgroundColor": "transparent",
            "title": {"text": titulo, "textStyle": {"color": "#e5e7eb", "fontSize": 13,
                                                    "fontWeight": 600}, "left": 4, "top": 2},
            "grid": {"left": 44, "right": 14, "top": 44, "bottom": 28},
            "tooltip": {"trigger": "axis"}}


class PainelGraficos:
    def __init__(self):
        self.ultima_amostra = None
        with ui.card().classes("painel w-full"):
            ui.label("Gráficos").classes("painel-titulo")
            with ui.element("div").classes("grade-graficos"):
                self.por_estado = ui.echart(self._opcoes_estados({})).classes("grafico-frota")
                self.baterias = ui.echart(self._opcoes_baterias({})).classes("grafico-frota")
                self.entregas = ui.echart(self._opcoes_entregas([])).classes("grafico-frota")

    def atualizar(self, dados):
        hist = dados["historico_entregas"]
        if not hist or hist[-1][0] == self.ultima_amostra:   # só com amostra nova
            return
        self.ultima_amostra = agora = hist[-1][0]
        robos = dados["status"]["robos"] if dados["status"] else []
        contagem = {nome: 0 for nome in cores.ESTADOS}
        for r in robos:
            contagem[r["estado"]] = contagem.get(r["estado"], 0) + 1
        ativos = {r["id"] for r in robos}
        baterias = {rid: [[round(t - agora, 1), b] for t, b, _ in h]
                    for rid, h in sorted(dados["historico"].items()) if rid in ativos}
        entregas = [[round(t - agora, 1), e] for t, e in hist]
        for grafico, opcoes in ((self.por_estado, self._opcoes_estados(contagem)),
                                (self.baterias, self._opcoes_baterias(baterias)),
                                (self.entregas, self._opcoes_entregas(entregas))):
            grafico.options.clear()
            grafico.options.update(opcoes)
            grafico.update()

    @staticmethod
    def _opcoes_estados(contagem):
        nomes = list(cores.ESTADOS)
        o = _base("Robôs por estado")
        o["tooltip"] = {"trigger": "item"}
        o["grid"]["bottom"] = 58
        o["xAxis"] = {"type": "category", "data": [cores.ESTADOS[n]["rotulo"] for n in nomes],
                      "axisLabel": dict(TEXTO, interval=0, rotate=30, fontSize=10)}
        o["yAxis"] = {"type": "value", "minInterval": 1, "axisLabel": TEXTO, "splitLine": GRADE}
        o["series"] = [{"type": "bar", "barWidth": "60%",
                        "data": [{"value": contagem.get(n, 0),
                                  "itemStyle": {"color": cores.ESTADOS[n]["cor"],
                                                "borderRadius": [4, 4, 0, 0]}} for n in nomes],
                        "label": {"show": True, "position": "top", "color": "#e5e7eb"}}]
        return o

    @staticmethod
    def _opcoes_baterias(baterias):
        o = _base("Bateria dos robôs (%)")
        o["legend"] = {"type": "scroll", "top": 22, "textStyle": TEXTO, "itemHeight": 8}
        o["grid"]["top"] = 56
        o["xAxis"] = _eixo_tempo()
        o["yAxis"] = {"type": "value", "min": 0, "max": 100, "axisLabel": TEXTO, "splitLine": GRADE}
        o["series"] = [{"name": f"robô {rid}", "type": "line", "data": pontos, "showSymbol": False,
                        "color": CORES_ROBOS[rid % len(CORES_ROBOS)], "lineStyle": {"width": 1.8}}
                       for rid, pontos in baterias.items()]
        return o

    @staticmethod
    def _opcoes_entregas(entregas):
        o = _base("Entregas acumuladas")
        o["xAxis"] = _eixo_tempo()
        o["yAxis"] = {"type": "value", "minInterval": 1, "axisLabel": TEXTO, "splitLine": GRADE}
        o["series"] = [{"type": "line", "step": "end", "data": entregas, "showSymbol": False,
                        "color": "#22c55e", "lineStyle": {"width": 2.5},
                        "areaStyle": {"opacity": 0.15}}]
        return o
