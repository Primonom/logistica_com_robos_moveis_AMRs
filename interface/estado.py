"""
estado.py — "Loja" de estado da Tela de Operação.

Guarda o que chega pelo MQTT: o último status, o mapa, os últimos eventos,
a última resposta de comando e se o broker está conectado.

Concorrência (é ATR também):
  - Quem ESCREVE aqui é a thread de rede do paho-mqtt (callbacks em
    mqtt_cliente.py).
  - Quem LÊ é o laço da interface (NiceGUI), num ui.timer a cada 200 ms.
  - As duas rodam ao mesmo tempo, então TUDO passa por self._trava
    (threading.Lock). Sem a trava, a interface poderia ler um status pela
    metade, ou a lista de eventos no meio de um append.
  - A interface nunca usa os dados daqui diretamente: pede copia(), que copia
    tudo com a trava e devolve um dicionário novo. A tela é desenhada a
    partir dessa cópia, FORA da trava (mesmo padrão do comando "status" no
    C: copia com o mutex, usa depois). Assim a thread do MQTT nunca espera a
    tela terminar de desenhar.

Este arquivo não importa nada da interface: dá para testar só com
dicionários (ver testes/test_estado.py).
"""
import copy
import threading
import time
from collections import deque

SISTEMA_OFFLINE_S = 2.0      # sem atr/status há mais que isso -> "offline"
MAX_EVENTOS = 500            # guarda só os últimos eventos
JANELA_ENTREGAS_S = 60.0     # janela para "entregas por minuto"


class Estado:
    def __init__(self):
        self._trava = threading.Lock()
        self._status = None              # último atr/status (dict)
        self._mapa = None                # atr/mapa (dict)
        self._status_em = None           # time.monotonic() do último status
        self._broker_conectado = False
        self._eventos = deque(maxlen=MAX_EVENTOS)
        self._ultima_resposta = None     # texto de atr/comando/resposta
        self._amostras_entregas = deque()  # (instante, entregues) da última janela

    # ---------- Escrita (chamadas pela thread do MQTT) ----------

    def guardar_status(self, status, agora=None):
        agora = time.monotonic() if agora is None else agora
        with self._trava:
            self._status = status
            self._status_em = agora
            self._amostras_entregas.append((agora, status["contadores"]["entregues"]))
            # descarta amostras mais velhas que a janela
            while self._amostras_entregas[0][0] < agora - JANELA_ENTREGAS_S:
                self._amostras_entregas.popleft()

    def guardar_mapa(self, mapa):
        with self._trava:
            self._mapa = mapa

    def guardar_evento(self, evento):
        with self._trava:
            self._eventos.append(evento)

    def guardar_resposta(self, texto):
        with self._trava:
            self._ultima_resposta = texto

    def definir_conexao(self, conectado):
        with self._trava:
            self._broker_conectado = conectado

    # ---------- Leitura (chamada pela interface) ----------

    def copia(self, agora=None):
        """Retorna um dicionário NOVO com tudo (cópia feita com a trava)."""
        agora = time.monotonic() if agora is None else agora
        with self._trava:
            dados = {
                "status": copy.deepcopy(self._status),
                "mapa": copy.deepcopy(self._mapa),
                "eventos": list(self._eventos),     # os eventos não são alterados depois
                "ultima_resposta": self._ultima_resposta,
                "broker_conectado": self._broker_conectado,
                "sistema_online": (self._status_em is not None
                                   and agora - self._status_em <= SISTEMA_OFFLINE_S),
                "entregas_por_minuto": self._entregas_por_minuto(),
            }
        return dados

    def _entregas_por_minuto(self):
        """Entregas na última janela, convertidas para "por minuto".
        Chamada com a trava já pega."""
        if len(self._amostras_entregas) < 2:
            return 0.0
        (t0, e0), (t1, e1) = self._amostras_entregas[0], self._amostras_entregas[-1]
        if t1 - t0 < 1.0:
            return 0.0
        return (e1 - e0) * 60.0 / (t1 - t0)
