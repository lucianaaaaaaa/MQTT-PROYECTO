"""
Simulador del objeto inteligente para probar el servidor MCP sin el ESP32.

Publica una distancia que varía suavemente (2 lecturas/s) en el tópico de
distancia e imprime los comandos que recibe en el tópico de comandos.
Usa las mismas variables de entorno que server.py.

    python simulador_objeto.py
"""

import json
import math
import os
import time
import uuid

import paho.mqtt.client as mqtt

BROKER = os.getenv("MQTT_BROKER", "broker.hivemq.com")
USAR_TLS = os.getenv("MQTT_TLS", "0") == "1"
PUERTO = int(os.getenv("MQTT_PORT", "8883" if USAR_TLS else "1883"))
TOPICO_DISTANCIA = os.getenv("TOPIC_DISTANCIA", "sis234/grupoX/distancia")
TOPICO_CMD = os.getenv("TOPIC_CMD", "sis234/grupoX/cmd")
PERIODO_S = 0.5


def al_conectar(cliente, _userdata, _flags, _reason_code, _properties):
    print(f"Conectado a {BROKER}:{PUERTO}; escuchando {TOPICO_CMD}")
    cliente.subscribe(TOPICO_CMD, qos=1)


def al_recibir(_cliente, _userdata, mensaje):
    print(f"[CMD] {mensaje.payload.decode('utf-8', errors='replace')}")


def main():
    cliente = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"sis234-sim-{uuid.uuid4().hex[:8]}",
    )
    if os.getenv("MQTT_USER"):
        cliente.username_pw_set(os.getenv("MQTT_USER"), os.getenv("MQTT_PASSWORD"))
    if USAR_TLS:
        cliente.tls_set()
    cliente.on_connect = al_conectar
    cliente.on_message = al_recibir
    cliente.connect(BROKER, PUERTO, keepalive=30)
    cliente.loop_start()

    inicio = time.time()
    try:
        while True:
            cm = 50 + 30 * math.sin((time.time() - inicio) / 5)
            cliente.publish(TOPICO_DISTANCIA, json.dumps({"cm": round(cm, 1)}))
            time.sleep(PERIODO_S)
    except KeyboardInterrupt:
        pass
    finally:
        cliente.loop_stop()
        cliente.disconnect()


if __name__ == "__main__":
    main()
