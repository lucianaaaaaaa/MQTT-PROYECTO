/**
 * @file ConexionMQTT.h
 * @brief Conectividad WiFi + cliente MQTT con reconexión automática (RF2, RF3, NFR de reconexión).
 *
 * Responsabilidades:
 *  - Conectarse a la red WiFi y al broker MQTT en la nube.
 *  - Publicar mensajes (p. ej. la distancia medida).
 *  - Suscribirse a tópicos y entregar los mensajes recibidos a un manejador externo
 *    (la clase Actuadores registra aquí su callback de comandos).
 *  - Detectar caídas de WiFi o del broker y reconectarse sola, sin bloquear el loop().
 *    Tras reconectarse vuelve a suscribirse a todos los tópicos registrados.
 *  - Publicar el estado del dispositivo ("online"/"offline") como mensaje retenido;
 *    "offline" lo publica el broker automáticamente (Last Will) si el ESP32 se cae.
 */
#ifndef CONEXION_MQTT_H
#define CONEXION_MQTT_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>

class ConexionMQTT {
public:
    /// Firma del manejador de mensajes: tópico y payload como texto terminado en '\0'.
    using ManejadorMensaje = std::function<void(const char* topico, const char* payload)>;

    static constexpr uint8_t MAX_SUSCRIPCIONES = 4;

    /**
     * @param ssid          Nombre de la red WiFi.
     * @param claveWifi     Contraseña de la red WiFi.
     * @param broker        Host del broker MQTT.
     * @param puerto        Puerto del broker (1883 sin TLS).
     * @param usuario       Usuario del broker ("" si no requiere).
     * @param claveMqtt     Contraseña del broker ("" si no requiere).
     * @param topicoEstado  Tópico donde se publica "online"/"offline" (retenido).
     */
    ConexionMQTT(const char* ssid, const char* claveWifi,
                 const char* broker, uint16_t puerto,
                 const char* usuario, const char* claveMqtt,
                 const char* topicoEstado)
        : ssid_(ssid),
          claveWifi_(claveWifi),
          broker_(broker),
          puerto_(puerto),
          usuario_(usuario),
          claveMqtt_(claveMqtt),
          topicoEstado_(topicoEstado),
          mqtt_(clienteWifi_) {}

    /// Inicia WiFi y configura el cliente MQTT. Llamar una vez en setup().
    void begin() {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);         // Sin ahorro de energía: menor latencia en los comandos.
        WiFi.setAutoReconnect(true);  // El driver del ESP32 también reintenta por su cuenta.

        // ID de cliente único (basado en la MAC): en un broker público dos clientes
        // con el mismo ID se desconectan mutuamente.
        const uint64_t mac = ESP.getEfuseMac();
        snprintf(idCliente_, sizeof(idCliente_), "sis234-oi-%04X%08X",
                 static_cast<uint16_t>(mac >> 32), static_cast<uint32_t>(mac));

        mqtt_.setServer(broker_, puerto_);
        mqtt_.setKeepAlive(15);       // El broker detecta la caída del ESP32 en ~15-22 s.
        mqtt_.setSocketTimeout(5);
        mqtt_.setCallback([this](char* topico, uint8_t* payload, unsigned int longitud) {
            recibirMensaje(topico, payload, longitud);
        });

