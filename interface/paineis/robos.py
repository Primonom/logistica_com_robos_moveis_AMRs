"""
robos.py — Cards dos robôs, painel de detalhe do robô selecionado e controles.

  Controles     botões Auto/Manual, "ir para…" (baias, zonas, recargas, com as
                coordenadas do atr/mapa) e goto por x/y. O card usa só os
                botões de modo (completo=False); o detalhe usa tudo.
  CardRobo      card COMPACTO por robô (cabem 4–5 por linha): miniatura do
                robô (o MESMO desenho do mapa, na cor do estado), id, estado,
                botões Auto/Manual, bateria e uma linha com posição e
                carga/equipe. Clicar no card abre o detalhe.
  PainelRobos   a grade de cards (cria/remove cards quando robôs aparecem/somem).
  PainelDetalhe o robô selecionado: tudo do card + gráfico de bateria e
                distância + linha do tempo dos eventos dele + simulação
                (falha/ok, bateria).

Regras (a tela não decide nada, só evita comandos que o C recusaria):
  - os botões Auto/Manual só MOSTRAM o modo que o C publica; clicar envia o
    comando ("auto 3" / "manual 3"). Manual só com o robô DISPONIVEL; Auto
    só em MANUAL (as mesmas regras do C, para o operador não clicar à toa);
  - goto (lista, x/y ou clique no mapa) só em modo MANUAL;
  - controle desabilitado mostra uma dica dizendo por quê.

Tudo é atualizado pelo ui.timer a partir da CÓPIA do estado (ver estado.py).
"""
from nicegui import ui

import cores
import mapa_svg


def _ponto(x, y):
    return "—" if x < 0 else f"({x}, {y})"


def destinos_rapidos(mapa):
    """Opções do "ir para…" a partir do atr/mapa: {"x y": "Baia 1  (3, 4)", ...}."""
    if mapa is None:
        return {}
    opcoes = {}
    for b in mapa["baias"]:
        opcoes[f"{b['x']} {b['y']}"] = f"Baia {b['id']}  ({b['x']}, {b['y']})"
    for z in mapa["zonas"]:
        opcoes[f"{z['x']} {z['y']}"] = f"Zona {z['id']}  ({z['x']}, {z['y']})"
    for k, e in enumerate(mapa["estacoes"]):
        opcoes[f"{e['x']} {e['y']}"] = f"Recarga {k + 1}  ({e['x']}, {e['y']})"
    return opcoes


def texto_equipe(r):
    if r["lider"] == -1:
        return "—"
    if r["lider"] == r["id"]:
        return "líder"
    return f"equipe do {r['lider']}"


