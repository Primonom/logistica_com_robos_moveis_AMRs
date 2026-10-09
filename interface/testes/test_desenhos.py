"""
Testes do desenhos.py sem interface (só texto).
Rodar da raiz do repositório:
    interface/.venv/bin/python -m unittest discover -s interface/testes -t interface
"""
import unittest
import xml.etree.ElementTree as ET

import desenhos

SVG = "{http://www.w3.org/2000/svg}"


def _svg(conteudo):
    return f'<svg xmlns="http://www.w3.org/2000/svg">{conteudo}</svg>'


class TestDesenhos(unittest.TestCase):
    def test_defs_valido_com_todos_os_simbolos(self):
        raiz = ET.fromstring(_svg(desenhos.defs()))
        ids = {s.get("id") for s in raiz.iter(SVG + "symbol")}
        self.assertEqual(ids, set(desenhos.NOMES_SIMBOLOS))

    def test_texto_escapado(self):
        t = desenhos.texto(0, 0, "<baia & zona>")
        self.assertIn("&lt;baia &amp; zona&gt;", t)
        ET.fromstring(_svg(t))

    def test_usar_aponta_para_o_simbolo(self):
        u = ET.fromstring(_svg(desenhos.usar("robo", 1, 2, 28, 28))).find(SVG + "use")
        self.assertEqual(u.get("href"), "#robo")


if __name__ == "__main__":
    unittest.main()
