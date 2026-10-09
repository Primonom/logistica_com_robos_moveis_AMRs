"""
mapa.py — Painel do mapa do galpão (peça central da tela), estilo cartoon.

Usa ui.interactive_image:
  - "source" = SVG de FUNDO (mapa_svg.svg_fundo), montado uma vez, quando o
    atr/mapa chega (é "retained": chega logo na conexão). Vai com a fonte
    embutida, porque é exibido como imagem;
  - "content" = SVG VIVO (mapa_svg.svg_vivo), trocado a cada atualização.
As duas camadas têm o mesmo tamanho em pixels, então ficam alinhadas, e a
imagem ocupa toda a largura do painel (o navegador escala as duas juntas).

Clique no mapa (o interactive_image devolve o ponto em pixels da imagem,
já desfazendo a escala):
  - em cima de um robô (mapa_svg.robo_no_ponto): seleciona o robô;
  - no chão, com um robô em MANUAL selecionado: envia "goto <id> <x> <y>"
    para a célula clicada (mapa_svg.celula_do_ponto); o C valida e responde.

Só desenha: atualizar(dados) recebe a cópia do estado (ver estado.py).
"""
import base64
import time
from pathlib import Path

from nicegui import ui

import mapa_svg

ARQUIVO_FONTE = Path(__file__).resolve().parent.parent / "static" / "fontes" / "Fredoka.woff2"
FONTE_BASE64 = base64.b64encode(ARQUIVO_FONTE.read_bytes()).decode("ascii")


def _data_uri(svg):
    """SVG em texto -> endereço "data:" que o navegador abre como imagem."""
    return "data:image/svg+xml;base64," + base64.b64encode(svg.encode("utf-8")).decode("ascii")


class PainelMapa:
    def __init__(self, ao_selecionar, enviar):
        self.ao_selecionar = ao_selecionar
        self.enviar = enviar
        self.mapa = None          # último atr/mapa desenhado no fundo
        self.selecionado = None   # id do robô selecionado
        self.status = None        # último status e direções desenhados (para o clique)
        self.direcoes = {}
        with ui.card().classes("painel w-full"):
            with ui.row().classes("w-full items-center justify-between"):
                ui.label("Mapa do galpão").classes("painel-titulo")
                self.aviso = ui.label("aguardando o mapa do sistema central (atr/mapa)…") \
                    .classes("texto-fraco")
                self.dica = ui.label("Clique num robô para selecionar.").classes("dica-mapa")
            # sanitize=False: o SVG é gerado por nós (textos escapados em
            # desenhos.texto); o filtro padrão removeria <style> e <use>.
            self.imagem = ui.interactive_image(sanitize=False, on_mouse=self._clique,
                                               events=["click"]).classes("w-full mapa")
            with ui.element("div").classes("w-full legenda-mapa"):
                ui.html(mapa_svg.svg_legenda(), sanitize=False)

    def atualizar(self, dados):
        mapa = dados["mapa"]
        if mapa is None:
            return
        if mapa != self.mapa:                       # primeiro mapa (ou mudou)
            self.mapa = mapa
            self.imagem.set_source(_data_uri(mapa_svg.svg_fundo(mapa, FONTE_BASE64)))
            self.aviso.set_visibility(False)
        self.status, self.direcoes = dados["status"], dados["direcoes"]
        self.imagem.set_content(mapa_svg.svg_vivo(mapa, dados["status"], self.selecionado,
                                                  dados["direcoes"], time.monotonic()))
        self._atualizar_dica()

    def _robo_selecionado(self):
        robos = self.status["robos"] if self.status else []
        return next((r for r in robos if r["id"] == self.selecionado), None)

    def _atualizar_dica(self):
        r = self._robo_selecionado()
        if r is None:
            texto = "Clique num robô para selecionar."
        elif r["estado"] == "MANUAL":
            texto = f"Robô {r['id']} em MANUAL: clique no chão para mandá-lo até lá."
        else:
            texto = f"Robô {r['id']} selecionado. Para mandá-lo a um ponto, coloque-o em Manual."
        self.dica.set_text(texto)

    def _clique(self, evento):
        """Robô no ponto -> seleciona; chão + robô MANUAL selecionado -> goto."""
        rid = mapa_svg.robo_no_ponto(evento.image_x, evento.image_y, self.status, self.direcoes)
        if rid is not None:
            self.ao_selecionar(rid)
            return
        r = self._robo_selecionado()
        if r is not None and r["estado"] == "MANUAL":
            x, y = mapa_svg.celula_do_ponto(evento.image_x, evento.image_y)
            self.enviar(f"goto {r['id']} {x} {y}")
