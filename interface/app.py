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
from nicegui import app, ui

import cores
from estado import Estado
from mqtt_cliente import ClienteMqtt
from paineis.cabecalho import Cabecalho
from paineis.robos import TabelaRobos

PERIODO_TELA_S = 0.2   # mesmo período do atr/status

estado = Estado()
cliente_mqtt = ClienteMqtt(estado)
app.on_startup(cliente_mqtt.iniciar)
app.on_shutdown(cliente_mqtt.parar)

CSS = f"""
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
.tabela {{ background: transparent; }}
"""


@ui.page("/")
def pagina():
    ui.add_css(CSS)
    with ui.column().classes("w-full p-4 gap-4"):
        cabecalho = Cabecalho()
        robos = TabelaRobos()

    def atualizar():
        dados = estado.copia()   # cópia feita com a trava; o resto é fora dela
        cabecalho.atualizar(dados)
        robos.atualizar(dados)

    atualizar()
    ui.timer(PERIODO_TELA_S, atualizar)


# host 127.0.0.1: só aceita conexões desta máquina (o WSL repassa o
#   localhost para o Windows, então o navegador do Windows abre normalmente).
# reload=False: sem recarregar ao editar arquivos (um processo só, simples).
# show=False: não tenta abrir navegador de dentro do WSL.
ui.run(host="127.0.0.1", port=8080, title="ATR · Tela de Operação",
       dark=True, reload=False, show=False, favicon="🤖")
