"""
eventos.py — Log de eventos ao vivo e notificações (toasts).

  PainelEventos  o atr/eventos ao vivo (mais recente em cima), com filtro por
                 robô e por tipo; cor e ícone pela categoria do evento
                 (cores.categoria_evento: só aparência, tirada do texto).
  Notificador    toasts para DEFEITO, bateria baixa e resposta de erro de
                 comando. Usa os números de sequência do estado.py para
                 avisar só do que é NOVO (ao abrir a página, nada antigo
                 aparece de novo).

Só desenha: recebem a CÓPIA do estado (ver estado.py) a cada atualização.
"""
from nicegui import ui

import cores

MAX_LINHAS = 150     # quantos eventos o log mostra (os mais recentes)


class PainelEventos:
    def __init__(self):
        self.mostrado = None          # (último seq, filtros) já desenhados
        with ui.card().classes("painel w-full h-full"):
            with ui.row().classes("w-full items-center justify-between no-wrap"):
                ui.label("Eventos").classes("painel-titulo")
                self.contagem = ui.label().classes("texto-fraco")
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                self.f_robo = ui.select({"todos": "todos os robôs"}, value="todos", label="robô") \
                    .props("dense outlined dark options-dense").classes("grow")
                tipos = {"todos": "todos os tipos"}
                tipos.update({k: v["rotulo"] for k, v in cores.CATEGORIAS_EVENTO.items()})
                self.f_tipo = ui.select(tipos, value="todos", label="tipo") \
                    .props("dense outlined dark options-dense").classes("grow")
            self.lista = ui.column().classes("w-full gap-0 lista-eventos")
        self.ids_robos = None

    def atualizar(self, dados):
        eventos = dados["eventos"]
        self._opcoes_robos(dados["status"])
        ultimo = eventos[-1]["seq"] if eventos else 0
        chave = (ultimo, self.f_robo.value, self.f_tipo.value)
        if chave == self.mostrado:                # nada novo e mesmos filtros
            return
        self.mostrado = chave
        filtrados = [e for e in eventos if self._passa(e)]
        self.contagem.set_text(f"{len(filtrados)} evento(s)")
        self.lista.clear()
        with self.lista:
            if not filtrados:
                ui.label("Nenhum evento com esses filtros.").classes("texto-fraco")
            for e in reversed(filtrados[-MAX_LINHAS:]):
                self._linha(e)

    def _passa(self, e):
        if self.f_robo.value != "todos" and e["robo"] != self.f_robo.value:
            return False
        return self.f_tipo.value == "todos" or cores.categoria_evento(e) == self.f_tipo.value

    def _opcoes_robos(self, status):
        ids = sorted(r["id"] for r in (status["robos"] if status else []))
        if ids != self.ids_robos:
            self.ids_robos = ids
            opcoes = {"todos": "todos os robôs", -1: "sistema / organizador"}
            opcoes.update({i: f"robô {i}" for i in ids})
            valor = self.f_robo.value if self.f_robo.value in opcoes else "todos"
            self.f_robo.set_options(opcoes, value=valor)

    @staticmethod
    def _linha(e):
        cat = cores.CATEGORIAS_EVENTO[cores.categoria_evento(e)]
        quem = "sistema" if e["robo"] == -1 else f"robô {e['robo']}"
        with ui.row().classes("w-full items-start no-wrap gap-2 linha-evento"):
            ui.icon(cat["icone"]).style(f"color: {cat['cor']}; font-size: 16px; margin-top: 2px")
            ui.label(e["data_hora"][11:23]).classes("evento-hora")
            ui.label(quem).classes("evento-quem").style(f"color: {cat['cor']}")
            ui.label(e["texto"]).classes("evento-texto")


class Notificador:
    """Toasts do que é novo: DEFEITO (vermelho), bateria baixa (âmbar) e
    resposta de erro de comando (vermelho)."""

    def __init__(self):
        self.visto_evento = None      # None = ainda não viu nada: a 1ª chamada só marca
        self.visto_resposta = None

    def atualizar(self, dados):
        eventos = dados["eventos"]
        ultimo = eventos[-1]["seq"] if eventos else 0
        if self.visto_evento is None:             # página acabou de abrir
            self.visto_evento, self.visto_resposta = ultimo, dados["seq_resposta"]
            return
        for e in eventos:
            if e["seq"] > self.visto_evento:
                self._avisar_evento(e)
        self.visto_evento = max(self.visto_evento, ultimo)
        if dados["seq_resposta"] > self.visto_resposta:
            self.visto_resposta = dados["seq_resposta"]
            resposta = dados["ultima_resposta"] or ""
            if resposta.startswith("erro:"):
                ui.notify(f"Comando recusado — {resposta[5:].strip()}", type="negative",
                          position="bottom-right", timeout=5000)

    @staticmethod
    def _avisar_evento(e):
        texto = e["texto"].lower()
        if "defeito:" in texto:
            ui.notify(f"Robô {e['robo']} em DEFEITO", type="negative", icon="warning",
                      position="bottom-right", timeout=6000)
        elif "bateria baixa" in texto:
            ui.notify(f"Robô {e['robo']}: bateria baixa, indo recarregar", type="warning",
                      icon="battery_alert", position="bottom-right", timeout=5000)
