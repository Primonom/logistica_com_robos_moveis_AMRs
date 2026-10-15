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
        e.guardar_status(status(robos=[{"id": 0, "x": 1, "y": 1, "sp_x": 1, "sp_y": 1,
                                       "bateria": 80, "distancia": 0}]))
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

    def test_entregas_por_minuto_nunca_negativo_se_o_sistema_reinicia(self):
        e = Estado()
        e.guardar_status(status(entregues=9), agora=0.0)
        e.guardar_status(status(entregues=0), agora=5.0)    # sistema central reiniciado
        e.guardar_status(status(entregues=1), agora=35.0)
        self.assertAlmostEqual(e.copia(agora=35.0)["entregas_por_minuto"], 2.0)

    def test_eventos_limitados(self):
        e = Estado()
        for i in range(600):
            e.guardar_evento({"robo": 0, "texto": str(i)})
        eventos = e.copia()["eventos"]
        self.assertEqual(len(eventos), 500)
        self.assertEqual(eventos[-1]["texto"], "599")

    def test_direcao_andando_e_parado(self):
        e = Estado()
        robo = {"id": 3, "x": 10, "y": 15, "sp_x": 10, "sp_y": 16,    # descendo
                "bateria": 80, "distancia": 0}
        e.guardar_status(status(robos=[robo]))
        self.assertAlmostEqual(e.copia()["direcoes"][3], 90.0)
        robo = dict(robo, y=16)                                       # parou
        e.guardar_status(status(robos=[robo]))
        self.assertAlmostEqual(e.copia()["direcoes"][3], 90.0)       # mantém a última

    def test_historico_uma_amostra_por_segundo(self):
        e = Estado()
        robo = {"id": 2, "x": 1, "y": 1, "sp_x": 1, "sp_y": 1, "bateria": 90, "distancia": 0}
        for k in range(10):                    # 10 status em 2 s (a cada 0,2 s)
            e.guardar_status(status(robos=[dict(robo, bateria=90 - k)]), agora=k * 0.2)
        h = e.copia(agora=2.0)["historico"][2]
        self.assertEqual([round(t, 1) for t, _, _ in h], [0.0, 1.0])
        self.assertEqual(h[1][1], 85)
        self.assertEqual([round(t, 1) for t, _ in e.copia()["historico_entregas"]], [0.0, 1.0])

    def test_sistema_iniciado_limpa_o_que_era_da_execucao_anterior(self):
        e = Estado()
        robo = {"id": 0, "x": 1, "y": 1, "sp_x": 2, "sp_y": 1, "bateria": 50, "distancia": 9}
        e.guardar_status(status(robos=[robo]), agora=0.0)
        e.guardar_evento({"robo": 0, "texto": "DEFEITO: robo parado"})
        e.guardar_evento({"robo": -1, "texto": "sistema iniciado"})
        d = e.copia(agora=0.0)
        self.assertEqual([ev["texto"] for ev in d["eventos"]], ["sistema iniciado"])
        self.assertEqual(d["historico"], {})
        self.assertEqual(d["direcoes"], {})

    def test_eventos_e_respostas_numerados(self):
        e = Estado()
        e.guardar_evento({"robo": 1, "texto": "a"})
        e.guardar_evento({"robo": 1, "texto": "b"})
        e.guardar_resposta("erro: robo 9 nao existe")
        d = e.copia()
        self.assertEqual([ev["seq"] for ev in d["eventos"]], [1, 2])
        self.assertEqual((d["seq_resposta"], d["ultima_resposta"]), (1, "erro: robo 9 nao existe"))

    def test_duas_threads_ao_mesmo_tempo(self):
        """Uma thread escreve (como o MQTT) enquanto outra copia (como a tela)."""
        e = Estado()
        erros = []

        def escritor():
            for i in range(2000):
                e.guardar_status(status(entregues=i))
                e.guardar_evento({"robo": 0, "texto": str(i)})

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
