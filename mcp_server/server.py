"""
Servidor MCP del Objeto Inteligente (SIS-234 — Práctica MQTT, grupo 4).

Expone a una herramienta de IA (Claude Desktop) un conjunto de herramientas
MCP para leer la distancia medida por el sensor ultrasónico, consultar y
controlar los LEDs del objeto inteligente y saber si este está en línea.
Toda la comunicación con el objeto pasa por el broker MQTT en la nube: este
servidor es un cliente MQTT más.

    Claude Desktop <--MCP (stdio)--> server.py <--MQTT--> broker <--> ESP32

Contrato MQTT (prefijo sis234/grupo4):
    /distancia  ESP32 -> todos    {"cm": 42.7}
    /cmd        todos -> ESP32    {"led": "rojo|amarillo|verde|todos", "estado": "on|off"}
    /estado     ESP32 -> todos    {"rojo": true, "amarillo": false, "verde": false}
    /conexion   ESP32 -> todos    "online" / "offline" (retenido, Last Will)

Configuración (variables de entorno, ver claude_desktop_config.json):
    MQTT_BROKER        Host del broker            (default: broker.hivemq.com)
    MQTT_PORT          Puerto                      (default: 1883, 8883 si TLS)
    MQTT_TLS           "1" para usar TLS           (default: 0)
    MQTT_USER          Usuario del broker          (opcional)
    MQTT_PASSWORD      Contraseña del broker       (opcional)
    MQTT_PREFIJO       Prefijo de los tópicos      (default: sis234/grupo4)

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
    prefijo: str

    @property
    def topico_distancia(self) -> str:
        return f"{self.prefijo}/distancia"

    @property
    def topico_cmd(self) -> str:
        return f"{self.prefijo}/cmd"

    @property
    def topico_estado(self) -> str:
        return f"{self.prefijo}/estado"

    @property
    def topico_conexion(self) -> str:
        return f"{self.prefijo}/conexion"

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
            prefijo=os.getenv("MQTT_PREFIJO", "sis234/grupo4").rstrip("/"),
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

    - Se suscribe a los tópicos de distancia, estado de LEDs y conexión, y
      guarda la última lectura, un historial acotado, el estado reportado de
      los LEDs y si el objeto inteligente está en línea.
    - Publica comandos JSON en el tópico de comandos.
    - Reconecta automáticamente (paho reintenta en su hilo de red) y vuelve a
      suscribirse en cada conexión.
    """

    TAMANO_HISTORIAL = 120  # Últimas 120 lecturas: 1 min a 2 lecturas/s, 10 min a 1 cada 5 s

    def __init__(self, config: ConfiguracionMQTT) -> None:
        self._config = config
        self._historial: deque[Lectura] = deque(maxlen=self.TAMANO_HISTORIAL)
        self._estado_leds: dict[str, bool] | None = None
        self._estado_leds_en: float | None = None
        self._conexion_objeto: str | None = None  # "online" / "offline"
        self._candado = threading.Lock()
        # Avisa a quien espera un dato (una lectura o un nuevo /estado) que llegó algo.
        self._dato_nuevo = threading.Condition(self._candado)
        self._conectado = threading.Event()

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
        topicos = (
            self._config.topico_distancia,
            self._config.topico_estado,
            self._config.topico_conexion,
        )
        log.info("Conectado al broker; suscribiendo a %s", ", ".join(topicos))
        cliente.subscribe([(topico, 0) for topico in topicos])
        self._conectado.set()

    def _al_desconectar(self, _cliente, _userdata, _flags, reason_code, _properties) -> None:
        self._conectado.clear()
        log.warning("Desconectado del broker (%s); paho reintentará", reason_code)

    def _al_recibir_mensaje(self, _cliente, _userdata, mensaje: mqtt.MQTTMessage) -> None:
        if mensaje.topic == self._config.topico_distancia:
            self._registrar_distancia(mensaje.payload)
        elif mensaje.topic == self._config.topico_estado:
            self._registrar_estado_leds(mensaje.payload)
        elif mensaje.topic == self._config.topico_conexion:
            self._registrar_conexion(mensaje.payload)

    def _registrar_distancia(self, payload: bytes) -> None:
        cm = self._parsear_distancia(payload)
        if cm is None:
            log.warning("Mensaje de distancia inválido: %r", payload[:100])
            return
        with self._candado:
            self._historial.append(Lectura(cm=cm, recibida_en=time.time()))
            self._dato_nuevo.notify_all()

    def _registrar_estado_leds(self, payload: bytes) -> None:
        try:
            dato = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            dato = None
        if not isinstance(dato, dict) or not all(
            isinstance(dato.get(color), bool) for color in COLORES_LED
        ):
            log.warning("Mensaje de estado de LEDs inválido: %r", payload[:100])
            return
        with self._candado:
            self._estado_leds = {color: dato[color] for color in COLORES_LED}
            self._estado_leds_en = time.time()
            self._dato_nuevo.notify_all()

    def _registrar_conexion(self, payload: bytes) -> None:
        texto = payload.decode("utf-8", errors="replace").strip().lower()
        if texto not in ("online", "offline"):
            log.warning("Mensaje de conexión inválido: %r", payload[:100])
            return
        with self._candado:
            self._conexion_objeto = texto
        log.info("Objeto inteligente: %s", texto)

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

    # ------------------------------------------------------------------ consultas

    def ultima_lectura(self, espera_s: float = 0.0) -> Lectura | None:
        """
        Devuelve la última lectura. Si aún no hay ninguna, espera hasta
        `espera_s` segundos a que llegue la primera.
        """
        with self._dato_nuevo:
            self._dato_nuevo.wait_for(lambda: self._historial, timeout=espera_s)
            return self._historial[-1] if self._historial else None

    def historial(self, cantidad: int) -> list[Lectura]:
        """Devuelve las últimas `cantidad` lecturas, de la más antigua a la más reciente."""
        with self._candado:
            return list(self._historial)[-cantidad:]

    def estado_leds(self) -> tuple[dict[str, bool] | None, float | None]:
        """Último estado de LEDs reportado por el objeto y cuándo llegó."""
        with self._candado:
            leds = dict(self._estado_leds) if self._estado_leds else None
            return leds, self._estado_leds_en

    def esperar_estado_leds(
        self, esperado: dict[str, bool], timeout_s: float
    ) -> dict[str, bool] | None:
        """
        Espera hasta `timeout_s` segundos a que el objeto reporte en /estado
        los LEDs de `esperado` con esos valores (los demás no se comparan).

        Devuelve el último estado reportado, coincida o no, o None si el
        objeto nunca informó su estado.
        """

        def coincide() -> bool:
            return self._estado_leds is not None and all(
                self._estado_leds[color] == valor for color, valor in esperado.items()
            )

        with self._dato_nuevo:
            self._dato_nuevo.wait_for(coincide, timeout=timeout_s)
            return dict(self._estado_leds) if self._estado_leds else None

    def conexion_objeto(self) -> str | None:
        """"online", "offline" o None si el objeto aún no lo informó."""
        with self._candado:
            return self._conexion_objeto

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
mcp = FastMCP(
    "objeto-inteligente-sis234",
    instructions=(
        "Controla un objeto inteligente (ESP32) con un sensor ultrasónico y "
        "exactamente tres LEDs: rojo, amarillo y verde. El objeto NO tiene "
        "servomotor ni otros actuadores: si el usuario pide mover un servo, "
        "explícale que este prototipo solo tiene los tres LEDs y no llames "
        "ninguna herramienta. Las distancias están en centímetros."
    ),
)

