"""
app.py — Ponto de entrada da Tela de Operação (NiceGUI, no navegador).

Rodar (da raiz do repositório):
    interface/.venv/bin/python interface/app.py
e abrir http://localhost:8080 no navegador.

Estrutura:
  - UM cliente MQTT e UMA loja de estado para o programa todo
    (mqtt_cliente.py e estado.py).
  - Cada aba do navegador que abre a página ganha os seus componentes e o
    seu ui.timer, que a cada 200 ms pega uma CÓPIA do estado (com a trava)
    e atualiza os painéis. A thread do MQTT nunca mexe na interface.
  - A tela não decide nada: só mostra o que o C publica e envia comandos.
"""
from pathlib import Path

from nicegui import app, ui

import cores
from estado import Estado
from mqtt_cliente import ClienteMqtt
from paineis.cabecalho import Cabecalho
from paineis.caixas import PainelCaixas
from paineis.eventos import Notificador, PainelEventos
from paineis.mapa import PainelMapa
from paineis.simulacao import PainelSimulacao
from paineis.robos import PainelDetalhe, PainelRobos

PERIODO_TELA_S = 0.2   # mesmo período do atr/status

estado = Estado()
cliente_mqtt = ClienteMqtt(estado)
# Arquivos locais (fonte do mapa): a apresentação não depende de internet.
app.add_static_files("/static", Path(__file__).resolve().parent / "static")
app.on_startup(cliente_mqtt.iniciar)
app.on_shutdown(cliente_mqtt.parar)

CSS = f"""
@font-face {{ font-family: "Fredoka"; font-weight: 300 700;
             src: url("/static/fontes/Fredoka.woff2") format("woff2"); }}
body {{ background: {cores.FUNDO}; color: {cores.TEXTO}; }}
.titulo {{ font-size: 1.5rem; font-weight: 700; letter-spacing: .02em; }}
.subtitulo, .texto-fraco {{ color: {cores.TEXTO_FRACO}; font-size: .85rem; }}
.relogio {{ font-family: ui-monospace, monospace; font-size: 1.6rem; font-weight: 600; }}
.pilula {{ border: 1px solid; border-radius: 999px; padding: 2px 12px; font-size: .85rem; }}
.alerta-offline {{ background: {cores.ERRO}; color: white; font-weight: 700;
                   text-align: center; padding: 8px; border-radius: 8px; }}
.contador {{ background: {cores.PAINEL}; border: 1px solid {cores.BORDA};
             padding: 10px 16px; min-width: 120px; }}
.contador-numero {{ font-size: 1.8rem; font-weight: 700; line-height: 1; }}
.contador-titulo {{ color: {cores.TEXTO_FRACO}; font-size: .75rem; text-transform: uppercase;
                    letter-spacing: .06em; }}
.chip-estado {{ border: 1px solid; border-radius: 8px; padding: 2px 8px; }}
.chip-rotulo {{ color: {cores.TEXTO_FRACO}; font-size: .8rem; }}
.painel {{ background: {cores.PAINEL}; border: 1px solid {cores.BORDA}; }}
.painel-titulo {{ font-size: 1.1rem; font-weight: 600; }}
.grade-cards {{ display: grid; gap: 10px; grid-template-columns: repeat(auto-fill, minmax(230px, 1fr)); }}
.card-robo {{ background: {cores.PAINEL}; border: 1px solid {cores.BORDA}; border-left: 5px solid;
              padding: 8px 10px; gap: 4px; cursor: pointer; }}
.card-robo .botao-modo {{ min-width: 0; padding: 0 7px; font-size: .75rem; }}
.uma-linha {{ white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 100%; }}
.card-linha {{ color: {cores.TEXTO_FRACO}; font-size: .8rem; white-space: nowrap; overflow: hidden;
               text-overflow: ellipsis; width: 100%; }}
.card-robo.selecionado {{ outline: 2px solid #f8fafc; }}
.card-titulo {{ font-family: Fredoka, sans-serif; font-size: 1.05rem; font-weight: 600; line-height: 1.1; }}
.card-estado {{ font-weight: 600; font-size: .8rem; line-height: 1.1; }}
.pilula-modo {{ border: 1px solid {cores.BORDA}; border-radius: 999px; padding: 1px 10px;
                font-size: .75rem; color: {cores.TEXTO_FRACO}; }}
.miniatura {{ line-height: 0; }}
.bateria-txt {{ width: 3em; text-align: right; font-variant-numeric: tabular-nums; }}
.card-info {{ font-size: .85rem; }}
.rotulo-campo {{ color: {cores.TEXTO_FRACO}; }}
.botao-modo {{ min-width: 72px; }}
.campo-xy {{ width: 80px; }}
.dica {{ color: {cores.TEXTO_FRACO}; font-size: .78rem; }}
.dica-mapa {{ color: #fbbf24; font-size: .85rem; }}
.detalhe {{ padding: 6px 4px; }}
.detalhe-titulo {{ font-family: Fredoka, sans-serif; font-size: 1.5rem; font-weight: 600; }}
.secao {{ color: {cores.TEXTO_FRACO}; font-size: .75rem; text-transform: uppercase; letter-spacing: .06em; }}
.resposta {{ color: #93c5fd; font-size: .8rem; }}
.grafico {{ height: 190px; }}
.evento-hora {{ color: {cores.TEXTO_FRACO}; font-variant-numeric: tabular-nums; font-size: .8rem; }}
.evento-texto {{ font-size: .85rem; }}
.linha-inferior {{ display: grid; gap: 16px; width: 100%;
                   grid-template-columns: minmax(380px, 1.3fr) minmax(360px, 1.2fr) minmax(280px, .8fr); }}
@media (max-width: 1300px) {{ .linha-inferior {{ grid-template-columns: 1fr; }} }}
.aviso-espera {{ background: #3b2a08; border: 1px solid #b45309; color: #fcd34d; border-radius: 8px;
                 padding: 6px 10px; font-size: .85rem; }}
.tabela-caixas {{ background: transparent; max-height: 340px; }}
.caixa-tamanho {{ display: inline-block; min-width: 22px; padding: 1px 6px; border-radius: 4px;
                  color: #2B2A33; font-weight: 700; border: 1.5px solid #2B2A33; }}
.campo-caixa {{ width: 104px; }}
.lista-eventos {{ max-height: 420px; overflow-y: auto; }}
.linha-evento {{ padding: 2px 0; border-bottom: 1px solid {cores.BORDA}; }}
.evento-quem {{ font-size: .8rem; font-weight: 600; min-width: 62px; }}
.q-drawer {{ background: {cores.PAINEL} !important; border-left: 1px solid {cores.BORDA}; }}
.mapa {{ border-radius: 18px; overflow: hidden; }}
.legenda-mapa {{ background: #f4f0e8; border-radius: 14px; padding: 4px 8px; overflow-x: auto; }}
.legenda-mapa svg {{ max-width: 100%; height: auto; }}
"""


