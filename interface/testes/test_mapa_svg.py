"""
Testes do mapa_svg.py sem interface (só dicionários).
Rodar da raiz do repositório:
    interface/.venv/bin/python -m unittest discover -s interface/testes -t interface
"""
import itertools
import math
import unittest
import xml.etree.ElementTree as ET

import mapa_svg

# Igual ao atr/mapa publicado pelo C (copiado de uma execução real).
MAPA = {"largura": 60, "altura": 30, "corredor_y": 15,
        "baias": [{"id": 1, "x": 3, "y": 4}, {"id": 2, "x": 3, "y": 11},
                  {"id": 3, "x": 3, "y": 18}, {"id": 4, "x": 3, "y": 25}],
        "zonas": [{"id": "A", "x": 56, "y": 4}, {"id": "B", "x": 56, "y": 11},
                  {"id": "C", "x": 56, "y": 18}, {"id": "D", "x": 56, "y": 25}],
        "estacoes": [{"x": 45, "y": 1}, {"x": 45, "y": 28}],
        "passagens": [
            {"nome": "P1", "capacidade": 3, "retangulos": [{"x1": 14, "y1": 0, "x2": 16, "y2": 29}]},
            {"nome": "P2", "capacidade": 2, "retangulos": [{"x1": 39, "y1": 0, "x2": 41, "y2": 29}]},
            {"nome": "P3", "capacidade": 1, "retangulos": [{"x1": 45, "y1": 2, "x2": 45, "y2": 13},
                                                         {"x1": 45, "y1": 17, "x2": 45, "y2": 27}]}]}


def robo(rid, x, y, estado="DISPONIVEL", lider=-1, carga=-1, bateria=80, rota=()):
    return {"id": rid, "estado": estado, "modo": "auto", "x": x, "y": y, "sp_x": x, "sp_y": y,
            "destino_x": -1, "destino_y": -1, "bateria": bateria, "distancia": 0,
            "carga": carga, "lider": lider, "rota": [list(p) for p in rota]}


def status(robos, caixas=(), livres=(3, 2, 1)):
    return {"t": "2026-10-09 10:00:00.000", "robos": list(robos), "caixas": list(caixas),
            "passagens": [{"nome": f"P{i + 1}", "capacidade": c, "livres": l}
                          for i, (c, l) in enumerate(zip((3, 2, 1), livres))],
            "contadores": {"robos_ativos": len(robos), "entregues": 0, "pendentes": 0,
                           "em_andamento": 0}}


EQUIPE = [robo(0, 20, 15, "OPERANDO_CARGA", lider=0, carga=7, rota=[(38, 15), (56, 15)]),
          robo(1, 19, 15, "OPERANDO_CARGA", lider=0, carga=7)]
CAIXA_EQUIPE = {"id": 7, "tamanho": 2, "baia": 2, "zona": "B", "status": "EM_ANDAMENTO",
                "robos": [0, 1], "lider": 0}


class TestMapaSvg(unittest.TestCase):
    def test_fundo_e_vivo_validos_e_do_mesmo_tamanho(self):
        st = status(EQUIPE + [robo(2, 39, 15, "DEFEITO"), robo(3, 45, 28, "RECARREGANDO")],
                    [CAIXA_EQUIPE], livres=(3, 1, 1))
        fundo = ET.fromstring(mapa_svg.svg_fundo(MAPA, fonte_base64="AAAA"))
        vivo = ET.fromstring(mapa_svg.svg_vivo(MAPA, st, selecionado=0, agora=12.3))
        self.assertEqual(fundo.get("width"), vivo.get("width"))
        self.assertEqual(fundo.get("height"), vivo.get("height"))
        ET.fromstring(mapa_svg.svg_vivo(MAPA, None))

    def test_semaforo_e_placa_com_a_ocupacao(self):
        svg = mapa_svg.svg_vivo(MAPA, status([], livres=(1, 2, 0)))
        self.assertIn("P1  2/3", svg)
        self.assertIn("P2  0/2", svg)
        self.assertIn("P3  1/1", svg)
        self.assertEqual(mapa_svg.cor_ocupacao(0, 3), "verde")
        self.assertEqual(mapa_svg.cor_ocupacao(2, 3), "amarelo")
        self.assertEqual(mapa_svg.cor_ocupacao(3, 3), "vermelho")

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

    def test_texto_com_caracteres_especiais_fica_valido(self):
        mapa = dict(MAPA, zonas=[{"id": "<&>", "x": 56, "y": 4}])
        ET.fromstring(mapa_svg.svg_fundo(mapa))

    def test_legenda_valida(self):
        ET.fromstring(mapa_svg.svg_legenda())


if __name__ == "__main__":
    unittest.main()
