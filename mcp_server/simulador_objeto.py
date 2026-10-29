"""
Simulador del objeto inteligente para probar el servidor MCP sin el ESP32.

Reproduce el contrato MQTT del firmware:
  - publica una distancia que varía suavemente (1 lectura cada 5 s) en /distancia;
  - aplica los comandos de /cmd sobre tres LEDs virtuales (incluye "todos")
    y publica su estado (retenido) en /estado;
  - publica "online" (retenido) en /conexion y deja "offline" como Last Will.
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
PREFIJO = os.getenv("MQTT_PREFIJO", "sis234/grupo4").rstrip("/")
TOPICO_DISTANCIA = f"{PREFIJO}/distancia"
TOPICO_CMD = f"{PREFIJO}/cmd"
TOPICO_ESTADO = f"{PREFIJO}/estado"
TOPICO_CONEXION = f"{PREFIJO}/conexion"
PERIODO_S = 5.0

leds = {"rojo": False, "amarillo": False, "verde": False}


def publicar_estado(cliente):
    cliente.publish(TOPICO_ESTADO, json.dumps(leds), qos=0, retain=True)  # Igual que el ESP32


def al_conectar(cliente, _userdata, _flags, _reason_code, _properties):
    print(f"Conectado a {BROKER}:{PUERTO}; escuchando {TOPICO_CMD}")
    cliente.subscribe(TOPICO_CMD, qos=1)
    cliente.publish(TOPICO_CONEXION, "online", qos=1, retain=True)
    publicar_estado(cliente)


def al_recibir(cliente, _userdata, mensaje):
    texto = mensaje.payload.decode("utf-8", errors="replace")
    print(f"[CMD] {texto}")
    try:
        comando = json.loads(texto)
        led, estado = comando["led"], comando["estado"]
    except (json.JSONDecodeError, KeyError, TypeError):
        print("      ignorado: formato inválido")
        return
    if estado not in ("on", "off") or (led not in leds and led != "todos"):
        print("      ignorado: valores inválidos")
        return
    for color in leds if led == "todos" else [led]:
        leds[color] = estado == "on"
    publicar_estado(cliente)


def main():
    cliente = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"sis234-sim-{uuid.uuid4().hex[:8]}",
    )
    if os.getenv("MQTT_USER"):
        cliente.username_pw_set(os.getenv("MQTT_USER"), os.getenv("MQTT_PASSWORD"))
    if USAR_TLS:
        cliente.tls_set()
    cliente.will_set(TOPICO_CONEXION, "offline", qos=1, retain=True)
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
        # Desconexión ordenada: el broker no envía el Last Will, se avisa a mano.
        cliente.publish(TOPICO_CONEXION, "offline", qos=1, retain=True).wait_for_publish(2)
        cliente.loop_stop()
        cliente.disconnect()


if __name__ == "__main__":
    main()
