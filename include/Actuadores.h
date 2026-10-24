/**
 * @file Actuadores.h
 * @brief Control de los tres LEDs (rojo, amarillo, verde) del objeto inteligente.
 *
 * Clase header-only que encapsula los actuadores del ESP32. Recibe los comandos
 * del tópico MQTT `sis234/grupoX/cmd`, los valida y los aplica sobre los LEDs,
 * y genera el mensaje JSON que se publica en `sis234/grupoX/estado`.
 *
 * Contrato MQTT:
 *  - Comando (entrada): {"led": "rojo", "estado": "on"}
 *      led    ∈ {"rojo", "amarillo", "verde", "todos"}
 *      estado ∈ {"on", "off"}
 *  - Estado (salida):   {"rojo": true, "amarillo": false, "verde": false}
 *
 * Ningún método usa delay() ni bloquea: digitalWrite() y el parseo JSON
 * toman microsegundos, de modo que no afectan el tiempo de respuesta (≤ 1 s).
 *
 * Dependencia: ArduinoJson v7 (bblanchon/ArduinoJson@^7).
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// config.h no se versiona (.gitignore); si existe, define los pines reales.
#if __has_include("config.h")
#include "config.h"
#endif

/// Pin GPIO del LED rojo (valor por defecto si config.h no lo define).
#ifndef LED_ROJO_PIN
#define LED_ROJO_PIN 25
#endif

/// Pin GPIO del LED amarillo (valor por defecto si config.h no lo define).
#ifndef LED_AMARILLO_PIN
#define LED_AMARILLO_PIN 26
#endif

/// Pin GPIO del LED verde (valor por defecto si config.h no lo define).
#ifndef LED_VERDE_PIN
#define LED_VERDE_PIN 27
#endif

/**
 * @brief Identificador de cada LED. `Todos` aplica la acción a los tres.
 */
enum class ColorLed : uint8_t {
  Rojo = 0,
  Amarillo = 1,
  Verde = 2,
  Todos,    ///< Los tres LEDs a la vez.
  Invalido  ///< Texto de color no reconocido.
};

/**
 * @class Actuadores
 * @brief Gestiona los LEDs rojo, amarillo y verde y procesa comandos JSON.
 */
class Actuadores {
 public:
  /// Cantidad de LEDs individuales controlados.
  static constexpr uint8_t CANTIDAD_LEDS = 3;

  /**
   * @brief Construye el controlador con los pines de cada LED.
   * @param pinRojo     GPIO del LED rojo.
   * @param pinAmarillo GPIO del LED amarillo.
   * @param pinVerde    GPIO del LED verde.
   */
  Actuadores(uint8_t pinRojo = LED_ROJO_PIN,
             uint8_t pinAmarillo = LED_AMARILLO_PIN,
             uint8_t pinVerde = LED_VERDE_PIN)
      : _pines{pinRojo, pinAmarillo, pinVerde}, _estados{false, false, false} {}

  /**
   * @brief Configura los pines como salida y deja todos los LEDs apagados.
   *
   * Debe llamarse una vez en setup(), después de Serial.begin().
   */
  void iniciar() {
    for (uint8_t i = 0; i < CANTIDAD_LEDS; i++) {
      pinMode(_pines[i], OUTPUT);
    }
    apagarTodos();
    Serial.println(F("[Actuadores] LEDs inicializados (todos apagados)"));
  }

  /**
   * @brief Enciende un LED (o los tres si el color es ColorLed::Todos).
   * @param color LED a encender.
   */
  void encender(ColorLed color) { escribir(color, true); }

  /**
   * @brief Apaga un LED (o los tres si el color es ColorLed::Todos).
   * @param color LED a apagar.
   */
  void apagar(ColorLed color) { escribir(color, false); }

  /**
   * @brief Apaga los tres LEDs.
   */
  void apagarTodos() { escribir(ColorLed::Todos, false); }

  /**
   * @brief Consulta si un LED está encendido.
   * @param color LED a consultar (Rojo, Amarillo o Verde).
   * @return true si está encendido; false si está apagado o el color no es individual.
   */
  bool estadoLed(ColorLed color) const {
    if (!esIndividual(color)) {
      return false;
    }
    return _estados[static_cast<uint8_t>(color)];
  }

  /**
   * @brief Convierte el texto del campo "led" del comando a ColorLed.
   * @param texto "rojo", "amarillo", "verde" o "todos" (minúsculas, según el contrato).
   * @return El color correspondiente o ColorLed::Invalido.
   */
  static ColorLed colorDesdeTexto(const char* texto) {
    if (texto == nullptr) return ColorLed::Invalido;
    if (strcmp(texto, "rojo") == 0) return ColorLed::Rojo;
    if (strcmp(texto, "amarillo") == 0) return ColorLed::Amarillo;
    if (strcmp(texto, "verde") == 0) return ColorLed::Verde;
    if (strcmp(texto, "todos") == 0) return ColorLed::Todos;
    return ColorLed::Invalido;
  }

