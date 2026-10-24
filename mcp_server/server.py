"""
Servidor MCP del Objeto Inteligente (SIS-234 — Práctica MQTT).

Expone a una herramienta de IA (Claude Desktop) un conjunto de herramientas
MCP para leer la distancia medida por el sensor ultrasónico y controlar los
LEDs del objeto inteligente. Toda la comunicación con el objeto pasa por el
broker MQTT en la nube: este servidor es un cliente MQTT más.

    Claude Desktop <--MCP (stdio)--> server.py <--MQTT--> broker <--> ESP32

Configuración (variables de entorno, ver claude_desktop_config.json):
    MQTT_BROKER        Host del broker            (default: broker.hivemq.com)
    MQTT_PORT          Puerto                      (default: 1883, 8883 si TLS)
    MQTT_TLS           "1" para usar TLS           (default: 0)
    MQTT_USER          Usuario del broker          (opcional)
    MQTT_PASSWORD      Contraseña del broker       (opcional)
    TOPIC_DISTANCIA    Tópico de lecturas          (default: sis234/grupoX/distancia)
    TOPIC_CMD          Tópico de comandos          (default: sis234/grupoX/cmd)

Importante: con transporte stdio, stdout está reservado para el protocolo MCP.
Todos los mensajes de diagnóstico se escriben en stderr mediante `logging`.
"""

from __future__ import annotations

import json
import logging
import os
import sys
import threading
import time
import uuid
from collections import deque
from dataclasses import dataclass
from typing import Literal

import paho.mqtt.client as mqtt
from mcp.server.fastmcp import FastMCP