        Serial.printf("[WiFi] Conectando a \"%s\"...\n", ssid_);
        WiFi.begin(ssid_, claveWifi_);
        ultimoIntentoWifiMs_ = millis();
    }

    /**
     * Mantiene viva la conexión. Llamar en cada loop(): procesa mensajes entrantes,
     * y si se perdió WiFi o MQTT reintenta cada cierto intervalo sin bloquear.
     */
    void loop() {
        const uint32_t ahora = millis();

        if (WiFi.status() != WL_CONNECTED) {
            if (wifiConectado_) {
                wifiConectado_ = false;
                Serial.println("[WiFi] Conexión perdida");
                // Se cierra el socket MQTT para no esperar al keep-alive con una conexión muerta.
                mqtt_.disconnect();
                mqttConectado_ = false;
            }
            if (ahora - ultimoIntentoWifiMs_ >= INTERVALO_REINTENTO_WIFI_MS) {
                ultimoIntentoWifiMs_ = ahora;
                Serial.println("[WiFi] Reintentando conexión...");
                WiFi.disconnect();
                WiFi.begin(ssid_, claveWifi_);
            }
            return;
        }

        if (!wifiConectado_) {
            wifiConectado_ = true;
            if (conexionesWifi_++ > 0) {
                reconexionesWifi_++;
            }
            Serial.printf("[WiFi] Conectado. IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            ultimoIntentoMqttMs_ = ahora - INTERVALO_REINTENTO_MQTT_MS;  // Intentar MQTT de inmediato.
        }

        if (!mqtt_.connected()) {
            if (mqttConectado_) {
                mqttConectado_ = false;
                Serial.printf("[MQTT] Conexión perdida (estado %d)\n", mqtt_.state());
            }
            if (ahora - ultimoIntentoMqttMs_ >= INTERVALO_REINTENTO_MQTT_MS) {
                ultimoIntentoMqttMs_ = ahora;
                conectarBroker();
            }
            return;
        }

        mqtt_.loop();
    }

    /// Publica un mensaje (QoS 0). @return false si no hay conexión o falló el envío.
    bool publicar(const char* topico, const char* payload, bool retenido = false) {
        if (!mqtt_.connected()) {
            return false;
        }
        const bool ok = mqtt_.publish(topico, payload, retenido);
        if (ok) {
            mensajesPublicados_++;
        }
        return ok;
    }

    /**
     * Registra un tópico de suscripción (QoS 1). Se puede llamar antes de conectar:
     * la suscripción se hace (y se rehace) cada vez que se establece la conexión.
     * @return false si ya se alcanzó MAX_SUSCRIPCIONES.
     */
    bool suscribir(const char* topico) {
        if (numSuscripciones_ >= MAX_SUSCRIPCIONES) {
            return false;
        }
        suscripciones_[numSuscripciones_++] = topico;
        if (mqtt_.connected()) {
            mqtt_.subscribe(topico, 1);
        }
        return true;
    }

    /// Registra la función que recibirá los mensajes de los tópicos suscritos.
    void alRecibirMensaje(ManejadorMensaje manejador) { manejador_ = manejador; }

    bool conectado() { return WiFi.status() == WL_CONNECTED && mqtt_.connected(); }
    const char* idCliente() const { return idCliente_; }

    // Contadores para verificar los requerimientos no funcionales (estabilidad y reconexión).
    uint32_t reconexionesWifi() const { return reconexionesWifi_; }
    uint32_t reconexionesMqtt() const { return reconexionesMqtt_; }
    uint32_t mensajesPublicados() const { return mensajesPublicados_; }

private:
    static constexpr uint32_t INTERVALO_REINTENTO_WIFI_MS = 10000;
    static constexpr uint32_t INTERVALO_REINTENTO_MQTT_MS = 3000;

    /// Intenta conectarse al broker; si lo logra publica "online" y rehace las suscripciones.
    void conectarBroker() {
        Serial.printf("[MQTT] Conectando a %s:%u como %s...\n", broker_, puerto_, idCliente_);

        const char* usuario = (usuario_ && usuario_[0] != '\0') ? usuario_ : nullptr;
        const char* clave = (claveMqtt_ && claveMqtt_[0] != '\0') ? claveMqtt_ : nullptr;

        // Last Will: si el ESP32 se desconecta sin avisar, el broker publica "offline".
        const bool ok = mqtt_.connect(idCliente_, usuario, clave,
                                      topicoEstado_, 1, true, "offline");
        if (!ok) {
            Serial.printf("[MQTT] Falló la conexión (estado %d). Reintento en %lu ms\n",
                          mqtt_.state(), static_cast<unsigned long>(INTERVALO_REINTENTO_MQTT_MS));
            return;
        }

        mqttConectado_ = true;
        if (conexionesMqtt_++ > 0) {
            reconexionesMqtt_++;
        }
        Serial.println("[MQTT] Conectado");
        mqtt_.publish(topicoEstado_, "online", true);

        for (uint8_t i = 0; i < numSuscripciones_; i++) {
            mqtt_.subscribe(suscripciones_[i], 1);
            Serial.printf("[MQTT] Suscrito a %s\n", suscripciones_[i]);
        }
    }

    /// Convierte el payload (no terminado en '\0') a texto y lo entrega al manejador.
    void recibirMensaje(char* topico, uint8_t* payload, unsigned int longitud) {
        char texto[TAM_MAX_PAYLOAD + 1];
        const unsigned int n = min(longitud, static_cast<unsigned int>(TAM_MAX_PAYLOAD));
        memcpy(texto, payload, n);
        texto[n] = '\0';

        Serial.printf("[MQTT] Recibido en %s: %s\n", topico, texto);
        if (manejador_) {
            manejador_(topico, texto);
        }
    }

    static constexpr unsigned int TAM_MAX_PAYLOAD = 255;

    const char* ssid_;
    const char* claveWifi_;
    const char* broker_;
    const uint16_t puerto_;
    const char* usuario_;
    const char* claveMqtt_;
    const char* topicoEstado_;

    WiFiClient clienteWifi_;
    PubSubClient mqtt_;
    char idCliente_[32] = {};

    const char* suscripciones_[MAX_SUSCRIPCIONES] = {};
    uint8_t numSuscripciones_ = 0;
    ManejadorMensaje manejador_;

    bool wifiConectado_ = false;
    bool mqttConectado_ = false;
    uint32_t ultimoIntentoWifiMs_ = 0;
    uint32_t ultimoIntentoMqttMs_ = 0;

    uint32_t conexionesWifi_ = 0;
    uint32_t conexionesMqtt_ = 0;
    uint32_t reconexionesWifi_ = 0;
    uint32_t reconexionesMqtt_ = 0;
    uint32_t mensajesPublicados_ = 0;
};

#endif  // CONEXION_MQTT_H
