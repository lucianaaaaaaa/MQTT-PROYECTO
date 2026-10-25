/**
 * @file config.example.h
 * @brief Plantilla de configuración del Objeto Inteligente (SIN credenciales reales).
 *
 * Uso:
 *   1. Copiar este archivo como `config.h` en la misma carpeta (raíz del repositorio).
 *   2. Reemplazar los valores de ejemplo por los reales.
 *
 * `config.h` está excluido en .gitignore y NUNCA debe subirse al repositorio.
 */
#ifndef CONFIG_H
#define CONFIG_H

// ----------------------------------------------------------------------------
// Red WiFi (solo 2.4 GHz: el ESP32 no soporta 5 GHz)
// ----------------------------------------------------------------------------
#define WIFI_SSID     "NOMBRE_DE_TU_RED"
#define WIFI_PASSWORD "CLAVE_DE_TU_RED"

// ----------------------------------------------------------------------------
// Broker MQTT en la nube (HiveMQ público: sin TLS ni usuario)
// ----------------------------------------------------------------------------
#define MQTT_BROKER   "broker.hivemq.com"
#define MQTT_PUERTO   1883
#define MQTT_USUARIO  ""   // Vacío en el broker público
#define MQTT_CLAVE    ""   // Vacío en el broker público

#endif  // CONFIG_H