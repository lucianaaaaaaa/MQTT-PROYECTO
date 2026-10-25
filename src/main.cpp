/**
 * @file main.cpp
 * @brief Objeto Inteligente: ESP32 + sensor ultrasónico HC-SR04 + LEDs, cliente MQTT.
 *
 * Flujo:
 *  - SensorUltrasonico muestrea la distancia cada 60 ms y la filtra (mediana).
 *  - Cada PERIODO_PUBLICACION_MS se publica la distancia filtrada en TOPICO_DISTANCIA.
 *  - ConexionMQTT mantiene WiFi + broker y entrega los comandos de TOPICO_COMANDOS
 *    al manejador de actuadores.
 *
 * Credenciales: copiar config.example.h (raíz del repositorio) como config.h y completarlo.
 */
#include <Arduino.h>

#if __has_include("../config.h")
#include "../config.h"
#else
#error "Falta config.h en la raíz del repositorio: copia config.example.h como config.h y completa tus credenciales."
#endif

#include "ConexionMQTT.h"
#include "SensorUltrasonico.h"

// ----------------------------------------------------------------------------
// Tópicos MQTT (el prefijo debe ser único del grupo en el broker público)
// ----------------------------------------------------------------------------
#define MQTT_PREFIJO "sis234/grupo4"

static const char TOPICO_DISTANCIA[] = MQTT_PREFIJO "/distancia";  // Publica: {"cm": 42.7}
static const char TOPICO_COMANDOS[]  = MQTT_PREFIJO "/cmd";        // Se suscribe: {"led": "rojo", "estado": "on"}
static const char TOPICO_CONEXION[]  = MQTT_PREFIJO "/conexion";   // "online"/"offline" (retenido + Last Will)
// MQTT_PREFIJO "/estado" queda reservado para el estado de los LEDs (clase Actuadores).

// ----------------------------------------------------------------------------
// Hardware
// ----------------------------------------------------------------------------
// TODO: ajustar a los pines definitivos del montaje.
// ATENCIÓN: el HC-SR04 alimentado a 5 V entrega 5 V en ECHO y los GPIO del ESP32
// toleran máximo 3.6 V. Usar un divisor de tensión (p. ej. 1 kΩ en serie + 2 kΩ a GND).
constexpr uint8_t PIN_TRIG = 5;
constexpr uint8_t PIN_ECHO = 18;

// ----------------------------------------------------------------------------
// Parámetros de medición y publicación
// ----------------------------------------------------------------------------
constexpr uint8_t  MUESTRAS_FILTRO          = 5;     // Ventana de la mediana (impar)
constexpr uint32_t INTERVALO_MUESTREO_MS    = 60;    // Mínimo recomendado por la hoja de datos
constexpr float    DISTANCIA_MIN_CM         = 2.0f;
constexpr float    DISTANCIA_MAX_CM         = 400.0f;
constexpr float    TEMPERATURA_AMBIENTE_C   = 20.0f; // Para compensar la velocidad del sonido
constexpr float    OFFSET_CALIBRACION_CM    = 0.0f;  // Ajustar tras medir contra la cinta métrica
constexpr uint32_t PERIODO_PUBLICACION_MS   = 250;   // 4 lecturas/s (requisito: ≥ 2 lecturas/s)
constexpr uint32_t PERIODO_DIAGNOSTICO_MS   = 10000; // Resumen por Serial para las pruebas de NFR

// ----------------------------------------------------------------------------
// Objetos del sistema
// ----------------------------------------------------------------------------
SensorUltrasonico sensor(PIN_TRIG, PIN_ECHO, MUESTRAS_FILTRO, INTERVALO_MUESTREO_MS,
                         DISTANCIA_MIN_CM, DISTANCIA_MAX_CM);

ConexionMQTT conexion(WIFI_SSID, WIFI_PASSWORD,
                      MQTT_BROKER, MQTT_PUERTO, MQTT_USUARIO, MQTT_CLAVE,
                      TOPICO_CONEXION);

// TODO (Integrante 3): #include "Actuadores.h" y crear aquí el objeto de actuadores.

uint32_t ultimaPublicacionMs = 0;
uint32_t ultimoDiagnosticoMs = 0;

/// Publica la distancia filtrada si hay una lectura válida (RF2).
void publicarDistancia() {
    const float distancia = sensor.distanciaCm();
    if (isnan(distancia)) {
        return;  // Objeto fuera de rango: no se publican valores poco confiables.
    }
    char payload[32];
    snprintf(payload, sizeof(payload), "{\"cm\":%.1f}", distancia);
    conexion.publicar(TOPICO_DISTANCIA, payload);
}

/// Imprime un resumen periódico: útil para verificar estabilidad, frecuencia y reconexión.
void imprimirDiagnostico() {
    const float distancia = sensor.distanciaCm();
    Serial.printf("[Estado] uptime=%lus conectado=%s distancia=%s cm publicados=%lu "
                  "reconexiones WiFi=%lu MQTT=%lu heap=%lu\n",
                  static_cast<unsigned long>(millis() / 1000),
                  conexion.conectado() ? "si" : "no",
                  isnan(distancia) ? "--" : String(distancia, 1).c_str(),
                  static_cast<unsigned long>(conexion.mensajesPublicados()),
                  static_cast<unsigned long>(conexion.reconexionesWifi()),
                  static_cast<unsigned long>(conexion.reconexionesMqtt()),
                  static_cast<unsigned long>(ESP.getFreeHeap()));
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== Objeto Inteligente SIS-234 ===");

    sensor.begin();
    sensor.setTemperatura(TEMPERATURA_AMBIENTE_C);
    sensor.setOffsetCalibracion(OFFSET_CALIBRACION_CM);

    // RF3: los comandos llegan por este tópico y se derivan a los actuadores.
    conexion.suscribir(TOPICO_COMANDOS);
    conexion.alRecibirMensaje([](const char* topico, const char* payload) {
        if (strcmp(topico, TOPICO_COMANDOS) == 0) {
            // TODO (Integrante 3): actuadores.procesarComando(payload);
        }
    });

    conexion.begin();
    Serial.printf("Publicando en %s cada %lu ms\n", TOPICO_DISTANCIA,
                  static_cast<unsigned long>(PERIODO_PUBLICACION_MS));
}

void loop() {
    conexion.loop();
    sensor.actualizar();

    const uint32_t ahora = millis();
    if (ahora - ultimaPublicacionMs >= PERIODO_PUBLICACION_MS) {
        ultimaPublicacionMs = ahora;
        publicarDistancia();
    }
    if (ahora - ultimoDiagnosticoMs >= PERIODO_DIAGNOSTICO_MS) {
        ultimoDiagnosticoMs = ahora;
        imprimirDiagnostico();
    }
}