@ui.page("/")
def pagina():
    ui.add_css(CSS)
    enviar = cliente_mqtt.enviar_comando
    selecao = {"id": None}    # robô selecionado NESTA aba do navegador
    modo_clique = {"novo_robo": False}   # clique no mapa posiciona um novo robô?

    def selecionar(rid):
        selecao["id"] = rid
        mapa.selecionado = rid
        detalhe.selecionar(rid)
        gaveta.show()

    def fechar_detalhe():
        selecao["id"] = None
        mapa.selecionado = None
        gaveta.hide()

    # Gaveta da direita com o detalhe do robô selecionado (empurra a página,
    # não cobre o mapa).
    with ui.right_drawer(value=False, fixed=True).props("width=430") as gaveta:
        detalhe = PainelDetalhe(fechar_detalhe, enviar)
    with ui.column().classes("w-full p-4 gap-4"):
        cabecalho = Cabecalho()
        mapa = PainelMapa(selecionar, enviar, modo_clique)
        robos = PainelRobos(selecionar, enviar)
        with ui.element("div").classes("linha-inferior"):
            caixas = PainelCaixas(enviar)
            eventos = PainelEventos()
            simulacao = PainelSimulacao(enviar, modo_clique)
    notificador = Notificador()

    def atualizar():
        dados = estado.copia()   # cópia feita com a trava; o resto é fora dela
        cabecalho.atualizar(dados)
        mapa.atualizar(dados)
        robos.atualizar(dados, selecao["id"])
        caixas.atualizar(dados)
        eventos.atualizar(dados)
        simulacao.atualizar(dados)
        notificador.atualizar(dados)
        if selecao["id"] is not None:
            detalhe.atualizar(dados)

    atualizar()
    ui.timer(PERIODO_TELA_S, atualizar)


# host 127.0.0.1: só aceita conexões desta máquina (o WSL repassa o
#   localhost para o Windows, então o navegador do Windows abre normalmente).
# reload=False: sem recarregar ao editar arquivos (um processo só, simples).
# show=False: não tenta abrir navegador de dentro do WSL.
ui.run(host="127.0.0.1", port=8080, title="ATR · Tela de Operação",
       dark=True, reload=False, show=False, favicon="🤖")