  /**
   * @brief Valida y aplica un comando recibido por MQTT.
   *
   * Formato esperado: {"led": "<rojo|amarillo|verde|todos>", "estado": "<on|off>"}.
   * Si el JSON está mal formado, faltan campos o los valores no son válidos,
   * el comando se ignora y se registra el motivo por Serial.
   *
   * @param payload  Bytes del mensaje (no necesita terminar en '\0').
   * @param longitud Cantidad de bytes del payload.
   * @return true si el comando se aplicó; false si se descartó.
   */
  bool procesarComando(const char* payload, unsigned int longitud) {
    if (payload == nullptr || longitud == 0) {
      registrarRechazo("payload vacío");
      return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, longitud);
    if (error) {
      Serial.print(F("[Actuadores] Comando ignorado: JSON mal formado ("));
      Serial.print(error.c_str());
      Serial.println(F(")"));
      return false;
    }

    if (!doc.is<JsonObject>()) {
      registrarRechazo("el JSON no es un objeto");
      return false;
    }

    if (!doc["led"].is<const char*>()) {
      registrarRechazo("falta el campo \"led\" o no es texto");
      return false;
    }
    if (!doc["estado"].is<const char*>()) {
      registrarRechazo("falta el campo \"estado\" o no es texto");
      return false;
    }

    const char* textoLed = doc["led"];
    const char* textoEstado = doc["estado"];

    ColorLed color = colorDesdeTexto(textoLed);
    if (color == ColorLed::Invalido) {
      registrarRechazo("color inválido", textoLed);
      return false;
    }

    bool encenderLed;
    if (strcmp(textoEstado, "on") == 0) {
      encenderLed = true;
    } else if (strcmp(textoEstado, "off") == 0) {
      encenderLed = false;
    } else {
      registrarRechazo("estado inválido", textoEstado);
      return false;
    }

    escribir(color, encenderLed);

    Serial.print(F("[Actuadores] Comando aplicado: "));
    Serial.print(textoLed);
    Serial.print(F(" -> "));
    Serial.println(textoEstado);
    return true;
  }

  /**
   * @brief Genera el JSON del tópico estado.
   *
   * Ejemplo: {"rojo":true,"amarillo":false,"verde":false}
   *
   * @param buffer Destino donde se escribe el texto (terminado en '\0').
   * @param tam    Tamaño del buffer en bytes (se recomiendan ≥ 64).
   * @return Longitud del JSON escrito, o 0 si el buffer es insuficiente.
   */
  size_t estadoJson(char* buffer, size_t tam) const {
    if (buffer == nullptr || tam == 0) {
      return 0;
    }

    JsonDocument doc;
    doc["rojo"] = _estados[static_cast<uint8_t>(ColorLed::Rojo)];
    doc["amarillo"] = _estados[static_cast<uint8_t>(ColorLed::Amarillo)];
    doc["verde"] = _estados[static_cast<uint8_t>(ColorLed::Verde)];

    // measureJson no cuenta el '\0' final, por eso se exige tam > longitud.
    if (measureJson(doc) >= tam) {
      Serial.println(F("[Actuadores] Buffer insuficiente para el estado"));
      buffer[0] = '\0';
      return 0;
    }
    return serializeJson(doc, buffer, tam);
  }

 private:
  uint8_t _pines[CANTIDAD_LEDS];   ///< GPIO de cada LED, indexado por ColorLed.
  bool _estados[CANTIDAD_LEDS];    ///< Estado lógico actual de cada LED.

  /// Indica si el color corresponde a un único LED.
  static bool esIndividual(ColorLed color) {
    return color == ColorLed::Rojo || color == ColorLed::Amarillo ||
           color == ColorLed::Verde;
  }

  /**
   * @brief Escribe el nivel en el pin y actualiza el estado guardado.
   * @param color     LED destino (o Todos).
   * @param encendido true para encender, false para apagar.
   */
  void escribir(ColorLed color, bool encendido) {
    if (color == ColorLed::Todos) {
      for (uint8_t i = 0; i < CANTIDAD_LEDS; i++) {
        digitalWrite(_pines[i], encendido ? HIGH : LOW);
        _estados[i] = encendido;
      }
      return;
    }
    if (!esIndividual(color)) {
      return;
    }
    uint8_t i = static_cast<uint8_t>(color);
    digitalWrite(_pines[i], encendido ? HIGH : LOW);
    _estados[i] = encendido;
  }

  /// Registra por Serial el motivo por el que se descartó un comando.
  static void registrarRechazo(const char* motivo, const char* valor = nullptr) {
    Serial.print(F("[Actuadores] Comando ignorado: "));
    Serial.print(motivo);
    if (valor != nullptr) {
      Serial.print(F(" (\""));
      Serial.print(valor);
      Serial.print(F("\")"));
    }
    Serial.println();
  }
};
