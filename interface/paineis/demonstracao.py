"""
demonstracao.py — Cenário de demonstração (para gravar o vídeo).

Cinco atos, em sequência; no fim de cada um o cenário PAUSA até o operador
clicar "Próximo ato ▶" (tempo para narrar):
  1. Equipes se formando (escolhidas pelo caminho A*) e o acoplamento síncrono.
  2. A passagem de capacidade 2 enchendo: semáforo vermelho, cancelas
     fechadas e um robô esperando na porta.
  3. Uma caixa de tamanho 4 esperando robôs (faixa âmbar no painel Caixas).
  4. Falha de um seguidor: a equipe recebe PAUSAR e, com "ok", CONTINUAR.
  5. Um robô com bateria baixa indo recarregar.

O cenário NÃO decide nada pelo sistema: cria robôs como o simulador
(atr/robo/novo, por enviar_novo_robo), envia as mesmas linhas de comando do
Console ("caixa", "manual", "goto", "falha", "ok", "bateria"...) e ESPERA
condições no status/eventos (como o comando "aguarda" do Console).
Uma faixa no topo da página mostra o ato, a semente do mapa, o que observar
e o que está esperando, e a página rola sozinha até o painel que importa no
ato (o mapa, ou o painel Caixas no ato 3).

O mapa é sorteado a cada execução do C. Por isso NENHUMA coordenada é fixa
aqui: as posições saem do atr/mapa (baias, passagens). PARA GRAVAR O VÍDEO,
use a semente 42 (a mesma dos cenários de teste), para o mapa do vídeo ser
sempre o mesmo:
    ATR_SEMENTE=42 ./logistica testes/sem_caixas.txt

Concorrência: o roteiro é uma corrotina (asyncio) no laço do NiceGUI; ela lê
a loja de estado por estado.copia() (com a trava, como a tela) e dorme com
await asyncio.sleep, sem travar a interface.

Precisa do sistema recém-iniciado e sem caixas (ver o comando acima).
"""
import asyncio
import time

from nicegui import background_tasks, ui

TOTAL_ATOS = 5


class Encerrado(Exception):
    """O operador clicou "Encerrar"."""


