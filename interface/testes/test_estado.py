"""
Testes do estado.py sem interface (só dicionários).
Rodar da raiz do repositório:
    interface/.venv/bin/python -m unittest discover -s interface/testes -t interface
"""
import threading
import unittest

from estado import Estado


def status(entregues=0, robos=None):
    """Um atr/status mínimo."""
    return {"t": "2026-10-09 10:00:00.000", "robos": robos or [], "caixas": [],
            "passagens": [],
            "contadores": {"robos_ativos": len(robos or []), "entregues": entregues,
                           "pendentes": 0, "em_andamento": 0}}


class TestEstado(unittest.TestCase):
    def test_comeca_vazio_e_offline(self):
        d = Estado().copia()
        self.assertIsNone(d["status"])
        self.assertFalse(d["sistema_online"])
        self.assertFalse(d["broker_conectado"])

    def test_online_ate_2s_sem_status(self):
        e = Estado()
        e.guardar_status(status(), agora=100.0)
        self.assertTrue(e.copia(agora=101.9)["sistema_online"])
        self.assertFalse(e.copia(agora=102.1)["sistema_online"])

    def test_copia_nao_e_afetada_por_status_novo(self):
        e = Estado()
        e.guardar_status(status(robos=[{"id": 0}]))
        c = e.copia()
        c["status"]["robos"].append({"id": 99})          # mexer na cópia...
        self.assertEqual(len(e.copia()["status"]["robos"]), 1)  # ...não muda a loja

    def test_entregas_por_minuto(self):
        e = Estado()
        e.guardar_status(status(entregues=0), agora=0.0)
        e.guardar_status(status(entregues=3), agora=30.0)
        self.assertAlmostEqual(e.copia(agora=30.0)["entregas_por_minuto"], 6.0)
        # amostras com mais de 60 s saem da janela
        e.guardar_status(status(entregues=3), agora=95.0)
        self.assertAlmostEqual(e.copia(agora=95.0)["entregas_por_minuto"], 0.0)

    def test_eventos_limitados(self):
        e = Estado()
        for i in range(600):
            e.guardar_evento({"texto": str(i)})
        eventos = e.copia()["eventos"]
        self.assertEqual(len(eventos), 500)
        self.assertEqual(eventos[-1]["texto"], "599")

    def test_duas_threads_ao_mesmo_tempo(self):
        """Uma thread escreve (como o MQTT) enquanto outra copia (como a tela)."""
        e = Estado()
        erros = []

        def escritor():
            for i in range(2000):
                e.guardar_status(status(entregues=i))
                e.guardar_evento({"texto": str(i)})

        def leitor():
            try:
                for _ in range(2000):
                    d = e.copia()
                    if d["status"] is not None:
                        assert "contadores" in d["status"]
            except Exception as erro:
                erros.append(erro)

        threads = [threading.Thread(target=escritor), threading.Thread(target=leitor)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(erros, [])


if __name__ == "__main__":
    unittest.main()