logging.basicConfig(
    stream=sys.stderr,
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("mcp_server")

# Colores de LED aceptados por el firmware (contrato con la clase Actuadores).
COLORES_LED = ("rojo", "amarillo", "verde")
Color = Literal["rojo", "amarillo", "verde"]
Estado = Literal["on", "off"]


@dataclass(frozen=True)
class ConfiguracionMQTT:
    """Parámetros de conexión al broker y tópicos del grupo."""

    broker: str
    puerto: int
    usar_tls: bool
    usuario: str | None
    contrasena: str | None
    topico_distancia: str
    topico_cmd: str

    @classmethod
    def desde_entorno(cls) -> "ConfiguracionMQTT":
        """Construye la configuración a partir de variables de entorno."""
        usar_tls = os.getenv("MQTT_TLS", "0") == "1"
        return cls(
            broker=os.getenv("MQTT_BROKER", "broker.hivemq.com"),
            puerto=int(os.getenv("MQTT_PORT", "8883" if usar_tls else "1883")),
            usar_tls=usar_tls,
            usuario=os.getenv("MQTT_USER") or None,
            contrasena=os.getenv("MQTT_PASSWORD") or None,
            topico_distancia=os.getenv("TOPIC_DISTANCIA", "sis234/grupoX/distancia"),
            topico_cmd=os.getenv("TOPIC_CMD", "sis234/grupoX/cmd"),
        )


@dataclass(frozen=True)
class Lectura:
    """Una medición de distancia recibida desde el objeto inteligente."""

    cm: float
    recibida_en: float  # time.time() al momento de recibirla

    def antiguedad_s(self) -> float:
        """Segundos transcurridos desde que se recibió la lectura."""
        return time.time() - self.recibida_en


class ClienteMQTT:
    """
    Cliente MQTT del servidor MCP.

    - Se suscribe al tópico de distancia y guarda la última lectura y un
      historial acotado.
    - Publica comandos JSON en el tópico de comandos.
    - Reconecta automáticamente (paho reintenta en su hilo de red) y vuelve a
      suscribirse en cada conexión.
    """

    TAMANO_HISTORIAL = 120  # ~1 min a 2 lecturas/s

    def __init__(self, config: ConfiguracionMQTT) -> None:
        self._config = config
        self._historial: deque[Lectura] = deque(maxlen=self.TAMANO_HISTORIAL)
        self._candado = threading.Lock()
        self._conectado = threading.Event()
        self._nueva_lectura = threading.Event()

        self._cliente = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"sis234-mcp-{uuid.uuid4().hex[:8]}",
        )
        if config.usuario:
            self._cliente.username_pw_set(config.usuario, config.contrasena)
        if config.usar_tls:
            self._cliente.tls_set()
        self._cliente.reconnect_delay_set(min_delay=1, max_delay=10)
        self._cliente.on_connect = self._al_conectar
        self._cliente.on_disconnect = self._al_desconectar
        self._cliente.on_message = self._al_recibir_mensaje

    # ------------------------------------------------------------------ ciclo de vida

    def iniciar(self) -> None:
        """Inicia la conexión asíncrona y el hilo de red de paho."""
        log.info("Conectando a %s:%d ...", self._config.broker, self._config.puerto)
        self._cliente.connect_async(self._config.broker, self._config.puerto, keepalive=30)
        self._cliente.loop_start()

    def detener(self) -> None:
        """Cierra la conexión con el broker."""
        self._cliente.disconnect()
        self._cliente.loop_stop()

    @property
    def conectado(self) -> bool:
        return self._conectado.is_set()

    # ------------------------------------------------------------------ callbacks paho

    def _al_conectar(self, cliente, _userdata, _flags, reason_code, _properties) -> None:
        if reason_code.is_failure:
            log.error("Conexión rechazada por el broker: %s", reason_code)
            return
        log.info("Conectado al broker; suscribiendo a %s", self._config.topico_distancia)
        cliente.subscribe(self._config.topico_distancia, qos=0)
        self._conectado.set()

    def _al_desconectar(self, _cliente, _userdata, _flags, reason_code, _properties) -> None:
        self._conectado.clear()
        log.warning("Desconectado del broker (%s); paho reintentará", reason_code)

    def _al_recibir_mensaje(self, _cliente, _userdata, mensaje: mqtt.MQTTMessage) -> None:
        cm = self._parsear_distancia(mensaje.payload)
        if cm is None:
            log.warning("Mensaje de distancia inválido: %r", mensaje.payload[:100])
            return
        with self._candado:
            self._historial.append(Lectura(cm=cm, recibida_en=time.time()))
        self._nueva_lectura.set()

    @staticmethod
    def _parsear_distancia(payload: bytes) -> float | None:
        """Acepta {"cm": 42.7} o, como respaldo, un número plano "42.7"."""
        try:
            dato = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None
        if isinstance(dato, dict):
            dato = dato.get("cm")
        if isinstance(dato, (int, float)) and not isinstance(dato, bool):
            return float(dato)
        return None

    # ------------------------------------------------------------------ lecturas

    def ultima_lectura(self, espera_s: float = 0.0) -> Lectura | None:
        """
        Devuelve la última lectura. Si aún no hay ninguna, espera hasta
        `espera_s` segundos a que llegue la primera.
        """
        with self._candado:
            if self._historial:
                return self._historial[-1]
            self._nueva_lectura.clear()
        if espera_s > 0 and self._nueva_lectura.wait(espera_s):
            with self._candado:
                return self._historial[-1] if self._historial else None
        return None

    def historial(self, cantidad: int) -> list[Lectura]:
        """Devuelve las últimas `cantidad` lecturas, de la más antigua a la más reciente."""
        with self._candado:
            return list(self._historial)[-cantidad:]

    # ------------------------------------------------------------------ comandos

    def publicar_comando(self, comando: dict, timeout_s: float = 2.0) -> None:
        """
        Publica un comando JSON con QoS 1 y espera la confirmación del broker.

        Lanza RuntimeError si no hay conexión o el broker no confirma a tiempo.
        """
        if not self.conectado and not self._conectado.wait(timeout_s):
            raise RuntimeError("Sin conexión con el broker MQTT")
        payload = json.dumps(comando, ensure_ascii=False)
        info = self._cliente.publish(self._config.topico_cmd, payload, qos=1)
        info.wait_for_publish(timeout=timeout_s)
        if not info.is_published():
            raise RuntimeError("El broker no confirmó el comando a tiempo")
        log.info("Comando publicado en %s: %s", self._config.topico_cmd, payload)


# ====================================================================== servidor MCP

config = ConfiguracionMQTT.desde_entorno()
mqtt_cliente = ClienteMQTT(config)
mcp = FastMCP("objeto-inteligente-sis234")

# Si la última lectura es más antigua que esto, se avisa que el objeto
# podría estar desconectado (el firmware publica ≥ 2 lecturas/s).
LECTURA_VIEJA_S = 5.0


