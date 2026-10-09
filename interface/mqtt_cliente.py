"""
mqtt_cliente.py — Conexão da Tela de Operação com o broker MQTT.

Assina:   atr/mapa, atr/status, atr/eventos, atr/comando/resposta
Publica:  atr/comando  (linhas iguais às do Console do C: "manual 2", ...)

Concorrência:
  - client.loop_start() cria a THREAD DE REDE do paho-mqtt. Os callbacks
    (_ao_conectar, _ao_desconectar, _ao_receber) rodam nela.
  - Os callbacks só GUARDAM os dados na loja (estado.py, protegida por
    threading.Lock). Nunca mexem em componentes da interface: quem desenha é
    o ui.timer do NiceGUI, no laço dele, a partir de uma cópia do estado.
  - Se o broker não estiver no ar, o paho continua tentando reconectar
    sozinho (connect_async + loop_start), e a tela mostra "broker
    desconectado".
"""
import json

import paho.mqtt.client as mqtt

TOPICOS = ["atr/mapa", "atr/status", "atr/eventos", "atr/comando/resposta"]


class ClienteMqtt:
    def __init__(self, estado, host="localhost", porta=1883):
        self.estado = estado
        self.host = host
        self.porta = porta
        self.cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                   client_id="atr_tela")
        self.cliente.on_connect = self._ao_conectar
        self.cliente.on_disconnect = self._ao_desconectar
        self.cliente.on_message = self._ao_receber
        self.cliente.reconnect_delay_set(min_delay=1, max_delay=5)

    def iniciar(self):
        """Começa a conectar (sem esperar) e inicia a thread de rede."""
        self.cliente.connect_async(self.host, self.porta, keepalive=10)
        self.cliente.loop_start()

    def parar(self):
        self.cliente.disconnect()
        self.cliente.loop_stop()

    def enviar_comando(self, linha):
        """Publica uma linha de comando (igual à do Console do C).
        A resposta chega depois em atr/comando/resposta."""
        self.cliente.publish("atr/comando", linha)

    # ---------- Callbacks (thread de rede do paho) ----------

    def _ao_conectar(self, cliente, dados, flags, codigo, propriedades):
        if codigo.is_failure:
            print(f"Tela: conexão recusada pelo broker ({codigo})")
            return
        # Assina a cada (re)conexão. atr/mapa é "retained": chega na hora.
        for topico in TOPICOS:
            cliente.subscribe(topico)
        self.estado.definir_conexao(True)
        print(f"Tela: conectada ao broker {self.host}:{self.porta}")

    def _ao_desconectar(self, cliente, dados, flags, codigo, propriedades):
        self.estado.definir_conexao(False)

    def _ao_receber(self, cliente, dados, msg):
        texto = msg.payload.decode("utf-8", errors="replace")
        if msg.topic == "atr/comando/resposta":
            self.estado.guardar_resposta(texto)
            return
        try:
            conteudo = json.loads(texto)
        except json.JSONDecodeError:
            print(f"Tela: JSON inválido em {msg.topic}: {texto[:80]}")
            return
        if msg.topic == "atr/status":
            self.estado.guardar_status(conteudo)
        elif msg.topic == "atr/mapa":
            self.estado.guardar_mapa(conteudo)
        elif msg.topic == "atr/eventos":
            self.estado.guardar_evento(conteudo)
