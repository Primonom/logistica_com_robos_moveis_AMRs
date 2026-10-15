"""
demonstracao.py — Cenário de demonstração (para gravar o vídeo).

Cinco atos, em sequência; no fim de cada um o cenário PAUSA até o operador
clicar "Próximo ato ▶" (tempo para narrar):
  1. Equipes se formando e o acoplamento síncrono.
  2. Uma passagem enchendo: semáforo vermelho, cancelas fechadas e um robô
     esperando na porta.
  3. Uma caixa de tamanho 4 esperando robôs (faixa âmbar no painel Caixas).
  4. Falha de um seguidor: a equipe recebe PAUSAR e, com "ok", CONTINUAR.
  5. Um robô com bateria baixa indo recarregar.

O cenário NÃO decide nada pelo sistema: só envia as mesmas linhas de comando
do Console ("novo", "caixa", "manual", "goto", "falha", "ok", "bateria"...) e
ESPERA condições no status/eventos (como o comando "aguarda" do Console).
Uma faixa no topo da página mostra o ato, o que observar e o que está
esperando, e a página rola sozinha até o painel que importa no ato (o mapa,
ou o painel Caixas no ato 3).

Concorrência: o roteiro é uma corrotina (asyncio) no laço do NiceGUI; ela lê
a loja de estado por estado.copia() (com a trava, como a tela) e dorme com
await asyncio.sleep, sem travar a interface.

Precisa do sistema recém-iniciado e sem caixas:
    ./logistica testes/sem_caixas.txt
"""
import asyncio
import time

from nicegui import background_tasks, ui

TOTAL_ATOS = 5


class Encerrado(Exception):
    """O operador clicou "Encerrar"."""


