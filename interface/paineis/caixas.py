"""
caixas.py — Painel das caixas: a fila em ordem de chegada e "Nova caixa".

  - Tabela: id, tamanho, baia, zona, status e equipe (com o líder), na ordem
    em que as caixas chegaram (a mesma ordem da fila do Organizador).
  - Destaque da caixa que o Organizador está TENTANDO atender e o motivo da
    espera. Quem diz isso é o C (campo "organizador" do atr/status): a tela
    não repete a regra da fila, só mostra.
  - "Nova caixa": tamanho 1–4, baia 1–4, zona A–D -> comando "caixa t b z".

Só desenha: atualizar(dados) recebe a CÓPIA do estado (ver estado.py). A
tabela só é reenviada ao navegador quando alguma linha muda.
"""
from nicegui import ui

import cores

COLUNAS = [
    {"name": "id",      "label": "Caixa",   "field": "id",      "align": "center"},
    {"name": "tamanho", "label": "Tamanho", "field": "tamanho", "align": "center"},
    {"name": "baia",    "label": "Baia",    "field": "baia",    "align": "center"},
    {"name": "zona",    "label": "Zona",    "field": "zona",    "align": "center"},
    {"name": "status",  "label": "Status",  "field": "status",  "align": "left"},
    {"name": "equipe",  "label": "Equipe",  "field": "equipe",  "align": "left"},
]
STATUS = {"ESPERANDO":    ("Esperando",    "#f59e0b"),
          "EM_ANDAMENTO": ("Em andamento", "#a855f7"),
          "ENTREGUE":     ("Entregue",     "#22c55e")}

# Células especiais (template Vue/Quasar). "props.row" é a linha de _linha().
CELULA_TAMANHO = """
<q-td :props="props">
  <span class="caixa-tamanho" :style="{background: props.row.cor_caixa}">{{ props.value }}</span>
</q-td>
"""
CELULA_STATUS = """
<q-td :props="props">
  <q-badge :style="{background: props.row.cor_status}" class="text-white q-pa-xs">{{ props.value }}</q-badge>
  <q-icon v-if="props.row.em_espera" name="hourglass_top" color="amber" size="18px" class="q-ml-xs"/>
</q-td>
"""


def _linha(c, em_espera):
    rotulo, cor = STATUS.get(c["status"], (c["status"], "#64748b"))
    if c["robos"]:
        equipe = " ".join(f"{r}★" if r == c["lider"] else str(r) for r in c["robos"])
    else:
        equipe = "—"
    return {"id": c["id"], "tamanho": c["tamanho"], "baia": c["baia"], "zona": c["zona"],
            "status": rotulo, "cor_status": cor, "equipe": equipe,
            "cor_caixa": cores.PAPELAO, "em_espera": c["id"] == em_espera}


class PainelCaixas:
    def __init__(self, enviar):
        self.enviar = enviar
        self.linhas = None
        with ui.card().classes("painel painel-caixas w-full h-full"):
            with ui.row().classes("w-full items-center justify-between"):
                ui.label("Caixas").classes("painel-titulo")
                self.mostrar_entregues = ui.switch("mostrar entregues", value=False) \
                    .props("dense dark color=green")
            self.espera = ui.label().classes("aviso-espera w-full")
            self.tabela = ui.table(columns=COLUNAS, rows=[], row_key="id") \
                .classes("w-full tabela-caixas").props("flat dense dark hide-no-data virtual-scroll")
            self.tabela.add_slot("body-cell-tamanho", CELULA_TAMANHO)
            self.tabela.add_slot("body-cell-status", CELULA_STATUS)
            self.vazio = ui.label("Nenhuma caixa na fila.").classes("texto-fraco")
            ui.separator().props("dark")
            ui.label("Nova caixa").classes("secao")
            with ui.row().classes("w-full items-center no-wrap gap-2"):
                self.tamanho = ui.select([1, 2, 3, 4], value=1, label="tamanho") \
                    .props("dense outlined dark").classes("campo-caixa")
                self.baia = ui.select([1, 2, 3, 4], value=1, label="baia") \
                    .props("dense outlined dark").classes("campo-caixa")
                self.zona = ui.select(["A", "B", "C", "D"], value="A", label="zona") \
                    .props("dense outlined dark").classes("campo-caixa")
                ui.button("Incluir caixa", icon="add_box", on_click=self._incluir) \
                    .props("dense no-caps unelevated color=amber-9")

    def _incluir(self):
        self.enviar(f"caixa {self.tamanho.value} {self.baia.value} {self.zona.value}")

    def atualizar(self, dados):
        status = dados["status"]
        caixas = status["caixas"] if status else []
        org = status.get("organizador", {}) if status else {}
        em_espera = org.get("caixa_em_espera", -1)
        self._aviso(caixas, em_espera, org.get("robos_livres", 0))
        if not self.mostrar_entregues.value:
            caixas = [c for c in caixas if c["status"] != "ENTREGUE"]
        linhas = [_linha(c, em_espera) for c in caixas]
        if linhas != self.linhas:                  # só reenvia se mudou
            self.linhas = linhas
            self.tabela.rows = linhas
            self.tabela.update()
        self.vazio.set_visibility(not linhas)

    def _aviso(self, caixas, em_espera, livres):
        """Motivo da espera (o que o Organizador publicou)."""
        c = next((c for c in caixas if c["id"] == em_espera), None)
        if c is None:
            self.espera.set_visibility(False)
            return
        self.espera.set_visibility(True)
        precisa = "1 robô livre" if c["tamanho"] == 1 else f"{c['tamanho']} robôs livres ao mesmo tempo"
        self.espera.set_text(
            f"⏳ Fila parada na caixa #{c['id']} (tamanho {c['tamanho']}, baia {c['baia']} → "
            f"zona {c['zona']}): precisa de {precisa} e há {livres}. Livre = disponível, em "
            f"modo automático e com bateria ≥ 30%. As caixas de trás esperam (ordem de chegada).")
