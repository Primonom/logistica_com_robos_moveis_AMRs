"""
simulacao.py — Ações de "hardware" (itens 8a–8d do enunciado: a Tela de
Operação também faz o papel da tela do simulador).

  - Novo robô: por x/y, na posição padrão, ou clicando no mapa (liga o
    "modo de clique" da página: o próximo clique no chão envia "novo x y").
  - Gerar defeito / tirar defeito e baixar a bateria de qualquer robô
    (escolhido na lista), sem precisar abrir o detalhe.
  - Incluir caixa: o formulário fica no painel Caixas.

Tudo vira uma linha de comando igual à do Console ("novo 10 5", "falha 2",
"bateria 3 15"); quem valida e decide é o C.
"""
from nicegui import ui


class PainelSimulacao:
    def __init__(self, enviar, modo_clique):
        """modo_clique: dicionário da página {"novo_robo": bool}, lido pelo
        clique do mapa."""
        self.enviar = enviar
        self.modo_clique = modo_clique
        self.ids = None
        with ui.card().classes("painel w-full h-full"):
            ui.label("Simulação").classes("painel-titulo")
            ui.label("Novo robô").classes("secao")
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                self.x = ui.number("x", min=0, precision=0).props("dense outlined dark").classes("campo-xy")
                self.y = ui.number("y", min=0, precision=0).props("dense outlined dark").classes("campo-xy")
                ui.button("Criar", icon="smart_toy", on_click=self._novo) \
                    .props("dense no-caps unelevated color=primary")
            self.b_mapa = ui.button("Posicionar clicando no mapa", icon="ads_click",
                                    on_click=self._alternar_mapa) \
                .props("dense no-caps outline color=amber").classes("w-full")
            ui.label("Sem x/y, o robô nasce na posição padrão do Gestor.").classes("dica")
            ui.separator().props("dark")
            ui.label("Defeito e bateria").classes("secao")
            self.robo = ui.select({}, label="robô").props("dense outlined dark").classes("w-full")
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                ui.button("Gerar falha", icon="warning", on_click=lambda: self._cmd("falha")) \
                    .props("dense no-caps unelevated color=red-8")
                ui.button("OK", icon="build", on_click=lambda: self._cmd("ok")) \
                    .props("dense no-caps unelevated color=green-8")
            with ui.row().classes("w-full items-center no-wrap gap-2 pt-6"):  # espaço p/ o balão
                self.bateria = ui.slider(min=0, max=100, value=15).props("label-always dark") \
                    .classes("grow")
                ui.button("Bateria", icon="battery_alert",
                          on_click=lambda: self._cmd("bateria", f" {int(self.bateria.value)}")) \
                    .props("dense no-caps no-wrap unelevated color=amber-9")
            ui.label("Caixas novas: formulário no painel Caixas.").classes("dica")

    def _novo(self):
        if self.x.value is not None and self.y.value is not None:
            self.enviar(f"novo {int(self.x.value)} {int(self.y.value)}")
        else:
            self.enviar("novo")

    def _alternar_mapa(self):
        self.modo_clique["novo_robo"] = not self.modo_clique["novo_robo"]

    def _cmd(self, comando, extra=""):
        if self.robo.value is not None:
            self.enviar(f"{comando} {self.robo.value}{extra}")

    def atualizar(self, dados):
        status = dados["status"]
        ids = sorted(r["id"] for r in (status["robos"] if status else []))
        if ids != self.ids:                       # lista de robôs mudou
            self.ids = ids
            self.robo.set_options({i: f"Robô {i}" for i in ids},
                                  value=self.robo.value if self.robo.value in ids else None)
        ativo = self.modo_clique["novo_robo"]
        self.b_mapa.set_text("Clique no chão do mapa… (cancelar)" if ativo
                             else "Posicionar clicando no mapa")
        self.b_mapa.props(f"color={'red' if ativo else 'amber'}")