class Demonstracao:
    def __init__(self, estado, enviar):
        self.estado = estado
        self.enviar = enviar
        self.cliente = ui.context.client   # para rolar a página a partir da corrotina
        self.rodando = False
        self.continuar = asyncio.Event()
        self.cancelado = False
        with ui.card().classes("faixa-roteiro w-full") as self.faixa:
            with ui.row().classes("w-full items-center no-wrap gap-4"):
                ui.icon("movie").classes("text-3xl").style("color: #fbbf24")
                with ui.column().classes("gap-0 grow"):
                    self.titulo = ui.label().classes("roteiro-titulo")
                    self.texto = ui.label().classes("roteiro-texto")
                    self.esperando = ui.label().classes("roteiro-esperando")
                self.b_proximo = ui.button("Próximo ato ▶", on_click=self._proximo) \
                    .props("no-caps no-wrap unelevated color=amber-8 text-color=black")
                self.b_proximo.set_enabled(False)      # só habilita nas pausas
                ui.button("Encerrar", on_click=self._encerrar).props("no-caps flat color=grey-5")
        self.faixa.set_visibility(False)

    # ---------- Controle ----------

    def iniciar(self):
        if self.rodando:
            return
        dados = self.estado.copia()
        c = dados["status"]["contadores"] if dados["status"] else None
        if c is None or c["robos_ativos"] > 0 or c["pendentes"] > 0 or c["em_andamento"] > 0:
            ui.notify("O cenário precisa do sistema recém-iniciado e sem caixas: "
                      "./logistica testes/sem_caixas.txt", type="warning", timeout=7000)
            return
        self.rodando, self.cancelado = True, False
        self.faixa.set_visibility(True)
        background_tasks.create(self._roteiro())

    def _proximo(self):
        self.b_proximo.set_enabled(False)     # desabilita JÁ (evita clique duplo)
        self.continuar.set()

    def _rolar_para(self, classe):
        """Rola a página até o painel com essa classe CSS (mapa ou caixas)."""
        self.cliente.run_javascript(
            f"document.querySelector('.{classe}').scrollIntoView({{behavior: 'smooth', block: 'center'}})")

    def _encerrar(self):
        self.cancelado = True
        self.continuar.set()

    async def _roteiro(self):
        try:
            await self._ato1_equipes()
            await self._ato2_passagem()
            await self._ato3_caixa_grande()
            await self._ato4_falha_seguidor()
            await self._ato5_recarga()
            self._mostrar("Fim do cenário", "Obrigado! O sistema continua rodando normalmente.", "")
            await self._pausa()
        except Encerrado:
            pass
        except TimeoutError as erro:
            self._mostrar("Cenário interrompido", f"Demorou demais esperando: {erro}", "")
            try:
                await self._pausa()
            except Encerrado:
                pass
        finally:
            self.rodando = False
            self.faixa.set_visibility(False)

    # ---------- Utilitários ----------

    def _mostrar(self, titulo, texto, esperando, painel="painel-mapa"):
        self._rolar_para(painel)
        self.titulo.set_text(titulo)
        self.texto.set_text(texto)
        self.esperando.set_text(esperando)

    async def _pausa(self):
        """Espera o operador clicar "Próximo ato ▶" (tempo para narrar)."""
        self.continuar.clear()
        self.b_proximo.set_enabled(True)
        self.esperando.set_text("⏸ pausado — clique em “Próximo ato ▶” quando terminar de narrar")
        await self.continuar.wait()
        self.b_proximo.set_enabled(False)
        if self.cancelado:
            raise Encerrado()

    async def _esperar(self, condicao, descricao, timeout=90):
        """Espera condicao(dados) ser verdadeira (dados = cópia do estado),
        olhando a cada 0,2 s, como o "aguarda" do Console."""
        self.b_proximo.set_enabled(False)
        self.esperando.set_text(f"⏳ esperando: {descricao}")
        fim = time.monotonic() + timeout
        while time.monotonic() < fim:
            if self.cancelado:
                raise Encerrado()
            resultado = condicao(self.estado.copia())
            if resultado:
                return resultado
            await asyncio.sleep(0.2)
        raise TimeoutError(descricao)

    async def _evento(self, robo, trecho, desde):
        """Espera um evento novo (seq > desde) do robô com o trecho no texto."""
        def achou(d):
            return any(e["seq"] > desde and e["robo"] == robo and trecho in e["texto"]
                       for e in d["eventos"])
        await self._esperar(achou, f"robô {robo}: “{trecho}”")

    def _seq(self):
        eventos = self.estado.copia()["eventos"]
        return eventos[-1]["seq"] if eventos else 0

    @staticmethod
    def _robos(d):
        return {r["id"]: r for r in (d["status"]["robos"] if d["status"] else [])}

    async def _criar(self, posicoes):
        """Cria robôs (um "novo x y" por posição) e devolve os ids novos,
        quando todos estiverem DISPONIVEL."""
        antes = set(self._robos(self.estado.copia()))
        for x, y in posicoes:
            self.enviar(f"novo {x} {y}")
            await asyncio.sleep(0.15)

        def prontos(d):
            novos = {i: r for i, r in self._robos(d).items() if i not in antes}
            if len(novos) == len(posicoes) and all(r["estado"] == "DISPONIVEL" for r in novos.values()):
                return sorted(novos)
            return None
        return await self._esperar(prontos, f"{len(posicoes)} robô(s) novo(s) ficarem disponíveis")

    async def _todos_em(self, ids, estado, descricao):
        await self._esperar(lambda d: all(self._robos(d).get(i, {}).get("estado") == estado
                                          for i in ids), descricao)

    # ---------- Atos ----------

    async def _ato1_equipes(self):
        self._mostrar(f"Ato 1 de {TOTAL_ATOS} — Equipes e acoplamento síncrono",
                      "Três robôs a distâncias diferentes da baia 2 recebem uma caixa de tamanho 3. "
                      "Quem chega primeiro espera (laranja, AGUARDANDO_EQUIPE); quando o último chega, "
                      "o Organizador manda ACOPLAR a todos no mesmo ciclo e a caixa aparece por cima "
                      "da equipe.", "")
        # distâncias bem diferentes: o primeiro espera uns 4–5 s pelos outros
        self.equipe1 = await self._criar([(8, 15), (27, 15), (36, 25)])
        self.enviar("caixa 3 2 C")
        await self._esperar(lambda d: any(r["estado"] == "AGUARDANDO_EQUIPE"
                                          for r in self._robos(d).values()),
                            "o primeiro robô chegar à baia 2")
        await self._todos_em(self.equipe1, "OPERANDO_CARGA",
                             "os outros chegarem e todos erguerem a caixa (ACOPLAR)")
        await self._pausa()
        self._mostrar(f"Ato 1 de {TOTAL_ATOS} — Transporte",
                      "No transporte só o LÍDER (menor id) usa os semáforos das passagens; os "
                      "seguidores copiam o setpoint dele. A equipe ocupa 1 vaga.", "")
        await self._esperar(lambda d: all(self._robos(d)[i]["estado"] == "DISPONIVEL"
                                          for i in self.equipe1), "a caixa ser entregue na zona C")

    async def _ato2_passagem(self):
        self._mostrar(f"Ato 2 de {TOTAL_ATOS} — Passagem enchendo (P2, capacidade 2)",
                      "Dois robôs em modo manual atravessam a P2 e param lá dentro (simulamos uma "
                      "parada): parados, continuam ocupando a vaga. Com 2/2 o semáforo fica "
                      "vermelho e as cancelas fecham; o terceiro robô espera na porta.", "")
        a, b, c = self.passagem = await self._criar([(34, 15), (35, 12), (30, 20)])
        for i in (a, b, c):
            self.enviar(f"manual {i}")
        await self._todos_em([a, b, c], "MANUAL", "os três ficarem em modo manual")
        desde = self._seq()
        self.enviar(f"goto {a} 50 15")
        self.enviar(f"goto {b} 50 12")
        await self._evento(a, "entrou na passagem P2", desde)
        self.enviar(f"falha {a}")
        await self._evento(b, "entrou na passagem P2", desde)
        self.enviar(f"falha {b}")
        self.enviar(f"goto {c} 50 20")
        await self._evento(c, "esperando passagem P2", desde)
        await self._pausa()
        self._mostrar(f"Ato 2 de {TOTAL_ATOS} — A passagem libera",
                      "Os dois robôs voltam a andar e saem da P2 (sem_post): o robô que esperava "
                      "consegue a vaga (sem_timedwait) e atravessa.", "")
        desde = self._seq()
        self.enviar(f"ok {a}")
        self.enviar(f"ok {b}")
        await self._evento(c, "saiu da passagem P2", desde)

    async def _ato3_caixa_grande(self):
        self._mostrar(f"Ato 3 de {TOTAL_ATOS} — Caixa de tamanho 4 esperando robôs",
                      "Só há 3 robôs automáticos livres. A caixa de tamanho 4 fica esperando (faixa "
                      "âmbar no painel Caixas) e o Organizador NÃO reserva ninguém (sem equipe "
                      "parcial = sem deadlock). Ao criar o 4º robô, a equipe se forma.", "",
                      painel="painel-caixas")
        self.enviar("caixa 4 4 B")
        await self._esperar(lambda d: (d["status"] or {}).get("organizador", {})
                            .get("caixa_em_espera", -1) != -1, "a caixa entrar em espera")
        await self._pausa()
        self._mostrar(f"Ato 3 de {TOTAL_ATOS} — O 4º robô chega",
                      "Novo robô criado: agora há 4 livres e a caixa é atribuída à equipe de 4.", "")
        novo = await self._criar([(22, 26)])
        self.equipe4 = self.equipe1 + novo
        await self._todos_em(self.equipe4, "OPERANDO_CARGA", "a equipe de 4 erguer a caixa")

    async def _ato4_falha_seguidor(self):
        lider = min(self.equipe4)
        seguidor = max(self.equipe4)
        self._mostrar(f"Ato 4 de {TOTAL_ATOS} — Falha de um seguidor",
                      f"No meio do transporte o robô {seguidor} (seguidor) entra em DEFEITO: o "
                      f"Organizador manda PAUSAR para a equipe inteira, que segura a posição.", "")
        await self._esperar(lambda d: len(self._robos(d)[lider].get("rota", [])) > 0,
                            "a equipe começar o transporte")
        await asyncio.sleep(2.5)
        desde = self._seq()
        self.enviar(f"falha {seguidor}")
        await self._evento(-1, "PAUSAR", desde)
        await self._pausa()
        self._mostrar(f"Ato 4 de {TOTAL_ATOS} — Defeito resolvido",
                      f"Com “ok {seguidor}” o defeito sai, o Organizador manda CONTINUAR e a equipe "
                      f"segue viagem.", "")
        desde = self._seq()
        self.enviar(f"ok {seguidor}")
        await self._evento(-1, "CONTINUAR", desde)
        await asyncio.sleep(3)

    async def _ato5_recarga(self):
        robo = self.passagem[2]
        self._mostrar(f"Ato 5 de {TOTAL_ATOS} — Bateria baixa: indo recarregar",
                      f"O robô {robo} volta ao modo automático e a bateria dele cai para 15%. Sem "
                      f"carga e abaixo de 20%, a thread Status manda recarregar: ele vai à estação "
                      f"mais próxima (passando pela P3, capacidade 1) e volta a DISPONIVEL com 100%.", "")
        self.enviar(f"auto {robo}")
        await self._todos_em([robo], "DISPONIVEL", f"o robô {robo} ficar disponível")
        self.enviar(f"bateria {robo} 15")
        await self._todos_em([robo], "RECARREGANDO", f"o robô {robo} ir recarregar")
        await self._esperar(lambda d: self._robos(d)[robo]["bateria"] > 40 and
                            self._robos(d)[robo]["estado"] == "RECARREGANDO",
                            f"o robô {robo} chegar à estação e carregar")
        await self._pausa()
        await self._todos_em([robo], "DISPONIVEL", f"o robô {robo} terminar a recarga (100%)")
