"""
Testes das funções de aparência de cores.py (sem interface).
Rodar da raiz do repositório:
    interface/.venv/bin/python -m pytest -q interface
"""
import unittest

import cores


def evento(texto, tipo="evento", robo=-1):
    return {"texto": texto, "tipo": tipo, "robo": robo}


class TestTextoEvento(unittest.TestCase):
    def test_carga_de_um_robo(self):
        self.assertEqual(cores.texto_evento(evento("4;2;D;0", "carga")),
                         "Caixa 4: baia 2 → zona D (robôs 0)")

    def test_carga_de_varios_robos(self):
        self.assertEqual(cores.texto_evento(evento("5;1;A;0 3 5", "carga")),
                         "Caixa 5: baia 1 → zona A (robôs 0, 3, 5)")

    def test_evento_comum_fica_igual(self):
        self.assertEqual(cores.texto_evento(evento("modo manual", robo=2)), "modo manual")


if __name__ == "__main__":
    unittest.main()
