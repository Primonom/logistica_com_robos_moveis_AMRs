"""
cabecalho.py — Faixa do topo: conexão, relógio do sistema e contadores.

Só desenha (não guarda estado). __init__ cria os componentes uma vez;
atualizar(dados) é chamado pelo ui.timer com uma CÓPIA do estado
(ver estado.py) e só troca textos e cores.
"""
from nicegui import ui

import cores


def _contador(titulo, icone):
    """Um quadrinho "número grande + título". Retorna o label do número."""
    with ui.card().classes("contador"):
        with ui.row().classes("items-center no-wrap gap-2"):
            ui.icon(icone).classes("text-xl").style(f"color: {cores.TEXTO_FRACO}")
            numero = ui.label("–").classes("contador-numero")
        ui.label(titulo).classes("contador-titulo")
    return numero


class Cabecalho:
    def __init__(self):
        with ui.row().classes("w-full items-center justify-between no-wrap"):
            with ui.row().classes("items-center gap-3 no-wrap"):
                ui.icon("precision_manufacturing").classes("text-4xl").style("color: #60a5fa")
                with ui.column().classes("gap-0"):
                    ui.label("Logística com AMRs").classes("titulo")
                    ui.label("Tela de Operação · ATR 2026/2").classes("subtitulo")
            with ui.row().classes("items-center gap-4 no-wrap"):
                self.broker = ui.label().classes("pilula")
                self.sistema = ui.label().classes("pilula")
                with ui.column().classes("gap-0 items-end"):
                    ui.label("relógio do sistema").classes("contador-titulo")
                    self.relogio = ui.label("--:--:--").classes("relogio")

        # Faixa vermelha que só aparece quando o sistema central some.
        self.alerta_offline = ui.label(
            "SISTEMA OFFLINE — nenhum status do sistema central há mais de 2 s"
        ).classes("alerta-offline w-full")

        with ui.row().classes("w-full gap-3 items-stretch"):
            self.n_robos = _contador("robôs ativos", "smart_toy")
            self.n_pendentes = _contador("caixas pendentes", "pending_actions")
            self.n_andamento = _contador("em andamento", "local_shipping")
            self.n_entregues = _contador("entregues", "task_alt")
            self.n_por_minuto = _contador("entregas / min", "speed")
            # Um chip por estado, na ordem da tabela de cores.
            with ui.card().classes("contador grow"):
                ui.label("robôs por estado").classes("contador-titulo")
                with ui.row().classes("gap-2"):
                    self.chips = {}
                    for nome, c in cores.ESTADOS.items():
                        with ui.row().classes("chip-estado items-center no-wrap gap-1") \
                                .style(f"border-color: {c['cor']}"):
                            ui.icon(c["icone"]).style(f"color: {c['cor']}")
                            self.chips[nome] = ui.label("0")
                            ui.label(c["rotulo"]).classes("chip-rotulo")

    def atualizar(self, dados):
        self._conexao(dados["broker_conectado"], dados["sistema_online"])
        status = dados["status"]
        if status is None:
            return
        self.relogio.set_text(status["t"][11:19])   # só HH:MM:SS
        c = status["contadores"]
        self.n_robos.set_text(str(c["robos_ativos"]))
        self.n_pendentes.set_text(str(c["pendentes"]))
        self.n_andamento.set_text(str(c["em_andamento"]))
        self.n_entregues.set_text(str(c["entregues"]))
        self.n_por_minuto.set_text(f"{dados['entregas_por_minuto']:.1f}")
        for nome in self.chips:
            n = sum(1 for r in status["robos"] if r["estado"] == nome)
            self.chips[nome].set_text(str(n))

    def _conexao(self, broker, sistema):
        self._pilula(self.broker, broker, "broker conectado", "broker desconectado")
        self._pilula(self.sistema, sistema, "sistema central online", "sistema central offline")
        self.alerta_offline.set_visibility(not sistema)

    @staticmethod
    def _pilula(label, ligado, texto_ok, texto_erro):
        cor = cores.OK if ligado else cores.ERRO
        label.set_text(("● " if ligado else "○ ") + (texto_ok if ligado else texto_erro))
        label.style(f"color: {cor}; border-color: {cor}")