# Si la última lectura es más antigua que esto, se avisa que no es reciente
# (holgado para cualquier periodo de publicación del firmware de hasta 5 s).
LECTURA_VIEJA_S = 15.0

# Tiempo máximo que se espera a que el objeto confirme un comando en /estado.
CONFIRMACION_S = 3.0


def _describir_lectura(lectura: Lectura) -> dict:
    antiguedad = round(lectura.antiguedad_s(), 1)
    resultado = {"distancia_cm": round(lectura.cm, 1), "antiguedad_s": antiguedad}
    if antiguedad > LECTURA_VIEJA_S:
        resultado["advertencia"] = (
            "La lectura no es reciente: el objeto inteligente podría estar "
            "desconectado o no detectar nada dentro de su rango (2 a 400 cm)."
        )
    return resultado


def _leds_como_texto(leds: dict[str, bool]) -> dict[str, str]:
    return {color: "on" if encendido else "off" for color, encendido in leds.items()}


def _enviar(comando: dict) -> dict | None:
    """Publica un comando; devuelve un dict de error si falla, o None si se envió."""
    try:
        mqtt_cliente.publicar_comando(comando)
    except RuntimeError as error:
        return {"ok": False, "error": str(error)}
    return None


def _ejecutar(comandos: list[dict], esperado: dict[str, bool]) -> dict:
    """
    Publica los comandos y espera a que el objeto confirme en /estado que sus
    LEDs quedaron como `esperado`. Que el broker reciba el comando no basta:
    si el objeto está apagado, nadie lo aplica.
    """
    for comando in comandos:
        error = _enviar(comando)
        if error:
            return error
    leds = mqtt_cliente.esperar_estado_leds(esperado, CONFIRMACION_S)
    confirmado = leds is not None and all(
        leds[color] == valor for color, valor in esperado.items()
    )
    resultado = {
        "ok": True,
        "confirmado": confirmado,
        "leds": _leds_como_texto(leds) if leds else None,
    }
    if not confirmado:
        resultado["advertencia"] = (
            "El comando llegó al broker, pero el objeto inteligente no confirmó "
            f"el cambio en {CONFIRMACION_S:.0f} s (estado del objeto: "
            f"{mqtt_cliente.conexion_objeto() or 'desconocido'})."
        )
    return resultado