def _describir_lectura(lectura: Lectura) -> dict:
    antiguedad = round(lectura.antiguedad_s(), 1)
    resultado = {"distancia_cm": round(lectura.cm, 1), "antiguedad_s": antiguedad}
    if antiguedad > LECTURA_VIEJA_S:
        resultado["advertencia"] = (
            "La lectura no es reciente; el objeto inteligente podría estar desconectado."
        )
    return resultado


@mcp.tool()
def obtener_distancia() -> dict:
    """
    Devuelve la distancia más reciente (en cm) medida por el sensor
    ultrasónico del objeto inteligente, y hace cuántos segundos se recibió.
    """
    lectura = mqtt_cliente.ultima_lectura(espera_s=5.0)
    if lectura is None:
        return {
            "error": "Aún no se recibió ninguna lectura del sensor.",
            "broker_conectado": mqtt_cliente.conectado,
            "topico": config.topico_distancia,
        }
    return _describir_lectura(lectura)


@mcp.tool()
def obtener_historial(cantidad: int = 20) -> dict:
    """
    Devuelve las últimas lecturas de distancia (máx. 120, ≈1 minuto) junto
    con estadísticas: mínimo, máximo y promedio en cm. Útil para describir
    tendencias, por ejemplo si un objeto se está acercando o alejando.

    Args:
        cantidad: número de lecturas a devolver (1 a 120).
    """
    cantidad = max(1, min(cantidad, ClienteMQTT.TAMANO_HISTORIAL))
    lecturas = mqtt_cliente.historial(cantidad)
    if not lecturas:
        return {"error": "Aún no se recibió ninguna lectura del sensor."}
    valores = [l.cm for l in lecturas]
    return {
        "lecturas_cm": [round(v, 1) for v in valores],
        "minimo_cm": round(min(valores), 1),
        "maximo_cm": round(max(valores), 1),
        "promedio_cm": round(sum(valores) / len(valores), 1),
        "intervalo_s": round(lecturas[-1].recibida_en - lecturas[0].recibida_en, 1),
    }


@mcp.tool()
def controlar_led(color: Color, estado: Estado) -> dict:
    """
    Enciende o apaga un LED del objeto inteligente.

    Args:
        color: "rojo", "amarillo" o "verde".
        estado: "on" para encender, "off" para apagar.
    """
    try:
        mqtt_cliente.publicar_comando({"led": color, "estado": estado})
    except RuntimeError as error:
        return {"ok": False, "error": str(error)}
    return {"ok": True, "led": color, "estado": estado}


@mcp.tool()
def establecer_leds(rojo: Estado, amarillo: Estado, verde: Estado) -> dict:
    """
    Fija el estado de los tres LEDs a la vez. Útil para instrucciones como
    "enciende el LED rojo y apaga los demás".

    Args:
        rojo: "on" u "off".
        amarillo: "on" u "off".
        verde: "on" u "off".
    """
    estados = {"rojo": rojo, "amarillo": amarillo, "verde": verde}
    try:
        for color, estado in estados.items():
            mqtt_cliente.publicar_comando({"led": color, "estado": estado})
    except RuntimeError as error:
        return {"ok": False, "error": str(error)}
    return {"ok": True, "leds": estados}


@mcp.tool()
def apagar_todos() -> dict:
    """Apaga los tres LEDs del objeto inteligente."""
    return establecer_leds("off", "off", "off")


@mcp.tool()
def estado_conexion() -> dict:
    """
    Informa si el servidor está conectado al broker MQTT, a qué broker y
    tópicos, y cuándo llegó la última lectura del objeto inteligente.
    """
    lectura = mqtt_cliente.ultima_lectura()
    return {
        "broker_conectado": mqtt_cliente.conectado,
        "broker": f"{config.broker}:{config.puerto}",
        "topico_distancia": config.topico_distancia,
        "topico_cmd": config.topico_cmd,
        "ultima_lectura": _describir_lectura(lectura) if lectura else None,
    }


def main() -> None:
    mqtt_cliente.iniciar()
    try:
        mcp.run(transport="stdio")
    finally:
        mqtt_cliente.detener()


if __name__ == "__main__":
    main()
