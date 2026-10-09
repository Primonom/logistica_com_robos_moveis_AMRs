"""
mapa.py — Painel do mapa do galpão (peça central da tela), estilo cartoon.

Usa ui.interactive_image:
  - "source" = SVG de FUNDO (mapa_svg.svg_fundo), montado uma vez, quando o
    atr/mapa chega (é "retained": chega logo na conexão). Vai com a fonte
    embutida, porque é exibido como imagem;
  - "content" = SVG VIVO (mapa_svg.svg_vivo), trocado a cada atualização.
As duas camadas têm o mesmo tamanho em pixels, então ficam alinhadas, e a
imagem ocupa toda a largura do painel (o navegador escala as duas juntas).
(No passo 13 o clique no mapa usa as coordenadas que o interactive_image
devolve e mapa_svg.celula_do_ponto.)

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
    def __init__(self):
        self.mapa = None          # último atr/mapa desenhado no fundo
        self.selecionado = None   # id do robô selecionado (passo 13)
        with ui.card().classes("painel w-full"):
            with ui.row().classes("w-full items-center justify-between"):
                ui.label("Mapa do galpão").classes("painel-titulo")
                self.aviso = ui.label("aguardando o mapa do sistema central (atr/mapa)…") \
                    .classes("texto-fraco")
            # sanitize=False: o SVG é gerado por nós (textos escapados em
            # desenhos.texto); o filtro padrão removeria <style> e <use>.
            self.imagem = ui.interactive_image(sanitize=False).classes("w-full mapa")
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
        self.imagem.set_content(mapa_svg.svg_vivo(mapa, dados["status"], self.selecionado,
                                                  dados["direcoes"], time.monotonic()))
