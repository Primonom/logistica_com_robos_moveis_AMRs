"""
Testes do mapa_svg.py sem interface (só dicionários).
Rodar da raiz do repositório:
    interface/.venv/bin/python -m unittest discover -s interface/testes -t interface
"""
import itertools
import json
import math
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import cores
import mapa_svg

# O atr/mapa publicado pelo C com ATR_SEMENTE=42 (copiado de uma execução real).
with open(Path(__file__).with_name("mapa_semente42.json"), encoding="utf-8") as arquivo:
    MAPA = json.load(arquivo)
CAPACIDADES = [p["capacidade"] for p in MAPA["passagens"]]    # (4, 2, 1, 3) na semente 42


def robo(rid, x, y, estado="DISPONIVEL", lider=-1, carga=-1, bateria=80, rota=()):
    return {"id": rid, "estado": estado, "modo": "auto", "x": x, "y": y, "sp_x": x, "sp_y": y,
            "destino_x": -1, "destino_y": -1, "bateria": bateria, "distancia": 0,
            "carga": carga, "lider": lider, "rota": [list(p) for p in rota]}


def status(robos, caixas=(), livres=None):
    livres = CAPACIDADES if livres is None else livres
    return {"t": "2026-10-09 10:00:00.000", "robos": list(robos), "caixas": list(caixas),
            "passagens": [{"nome": f"P{i + 1}", "capacidade": c, "livres": l}
                          for i, (c, l) in enumerate(zip(CAPACIDADES, livres))],
            "contadores": {"robos_ativos": len(robos), "entregues": 0, "pendentes": 0,
                           "em_andamento": 0}}


EQUIPE = [robo(0, 20, 15, "OPERANDO_CARGA", lider=0, carga=7, rota=[(38, 15), (56, 15)]),
          robo(1, 19, 15, "OPERANDO_CARGA", lider=0, carga=7)]
CAIXA_EQUIPE = {"id": 7, "tamanho": 2, "baia": 2, "zona": "B", "status": "EM_ANDAMENTO",
                "robos": [0, 1], "lider": 0}