class Controles:
    """Auto/Manual + "ir para…" + goto x/y de UM robô (self.rid pode mudar:
    o painel de detalhe troca de robô quando outro é selecionado)."""

    def __init__(self, rid, enviar, completo=True):
        self.rid = rid
        self.enviar = enviar
        self.completo = completo
        self.opcoes_destino = {}
        with ui.row().classes("items-center no-wrap gap-1" + (" w-full" if completo else "")):
            if completo:
                ui.label("Modo").classes("rotulo-campo")
            self.b_auto = ui.button("Auto", on_click=lambda: self.enviar(f"auto {self.rid}")) \
                .props("dense no-caps unelevated").classes("botao-modo")
            self.b_manual = ui.button("Manual", on_click=lambda: self.enviar(f"manual {self.rid}")) \
                .props("dense no-caps unelevated").classes("botao-modo")
            self.b_auto.mostrando = self.b_manual.mostrando = None   # (ativo, habilitado)
        if not completo:
            return
        with ui.row().classes("w-full items-center no-wrap gap-2"):
            self.ir_para = ui.select({}, label="Ir para…", on_change=self._ir_para) \
                .props("dense outlined dark options-dense").classes("grow")
        with ui.row().classes("w-full items-center no-wrap gap-2"):
            self.x = ui.number("x", min=0, precision=0).props("dense outlined dark").classes("campo-xy")
            self.y = ui.number("y", min=0, precision=0).props("dense outlined dark").classes("campo-xy")
            self.b_goto = ui.button("Goto", icon="near_me", on_click=self._goto) \
                .props("dense no-caps unelevated color=primary")
        self.dica = ui.label().classes("dica")

    def _ir_para(self, evento):
        if evento.value:                      # None = quando a lista é limpa abaixo
            self.enviar(f"goto {self.rid} {evento.value}")
            self.ir_para.set_value(None)

    def _goto(self):
        if self.x.value is not None and self.y.value is not None:
            self.enviar(f"goto {self.rid} {int(self.x.value)} {int(self.y.value)}")

    @staticmethod
    def _botao_modo(botao, ativo, pode):
        """Ativo: azul, habilitado mas sem clique (classe no-pointer-events),
        para não parecer "apagado". Inativo: cinza, clicável só se "pode"."""
        if botao.mostrando == (ativo, pode):          # nada mudou: não reenvia
            return
        botao.mostrando = (ativo, pode)
        botao.props(f"color={'primary' if ativo else 'grey-8'}")
        botao.classes(replace="botao-modo" + (" no-pointer-events" if ativo else ""))
        botao.set_enabled(ativo or pode)

    def atualizar(self, r, opcoes_destino):
        if self.completo and opcoes_destino != self.opcoes_destino:
            self.opcoes_destino = opcoes_destino
            self.ir_para.set_options(opcoes_destino)
        automatico = r["modo"] == "auto"
        # o modo atual (o que o C diz) fica destacado e não clicável; o outro
        # só fica habilitado quando o C aceitaria a troca
        self._botao_modo(self.b_auto, ativo=automatico, pode=r["estado"] == "MANUAL")
        self._botao_modo(self.b_manual, ativo=not automatico,
                         pode=automatico and r["estado"] == "DISPONIVEL")
        if not self.completo:
            return
        em_manual = r["estado"] == "MANUAL"
        for controle in (self.ir_para, self.x, self.y, self.b_goto):
            controle.set_enabled(em_manual)
        if em_manual:
            self.dica.set_text("Goto: escolha um destino, digite x/y ou clique no mapa.")
        elif automatico and r["estado"] != "DISPONIVEL":
            self.dica.set_text("Manual só com o robô DISPONÍVEL (agora está em "
                               f"{cores.do_estado(r['estado'])['rotulo'].lower()}).")
        else:
            self.dica.set_text("Goto só em modo manual.")


class CardRobo:
    def __init__(self, rid, ao_selecionar, enviar):
        self.rid = rid
        self.estado_desenhado = None
        with ui.card().classes("card-robo").on("click", lambda: ao_selecionar(rid)) as self.card:
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                self.miniatura = ui.html("", sanitize=False).classes("miniatura")
                with ui.column().classes("gap-0 grow min-w-0"):   # min-w-0: deixa cortar o texto
                    ui.label(f"Robô {rid}").classes("card-titulo")
                    self.estado = ui.label().classes("card-estado uma-linha")
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                self.bateria = ui.linear_progress(show_value=False, color=None, size="8px") \
                    .props("rounded track-color=grey-9").classes("grow")
                self.bateria_txt = ui.label().classes("bateria-txt")
                self.controles = Controles(rid, enviar, completo=False)
            self.linha = ui.label().classes("card-linha")

    def atualizar(self, r, selecionado, opcoes_destino):
        estilo = cores.do_estado(r["estado"])
        if r["estado"] != self.estado_desenhado:          # só redesenha se mudou
            self.estado_desenhado = r["estado"]
            self.miniatura.set_content(mapa_svg.svg_miniatura_robo(r["estado"], 44))
        self.estado.set_text(estilo["rotulo"])
        self.estado.style(f"color: {estilo['cor']}")
        self.bateria.set_value(r["bateria"] / 100)
        self.bateria.style(f"color: {cores.da_bateria(r['bateria'])}")
        self.bateria_txt.set_text(f"{r['bateria']}%")
        partes = [_ponto(r["x"], r["y"])]
        if r["carga"] != -1:
            partes.append(f"caixa #{r['carga']} · {texto_equipe(r)}")
        self.linha.set_text("  ·  ".join(partes))
        self.card.classes(replace="card-robo" + (" selecionado" if selecionado else ""))
        self.card.style(f"border-left-color: {estilo['cor']}")
        self.controles.atualizar(r, opcoes_destino)