class Demonstracao:
    def __init__(self, estado, enviar, enviar_novo_robo):
        """enviar(linha): atr/comando. enviar_novo_robo(x, y): atr/robo/novo
        (sem x/y = posição padrão), como o painel Simulação."""
        self.estado = estado
        self.enviar = enviar
        self.enviar_novo_robo = enviar_novo_robo
        self.cliente = ui.context.client   # para rolar a página a partir da corrotina
        self.rodando = False
        self.continuar = asyncio.Event()
        self.cancelado = False
        with ui.card().classes("faixa-roteiro w-full") as self.faixa:
            with ui.row().classes("w-full items-center no-wrap gap-4"):
                ui.icon("movie").classes("text-3xl").style("color: #fbbf24")
                with ui.column().classes("gap-0 grow"):
                    with ui.row().classes("items-baseline gap-3 no-wrap"):
                        self.titulo = ui.label().classes("roteiro-titulo")
                        self.semente = ui.label().classes("roteiro-esperando")
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
        if (dados["mapa"] is None or c is None or c["robos_ativos"] > 0 or c["pendentes"] > 0
                or c["em_andamento"] > 0):
            ui.notify("O cenário precisa do sistema recém-iniciado e sem caixas: "
                      "ATR_SEMENTE=42 ./logistica testes/sem_caixas.txt", type="warning", timeout=7000)
            return
        self.mapa = dados["mapa"]          # o mapa não muda durante a execução do C
        semente = self.mapa["semente"]
        self.semente.set_text(f"mapa: semente {semente}" +
                              ("" if semente == 42 else " (o vídeo usa a 42)"))
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
        """Cria robôs como o simulador: um pedido em atr/robo/novo por posição
        ((x, y), ou None = posição padrão do Gestor). Devolve os ids novos
        quando todos estiverem DISPONIVEL."""
        antes = set(self._robos(self.estado.copia()))
        for pos in posicoes:
            if pos is None:
                self.enviar_novo_robo()
            else:
                self.enviar_novo_robo(*pos)
            await asyncio.sleep(0.15)

        def prontos(d):
            novos = {i: r for i, r in self._robos(d).items() if i not in antes}
            if len(novos) == len(posicoes) and all(r["estado"] == "DISPONIVEL" for r in novos.values()):
                return sorted(novos)
            return None
        return await self._esperar(prontos, f"{len(posicoes)} robô(s) novo(s) ficarem disponíveis")

    def _baia(self, numero):
        b = self.mapa["baias"][numero - 1]
        return b["x"], b["y"]

    def _passagem(self, capacidade):
        """A passagem com essa capacidade (cada capacidade 1..4 aparece uma
        vez) e as colunas das bocas: (passagem, x_boca_esquerda,
        x_boca_direita, y)."""
        pas = next(p for p in self.mapa["passagens"] if p["capacidade"] == capacidade)
        xs = [x for x, _ in pas["regiao"]]
        return pas, min(xs), max(xs), pas["regiao"][0][1]

    async def _todos_em(self, ids, estado, descricao):
        await self._esperar(lambda d: all(self._robos(d).get(i, {}).get("estado") == estado
                                          for i in ids), descricao)

    # ---------- Atos ----------

    async def _ato1_equipes(self):
        self._mostrar(f"Ato 1 de {TOTAL_ATOS} — Equipes e acoplamento síncrono",
                      "Três robôs (na baia 1, na posição padrão e na baia 4) recebem uma caixa de "
                      "tamanho 3 na baia 2. O Organizador escolhe pelo MENOR CAMINHO A* (desviando de "
                      "paredes e obstáculos). Quem chega primeiro espera (laranja, AGUARDANDO_EQUIPE); "
                      "quando o último chega, todos recebem ACOPLAR no mesmo ciclo.", "")
        # distâncias bem diferentes até a baia 2: o primeiro espera pelos outros
        self.equipe1 = await self._criar([self._baia(1), None, self._baia(4)])
        self.enviar("caixa 3 2 C")
        await self._esperar(lambda d: any(r["estado"] == "AGUARDANDO_EQUIPE"
                                          for r in self._robos(d).values()),
                            "o primeiro robô chegar à baia 2")
        await self._todos_em(self.equipe1, "OPERANDO_CARGA",
                             "os outros chegarem e todos erguerem a caixa (ACOPLAR)")
        await self._pausa()
        self._mostrar(f"Ato 1 de {TOTAL_ATOS} — Transporte",
                      "O LÍDER (o mais perto da baia) calcula o caminho A* até a zona C e é o único que "
                      "usa os semáforos das passagens; cada seguidor calcula, a cada ciclo, o A* até a "
                      "posição do líder. A equipe ocupa 1 vaga, e o líder só libera a passagem quando "
                      "todos já saíram dela.", "")
        await self._esperar(lambda d: all(self._robos(d)[i]["estado"] == "DISPONIVEL"
                                          for i in self.equipe1), "a caixa ser entregue na zona C")

    async def _ato2_passagem(self):
        pas, xe, xs, y = self._passagem(2)
        nome = pas["nome"]
        self._mostrar(f"Ato 2 de {TOTAL_ATOS} — Passagem enchendo ({nome}, capacidade 2)",
                      f"Três robôs em modo manual vão atravessar a {nome}. Os dois primeiros estão com "
                      f"1% de bateria: ela acaba dentro do túnel e eles param lá (DEFEITO), continuando "
                      f"a ocupar a vaga. Com 2/2 o semáforo fica vermelho e as cancelas fecham; o "
                      f"terceiro espera na porta. A cada 3 s de espera ele recalcula o A* (a passagem "
                      f"cheia custa mais), mas aqui não há desvio que compense.", "")
        # Perto das bocas não há obstáculo (raio 2), então estas células são livres.
        # a e b ficam mais perto da boca que c: pegam as 2 vagas antes dele.
        a, b, c = self.passagem = await self._criar([(xe - 2, y), (xe - 2, y - 1), (xe - 3, y + 2)])
        for i in (a, b, c):
            self.enviar(f"manual {i}")
        await self._todos_em([a, b, c], "MANUAL", "os três ficarem em modo manual")
        desde = self._seq()
        # 1% de bateria = para depois de 5 células: já dentro do túnel
        self.enviar(f"bateria {a} 1")
        self.enviar(f"bateria {b} 1")
        self.enviar(f"goto {a} {xs + 2} {y}")
        self.enviar(f"goto {b} {xs + 2} {y - 1}")
        await asyncio.sleep(0.3)
        self.enviar(f"goto {c} {xs + 2} {y + 1}")
        await self._evento(a, f"entrou na passagem {nome}", desde)
        await self._evento(b, f"entrou na passagem {nome}", desde)
        await self._evento(c, f"esperando passagem {nome}", desde)
        await self._pausa()
        self._mostrar(f"Ato 2 de {TOTAL_ATOS} — A passagem libera",
                      f"Trocamos a bateria dos dois: eles voltam a andar e saem da {nome} (sem_post); "
                      f"o robô que esperava consegue a vaga (sem_timedwait) e atravessa.", "")
        desde = self._seq()
        self.enviar(f"bateria {a} 100")
        self.enviar(f"bateria {b} 100")
        await self._evento(c, "saiu da passagem", desde)

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
                      "Novo robô criado (posição padrão): agora há 4 livres e a caixa é atribuída à "
                      "equipe de 4.", "")
        novo = await self._criar([None])
        self.equipe4 = self.equipe1 + novo
        await self._todos_em(self.equipe4, "OPERANDO_CARGA", "a equipe de 4 erguer a caixa")

    async def _ato4_falha_seguidor(self):
        lider = self._robos(self.estado.copia())[self.equipe4[0]]["lider"]
        seguidor = max(i for i in self.equipe4 if i != lider)
        self._mostrar(f"Ato 4 de {TOTAL_ATOS} — Falha de um seguidor",
                      f"No meio do transporte o robô {seguidor} (seguidor; o líder é o {lider}) entra "
                      f"em DEFEITO: o Organizador manda PAUSAR para a equipe inteira, que segura a "
                      f"posição.", "")
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
                      f"carga e abaixo de 20%, a thread Status manda recarregar: ele vai à estação de "
                      f"MENOR CAMINHO A* e volta a DISPONIVEL com 100%.", "")
        self.enviar(f"auto {robo}")
        await self._todos_em([robo], "DISPONIVEL", f"o robô {robo} ficar disponível")
        self.enviar(f"bateria {robo} 15")
        await self._todos_em([robo], "RECARREGANDO", f"o robô {robo} ir recarregar")
        await self._esperar(lambda d: self._robos(d)[robo]["bateria"] > 40 and
                            self._robos(d)[robo]["estado"] == "RECARREGANDO",
                            f"o robô {robo} chegar à estação e carregar")
        await self._pausa()
        await self._todos_em([robo], "DISPONIVEL", f"o robô {robo} terminar a recarga (100%)")