class TestMapaSvg(unittest.TestCase):
    def test_fundo_e_vivo_validos_e_do_mesmo_tamanho(self):
        st = status(EQUIPE + [robo(2, 39, 15, "DEFEITO"), robo(3, 37, 28, "RECARREGANDO")],
                    [CAIXA_EQUIPE], livres=(4, 1, 1, 0))
        fundo = ET.fromstring(mapa_svg.svg_fundo(MAPA, fonte_base64="AAAA"))
        vivo = ET.fromstring(mapa_svg.svg_vivo(MAPA, st, selecionado=0, agora=12.3))
        self.assertEqual(fundo.get("width"), vivo.get("width"))
        self.assertEqual(fundo.get("height"), vivo.get("height"))
        ET.fromstring(mapa_svg.svg_vivo(MAPA, None))

    def test_semaforo_e_placa_com_a_ocupacao(self):
        svg = mapa_svg.svg_vivo(MAPA, status([], livres=(1, 2, 0, 3)))
        self.assertIn("P1  3/4", svg)
        self.assertIn("P2  0/2", svg)
        self.assertIn("P3  1/1", svg)
        self.assertIn("P4  0/3", svg)
        self.assertEqual(mapa_svg.cor_ocupacao(0, 3), "verde")
        self.assertEqual(mapa_svg.cor_ocupacao(2, 3), "amarelo")
        self.assertEqual(mapa_svg.cor_ocupacao(3, 3), "vermelho")

    def test_passagens_com_rotulo_da_capacidade(self):
        fundo = mapa_svg.svg_fundo(MAPA)
        for pas in MAPA["passagens"]:
            self.assertIn(f"{pas['nome']} (cap {pas['capacidade']})", fundo)
        self.assertIn("P3 (cap 1)", fundo)        # semente 42: P3 é a de capacidade 1

    def test_paredes_e_obstaculos_vem_do_json(self):
        fundo = ET.fromstring(mapa_svg.svg_fundo(MAPA))
        grupos = [g.get("class") for g in fundo.iter("{http://www.w3.org/2000/svg}g")]
        self.assertEqual(grupos.count("obstaculo"), len(MAPA["obstaculos"]))
        self.assertEqual(grupos.count("parede"), len(MAPA["paredes"]))
        # outro mapa (outra semente): o desenho muda junto, nada é fixo no Python
        outro = dict(MAPA, obstaculos=MAPA["obstaculos"][:3], paredes=MAPA["paredes"][:2])
        grupos = [g.get("class") for g in ET.fromstring(mapa_svg.svg_fundo(outro))
                  .iter("{http://www.w3.org/2000/svg}g")]
        self.assertEqual(grupos.count("obstaculo"), 3)
        self.assertEqual(grupos.count("parede"), 2)

    def test_tunel_e_regiao_da_passagem(self):
        pas = MAPA["passagens"][0]
        x1, x2, y = mapa_svg.tunel(pas)
        self.assertEqual(x2 - x1 + 1, 4)                         # atravessa a parede (4 colunas)
        self.assertEqual(len(pas["regiao"]), len(pas["celulas"]) + 2)   # + uma boca de cada lado
        self.assertTrue(all(c[1] == y for c in pas["regiao"]))

    def test_sem_faixas_de_corredor_no_fundo(self):
        # sem a faixa horizontal do antigo corredor y = 15 e sem as faixas
        # verticais de acesso: o único retângulo amarelo com borda é a régua
        fundo = mapa_svg.svg_fundo(MAPA)
        self.assertEqual(fundo.count(f'fill="{cores.FAIXA}" stroke="{cores.FAIXA_BORDA}"'), 2)
        self.assertNotIn(f'fill="{cores.FAIXA}" opacity', fundo)

    def test_nenhum_obstaculo_nas_areas_de_baias_e_zonas(self):
        # o C não sorteia obstáculo dentro dos retângulos que a tela desenha
        for ponto, lado in [(b, "esq") for b in MAPA["baias"]] + [(z, "dir") for z in MAPA["zonas"]]:
            x1, y1, x2, y2 = mapa_svg.regiao(ponto, MAPA, lado)
            for x, y in MAPA["obstaculos"]:
                self.assertFalse(x1 <= x <= x2 and y1 <= y <= y2, (ponto, x, y))

    def test_cancela_fecha_quando_a_passagem_enche(self):
        aberta = mapa_svg._todas_as_cancelas(MAPA, status([], livres=(4, 2, 1, 3)), fechadas=True)
        cheia = mapa_svg._todas_as_cancelas(MAPA, status([], livres=(4, 2, 0, 3)), fechadas=True)
        self.assertEqual(aberta, [])
        self.assertTrue(cheia)                                   # só a P3 (cap 1) está cheia

    def test_regiao_da_baia_nao_invade_o_corredor(self):
        for b in MAPA["baias"]:
            _, y0, _, y1 = mapa_svg.regiao(b, MAPA, "esq")
            self.assertFalse(y0 <= MAPA["corredor_y"] <= y1)
            self.assertTrue(y0 <= b["y"] <= y1)

    def test_celula_do_ponto_inverte_centro(self):
        x, y = mapa_svg.centro(12, 7)
        self.assertEqual(mapa_svg.celula_do_ponto(x, y), (12, 7))

    def test_unidades_na_mesma_celula_nao_se_sobrepoem(self):
        for n in (2, 3, 4):
            st = status([robo(i, 20, 15) for i in range(n)])
            pos = [p for p, _, _ in mapa_svg._enfileirar(mapa_svg._unidades(st), {})]
            for a, b in itertools.combinations(pos, 2):
                self.assertGreater(math.dist(a, b), mapa_svg.ROBO * 0.7)
        # duas equipes carregando + um robô na mesma célula (fila na porta)
        st = status(EQUIPE + [robo(5, 20, 15, "OPERANDO_CARGA", lider=5, carga=8),
                              robo(6, 20, 15)],
                    [CAIXA_EQUIPE, dict(CAIXA_EQUIPE, id=8, robos=[5], lider=5)])
        pos = [p for p, _, _ in mapa_svg._enfileirar(mapa_svg._unidades(st), {0: 0.0})]
        self.assertEqual(len(pos), 3)
        for a, b in itertools.combinations(pos, 2):
            self.assertGreater(math.dist(a, b), 40)
        # andando para a direita (0°), a fila cresce para a esquerda, no corredor
        self.assertTrue(all(abs(y - pos[0][1]) < 1 for _, y in pos))
        self.assertEqual(sorted(pos, reverse=True), pos)

    def test_robo_em_defeito_continua_na_equipe_carregando(self):
        st = status([dict(EQUIPE[0], estado="DEFEITO"), EQUIPE[1]], [CAIXA_EQUIPE])
        unidades = mapa_svg._unidades(st)
        self.assertEqual(len(unidades), 1)                 # uma unidade só
        self.assertEqual(unidades[0][2], "equipe")
        self.assertEqual(len(unidades[0][3][0]), 2)        # com os dois robôs

    def test_placas_de_robos_vizinhos_nao_se_cobrem(self):
        svg = mapa_svg._desenhar_placas([(100, 50, 9), (110, 50, 1), (105, 52, 4)])
        ys = sorted(float(r.get("y")) for r in ET.fromstring(f"<g>{''.join(svg)}</g>").iter("rect"))
        for a, b in zip(ys, ys[1:]):
            self.assertGreaterEqual(b - a, 20)

    def test_rota_so_do_selecionado_tem_bandeira(self):
        st = status(EQUIPE, [CAIXA_EQUIPE])
        self.assertIn('href="#bandeira"', mapa_svg.svg_vivo(MAPA, st, selecionado=0))
        self.assertNotIn('href="#bandeira"', mapa_svg.svg_vivo(MAPA, st, selecionado=None))

    def test_animacao_continua_da_fase_certa(self):
        # mesma fase em instantes separados por um período inteiro
        self.assertEqual(mapa_svg.animado("atr-pisca", "x", 3.25),
                         mapa_svg.animado("atr-pisca", "x", 4.25))
        self.assertIn("animation-delay: -0.25s", mapa_svg.animado("atr-pisca", "x", 3.25))

    def test_clique_acha_o_robo_desenhado(self):
        st = status(EQUIPE + [robo(5, 30, 10)], [CAIXA_EQUIPE])
        pos = mapa_svg.posicoes_robos(st, {})
        for rid, (x, y) in pos.items():          # clicar em cima de cada robô acha ele
            self.assertEqual(mapa_svg.robo_no_ponto(x + 3, y - 3, st, {}), rid)
        x, y = mapa_svg.centro(50, 25)           # chão vazio: ninguém
        self.assertIsNone(mapa_svg.robo_no_ponto(x, y, st, {}))

    def test_miniatura_valida(self):
        for estado in ("DISPONIVEL", "DEFEITO", "RECARREGANDO", "MANUAL"):
            ET.fromstring(mapa_svg.svg_miniatura_robo(estado))

    def test_caixa_fica_na_baia_ate_ser_erguida(self):
        caixa = dict(CAIXA_EQUIPE)                     # EM_ANDAMENTO, baia 2
        indo = status([dict(r, estado="DESLOCANDO") for r in EQUIPE], [caixa])
        erguida = status(EQUIPE, [caixa])               # equipe em OPERANDO_CARGA
        self.assertTrue(mapa_svg._caixas_nas_baias(MAPA, indo))
        self.assertFalse(mapa_svg._caixas_nas_baias(MAPA, erguida))

    def test_texto_com_caracteres_especiais_fica_valido(self):
        mapa = dict(MAPA, zonas=[{"id": "<&>", "x": 56, "y": 4}])
        ET.fromstring(mapa_svg.svg_fundo(mapa))

    def test_seta_de_sentido_so_na_passagem_ocupada(self):
        st = status([])
        st["passagens"][0].update(dentro=2, sentido=1)     # P1: 2 robôs para a esquerda
        st["passagens"][1].update(dentro=0, sentido=0)     # P2: vazia, sem seta
        svg = mapa_svg.svg_vivo(MAPA, st)
        ET.fromstring(svg)
        self.assertEqual(svg.count('class="seta-sentido"'), 1)
        x1, x2, _ = mapa_svg.tunel(MAPA["passagens"][0])
        # para a esquerda: a ponta fica no lado esquerdo do túnel
        self.assertIn(f'L{mapa_svg.px(x1) + 8} ', svg)
        # status antigo (sem "dentro"): nenhuma seta
        self.assertNotIn("seta-sentido", mapa_svg.svg_vivo(MAPA, status([])))

    def test_robo_bloqueado_mostra_aguardando(self):
        r = robo(0, 10, 15, "DESLOCANDO")
        self.assertNotIn('class="aguardando"', mapa_svg.svg_vivo(MAPA, status([r])))
        r["bloqueado"] = True
        svg = mapa_svg.svg_vivo(MAPA, status([r]))
        ET.fromstring(svg)
        self.assertIn('class="aguardando"', svg)

    def test_legenda_valida(self):
        ET.fromstring(mapa_svg.svg_legenda())


if __name__ == "__main__":
    unittest.main()