class PainelRobos:
    """Grade de cards: um por robô ativo, na ordem dos ids."""

    def __init__(self, ao_selecionar, enviar):
        self.ao_selecionar = ao_selecionar
        self.enviar = enviar
        self.cards = {}
        with ui.card().classes("painel w-full"):
            ui.label("Robôs").classes("painel-titulo")
            self.vazio = ui.label("Nenhum robô ativo. Crie um pelo Console: novo [x y]") \
                .classes("texto-fraco")
            self.grade = ui.element("div").classes("grade-cards w-full")

    def atualizar(self, dados, selecionado):
        status = dados["status"]
        robos = {r["id"]: r for r in (status["robos"] if status else [])}
        for rid in list(self.cards):                       # robôs que sumiram
            if rid not in robos:
                self.cards.pop(rid).card.delete()
        opcoes = destinos_rapidos(dados["mapa"])
        for rid in sorted(robos):
            if rid not in self.cards:                      # robô novo
                with self.grade:
                    self.cards[rid] = CardRobo(rid, self.ao_selecionar, self.enviar)
                self._ordenar()
            self.cards[rid].atualizar(robos[rid], rid == selecionado, opcoes)
        self.vazio.set_visibility(not robos)

    def _ordenar(self):
        for posicao, rid in enumerate(sorted(self.cards)):
            self.cards[rid].card.move(self.grade, target_index=posicao)