@mcp.tool()
def obtener_distancia() -> dict:
    """
    Devuelve la distancia más reciente (en cm) medida por el sensor
    ultrasónico del objeto inteligente, y hace cuántos segundos se recibió.
    """
    lectura = mqtt_cliente.ultima_lectura(espera_s=6.0)  # > periodo de publicación (5 s)
    if lectura is None:
        return {
            "error": "Aún no se recibió ninguna lectura del sensor.",
            "broker_conectado": mqtt_cliente.conectado,
            "objeto_inteligente": mqtt_cliente.conexion_objeto() or "desconocido",
            "topico": config.topico_distancia,
        }
    return _describir_lectura(lectura)


@mcp.tool()
def obtener_historial(cantidad: int = 20) -> dict:
    """
    Devuelve las últimas lecturas de distancia (máx. 120) junto
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
def obtener_estado_leds() -> dict:
    """
    Indica qué LEDs están encendidos, según el último estado que reportó el
    propio objeto inteligente (no el último comando enviado).
    """
    leds, recibido_en = mqtt_cliente.estado_leds()
    if leds is None:
        return {"error": "El objeto inteligente aún no reportó el estado de sus LEDs."}
    return {
        "leds": _leds_como_texto(leds),
        "encendidos": [color for color, encendido in leds.items() if encendido],
        "antiguedad_s": round(time.time() - recibido_en, 1),
    }


@mcp.tool()
def controlar_led(color: Color, estado: Estado) -> dict:
    """
    Enciende o apaga un LED del objeto inteligente. El objeto solo tiene
    LEDs (rojo, amarillo y verde); no tiene servomotor. "confirmado" indica
    si el objeto reportó el cambio; si es false, no afirmes que se aplicó.

    Args:
        color: "rojo", "amarillo" o "verde".
        estado: "on" para encender, "off" para apagar.
    """
    return _ejecutar([{"led": color, "estado": estado}], {color: estado == "on"})


@mcp.tool()
def establecer_leds(rojo: Estado, amarillo: Estado, verde: Estado) -> dict:
    """
    Fija el estado de los tres LEDs a la vez. Útil para instrucciones como
    "enciende el LED rojo y apaga los demás". "confirmado" indica si el
    objeto reportó el cambio; si es false, no afirmes que se aplicó.

    Args:
        rojo: "on" u "off".
        amarillo: "on" u "off".
        verde: "on" u "off".
    """
    estados = {"rojo": rojo, "amarillo": amarillo, "verde": verde}
    if len(set(estados.values())) == 1:
        comandos = [{"led": "todos", "estado": rojo}]
    else:
        comandos = [{"led": color, "estado": estado} for color, estado in estados.items()]
    return _ejecutar(comandos, {color: estado == "on" for color, estado in estados.items()})


@mcp.tool()
def apagar_todos() -> dict:
    """
    Apaga los tres LEDs del objeto inteligente con un único comando.
    "confirmado" indica si el objeto reportó el cambio.
    """
    return _ejecutar(
        [{"led": "todos", "estado": "off"}], {color: False for color in COLORES_LED}
    )


@mcp.tool()
def estado_conexion() -> dict:
    """
    Informa si el servidor está conectado al broker MQTT, si el objeto
    inteligente está en línea (online/offline), los tópicos usados y cuándo
    llegó la última lectura de distancia.
    """
    lectura = mqtt_cliente.ultima_lectura()
    return {
        "broker_conectado": mqtt_cliente.conectado,
        "broker": f"{config.broker}:{config.puerto}",
        "objeto_inteligente": mqtt_cliente.conexion_objeto() or "desconocido",
        "topicos": {
            "distancia": config.topico_distancia,
            "cmd": config.topico_cmd,
            "estado": config.topico_estado,
            "conexion": config.topico_conexion,
        },
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