class PainelDetalhe:
    """Painel do robô selecionado (fica na gaveta da direita)."""

    def __init__(self, ao_fechar, enviar):
        self.rid = None
        self.enviar = enviar
        self.estado_desenhado = None
        self.ultimo_evento = None
        self.ultima_amostra = None
        with ui.column().classes("w-full gap-3 detalhe"):
            with ui.row().classes("w-full items-center no-wrap gap-3"):
                self.miniatura = ui.html("", sanitize=False).classes("miniatura")
                with ui.column().classes("gap-0 grow"):
                    self.titulo = ui.label().classes("detalhe-titulo")
                    self.estado = ui.label().classes("card-estado")
                ui.button(icon="close", on_click=ao_fechar).props("flat round dense color=grey-5")
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                self.bateria = ui.linear_progress(show_value=False, color=None, size="12px") \
                    .props("rounded track-color=grey-9").classes("grow")
                self.bateria_txt = ui.label().classes("bateria-txt")
            with ui.grid(columns=2).classes("w-full gap-x-3 gap-y-0 card-info"):
                self.campos = {}
                for nome in ("Modo", "Posição", "Setpoint", "Destino", "Distância", "Carga",
                             "Equipe", "Waypoints"):
                    ui.label(nome).classes("rotulo-campo")
                    self.campos[nome] = ui.label()
            ui.separator().props("dark")
            self.controles = Controles(None, enviar)
            self.resposta = ui.label().classes("resposta")
            ui.separator().props("dark")
            ui.label("Simulação do hardware").classes("secao")
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                ui.button("Gerar falha", icon="warning", on_click=lambda: self._cmd("falha")) \
                    .props("dense no-caps unelevated color=red-8")
                ui.button("OK (sem falha)", icon="build", on_click=lambda: self._cmd("ok")) \
                    .props("dense no-caps unelevated color=green-8")
            with ui.row().classes("w-full items-center no-wrap gap-2 pt-6"):  # espaço p/ o balão
                self.slider = ui.slider(min=0, max=100, value=50).props("label-always dark") \
                    .classes("grow")
                ui.button("Bateria", icon="battery_alert",
                          on_click=lambda: self._cmd("bateria", f" {int(self.slider.value)}")) \
                    .props("dense no-caps no-wrap unelevated color=amber-9")
            ui.separator().props("dark")
            ui.label("Bateria e distância (últimos 5 min)").classes("secao")
            self.grafico = ui.echart(self._opcoes_grafico([], [])).classes("w-full grafico")
            ui.label("Eventos deste robô").classes("secao")
            self.eventos = ui.column().classes("w-full gap-1 eventos")

    def _cmd(self, comando, extra=""):
        if self.rid is not None:
            self.enviar(f"{comando} {self.rid}{extra}")

    def selecionar(self, rid):
        self.rid = rid
        self.controles.rid = rid
        self.estado_desenhado = self.ultimo_evento = self.ultima_amostra = None

    def atualizar(self, dados):
        status = dados["status"]
        r = next((r for r in (status["robos"] if status else []) if r["id"] == self.rid), None)
        if r is None:
            self.titulo.set_text(f"Robô {self.rid}: não está mais ativo")
            return
        estilo = cores.do_estado(r["estado"])
        if r["estado"] != self.estado_desenhado:
            self.estado_desenhado = r["estado"]
            self.miniatura.set_content(mapa_svg.svg_miniatura_robo(r["estado"], 72))
        self.titulo.set_text(f"Robô {r['id']}")
        self.estado.set_text(estilo["rotulo"])
        self.estado.style(f"color: {estilo['cor']}")
        self.bateria.set_value(r["bateria"] / 100)
        self.bateria.style(f"color: {cores.da_bateria(r['bateria'])}")
        self.bateria_txt.set_text(f"{r['bateria']}%")
        valores = {"Modo": "Automático" if r["modo"] == "auto" else "Manual",
                   "Posição": _ponto(r["x"], r["y"]), "Setpoint": _ponto(r["sp_x"], r["sp_y"]),
                   "Destino": _ponto(r["destino_x"], r["destino_y"]),
                   "Distância": f"{r['distancia']:.0f} células",
                   "Carga": "—" if r["carga"] == -1 else f"caixa #{r['carga']}",
                   "Equipe": texto_equipe(r), "Waypoints": str(len(r.get("rota", [])))}
        for nome, valor in valores.items():
            self.campos[nome].set_text(valor)
        self.controles.atualizar(r, destinos_rapidos(dados["mapa"]))
        if dados["ultima_resposta"]:
            self.resposta.set_text(f"Última resposta do sistema: {dados['ultima_resposta']}")
        self._atualizar_grafico(dados["historico"].get(self.rid, []))
        self._atualizar_eventos(dados["eventos"])

    @staticmethod
    def _opcoes_grafico(bateria, distancia):
        return {
            "backgroundColor": "transparent",
            "grid": {"left": 40, "right": 46, "top": 26, "bottom": 28},
            "legend": {"data": ["bateria (%)", "distância"], "textStyle": {"color": "#94a3b8"},
                       "top": 0},
            "tooltip": {"trigger": "axis"},
            "xAxis": {"type": "value", "name": "s", "max": 0, "nameGap": 4,
                      "axisLabel": {"color": "#94a3b8", "showMinLabel": False},
                      "splitLine": {"show": False}},
            "yAxis": [{"type": "value", "min": 0, "max": 100, "axisLabel": {"color": "#94a3b8"},
                       "splitLine": {"lineStyle": {"color": "#1f2b45"}}},
                      {"type": "value", "axisLabel": {"color": "#94a3b8"},
                       "splitLine": {"show": False}}],
            "series": [{"name": "bateria (%)", "type": "line", "data": bateria, "showSymbol": False,
                        "lineStyle": {"width": 2.5}, "color": "#22c55e", "areaStyle": {"opacity": 0.12}},
                       {"name": "distância", "type": "line", "yAxisIndex": 1, "data": distancia,
                        "showSymbol": False, "color": "#60a5fa"}],
        }

    def _atualizar_grafico(self, historico):
        """Só troca os dados quando chega amostra nova (uma por segundo)."""
        if not historico or historico[-1][0] == self.ultima_amostra:
            return
        self.ultima_amostra = agora = historico[-1][0]
        bateria = [[round(t - agora, 1), b] for t, b, _ in historico]
        distancia = [[round(t - agora, 1), dist] for t, _, dist in historico]
        self.grafico.options.update(self._opcoes_grafico(bateria, distancia))
        self.grafico.update()

    def _atualizar_eventos(self, eventos):
        """Linha do tempo dos eventos DESTE robô (mais recente em cima).
        Só reconstrói quando chega evento novo."""
        meus = [e for e in eventos if e["robo"] == self.rid]
        ultimo = (len(meus), meus[-1]["data_hora"]) if meus else None
        if ultimo == self.ultimo_evento:
            return
        self.ultimo_evento = ultimo
        self.eventos.clear()
        with self.eventos:
            if not meus:
                ui.label("Nenhum evento ainda.").classes("texto-fraco")
            for e in reversed(meus[-14:]):
                cor = cores.CATEGORIAS_EVENTO[cores.categoria_evento(e)]["cor"]
                with ui.row().classes("items-start no-wrap gap-2 w-full"):
                    ui.icon("circle").style(f"color: {cor}; font-size: 10px; margin-top: 5px")
                    ui.label(e["data_hora"][11:19]).classes("evento-hora")
                    ui.label(e["texto"]).classes("evento-texto")
