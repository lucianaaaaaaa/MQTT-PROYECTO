/**
 * @file ConexionMQTT.h
 * @brief Conectividad WiFi + cliente MQTT con reconexión automática (RF2, RF3, NFR de reconexión).
 *
 * Responsabilidades:
 *  - Conectarse a la red WiFi y al broker MQTT en la nube.
 *  - Publicar mensajes (p. ej. la distancia medida).
 *  - Suscribirse a tópicos y entregar los mensajes recibidos a un manejador externo
 *    (main.cpp registra ahí el procesamiento de comandos de los actuadores).
 *  - Detectar caídas de WiFi o del broker y reconectarse sola, sin bloquear el loop().
 *    Tras reconectarse vuelve a suscribirse a todos los tópicos registrados.
 *  - Publicar "online"/"offline" en el tópico de conexión como mensaje retenido;
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
    /// Función que recibe cada mensaje: tópico y payload como texto terminado en '\0'.
    using ManejadorMensaje = std::function<void(const char* topico, const char* payload)>;

    static constexpr uint8_t MAX_SUSCRIPCIONES = 4;

    /**
     * @param ssid           Nombre de la red WiFi.
     * @param claveWifi      Contraseña de la red WiFi.
     * @param broker         Host del broker MQTT.
     * @param puerto         Puerto del broker (1883 sin TLS).
     * @param usuario        Usuario del broker ("" si no requiere).
     * @param claveMqtt      Contraseña del broker ("" si no requiere).
     * @param topicoConexion Tópico donde se publica "online"/"offline" (retenido).
     */
    ConexionMQTT(const char* ssid, const char* claveWifi,
                 const char* broker, uint16_t puerto,
                 const char* usuario, const char* claveMqtt,
                 const char* topicoConexion)
        : ssid_(ssid),
          claveWifi_(claveWifi),
          broker_(broker),
          puerto_(puerto),
          usuario_(usuario),
          claveMqtt_(claveMqtt),
          topicoConexion_(topicoConexion),
          mqtt_(clienteWifi_) {}

    /// Inicia WiFi y configura el cliente MQTT. Llamar una vez en setup().
    void begin() {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);         // Sin ahorro de energía: menor latencia en los comandos.
        WiFi.setAutoReconnect(true);  // El driver del ESP32 también reintenta por su cuenta.

        // ID de cliente único a partir de la MAC: en un broker público, dos clientes
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
     * Mantiene viva la conexión. Llamar en cada loop().
     * Revisa en orden: 1) WiFi, 2) broker MQTT, 3) mensajes entrantes.
     * Si algo se cayó, reintenta cada cierto intervalo sin bloquear.
     */
    void loop() {
        const uint32_t ahora = millis();

        // ---- 1. WiFi ----------------------------------------------------------
        if (WiFi.status() != WL_CONNECTED) {
            if (wifiConectado_) {  // Se acaba de perder.
                wifiConectado_ = false;
                mqttConectado_ = false;
                Serial.println("[WiFi] Conexión perdida");
                // Se cierra el socket MQTT para no esperar al keep-alive con una conexión muerta.
                mqtt_.disconnect();
            }
            if (ahora - ultimoIntentoWifiMs_ >= INTERVALO_REINTENTO_WIFI_MS) {
                ultimoIntentoWifiMs_ = ahora;
                Serial.println("[WiFi] Reintentando conexión...");
                WiFi.disconnect();
                WiFi.begin(ssid_, claveWifi_);
            }
            return;  // Sin WiFi no tiene sentido seguir.
        }

        if (!wifiConectado_) {  // Se acaba de conectar.
            wifiConectado_ = true;
            if (conexionesWifi_ > 0) {
                reconexionesWifi_++;  // No es la primera vez: es una reconexión.
            }
            conexionesWifi_++;
            Serial.printf("[WiFi] Conectado. IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            ultimoIntentoMqttMs_ = ahora - INTERVALO_REINTENTO_MQTT_MS;  // Intentar MQTT de inmediato.
        }

        // ---- 2. Broker MQTT ---------------------------------------------------
        if (!mqtt_.connected()) {
            if (mqttConectado_) {  // Se acaba de perder.
                mqttConectado_ = false;
                Serial.printf("[MQTT] Conexión perdida (estado %d)\n", mqtt_.state());
            }
            if (ahora - ultimoIntentoMqttMs_ >= INTERVALO_REINTENTO_MQTT_MS) {
                ultimoIntentoMqttMs_ = ahora;
                conectarBroker();
            }
            return;
        }

        // ---- 3. Todo conectado: procesar mensajes entrantes -------------------
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
     * la suscripción se hace (y se rehace) cada vez que se conecta al broker.
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

    /// true si hay WiFi y conexión con el broker.
    bool conectado() { return WiFi.status() == WL_CONNECTED && mqtt_.connected(); }

    // Contadores para verificar los requerimientos no funcionales (estabilidad y reconexión).
    uint32_t reconexionesWifi() const { return reconexionesWifi_; }
    uint32_t reconexionesMqtt() const { return reconexionesMqtt_; }
    uint32_t mensajesPublicados() const { return mensajesPublicados_; }

private:
    static constexpr uint32_t INTERVALO_REINTENTO_WIFI_MS = 10000;
    static constexpr uint32_t INTERVALO_REINTENTO_MQTT_MS = 3000;
    static constexpr unsigned int TAM_MAX_PAYLOAD = 255;  // Mensajes más largos se recortan.

    /// Intenta conectarse al broker; si lo logra publica "online" y rehace las suscripciones.
    void conectarBroker() {
        Serial.printf("[MQTT] Conectando a %s:%u como %s...\n", broker_, puerto_, idCliente_);

        // PubSubClient espera nullptr (no "") cuando el broker no usa usuario/contraseña.
        const char* usuario = (usuario_ && usuario_[0] != '\0') ? usuario_ : nullptr;
        const char* clave = (claveMqtt_ && claveMqtt_[0] != '\0') ? claveMqtt_ : nullptr;

        // Last Will: si el ESP32 se desconecta sin avisar, el broker publica "offline" (QoS 1, retenido).
        const bool ok = mqtt_.connect(idCliente_, usuario, clave,
                                      topicoConexion_, 1, true, "offline");
        if (!ok) {
            Serial.printf("[MQTT] Falló la conexión (estado %d). Reintento en %lu ms\n",
                          mqtt_.state(), static_cast<unsigned long>(INTERVALO_REINTENTO_MQTT_MS));
            return;
        }

        mqttConectado_ = true;
        if (conexionesMqtt_ > 0) {
            reconexionesMqtt_++;  // No es la primera vez: es una reconexión.
        }
        conexionesMqtt_++;
        Serial.println("[MQTT] Conectado");
        mqtt_.publish(topicoConexion_, "online", true);

        for (uint8_t i = 0; i < numSuscripciones_; i++) {
            mqtt_.subscribe(suscripciones_[i], 1);
            Serial.printf("[MQTT] Suscrito a %s\n", suscripciones_[i]);
        }
    }

    /// Convierte el payload (que no termina en '\0') a texto y lo entrega al manejador.
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

    // Datos de conexión (vienen de config.h a través de main.cpp).
    const char* ssid_;
    const char* claveWifi_;
    const char* broker_;
    const uint16_t puerto_;
    const char* usuario_;
    const char* claveMqtt_;
    const char* topicoConexion_;

    // Cliente MQTT sobre la conexión TCP del WiFi.
    WiFiClient clienteWifi_;
    PubSubClient mqtt_;
    char idCliente_[32] = {};

    // Suscripciones y manejador de mensajes.
    const char* suscripciones_[MAX_SUSCRIPCIONES] = {};
    uint8_t numSuscripciones_ = 0;
    ManejadorMensaje manejador_;

    // Estado de la conexión y tiempos del último intento de reconexión.
    bool wifiConectado_ = false;
    bool mqttConectado_ = false;
    uint32_t ultimoIntentoWifiMs_ = 0;
    uint32_t ultimoIntentoMqttMs_ = 0;

    // Contadores para las pruebas de estabilidad y reconexión.
    uint32_t conexionesWifi_ = 0;
    uint32_t conexionesMqtt_ = 0;
    uint32_t reconexionesWifi_ = 0;
    uint32_t reconexionesMqtt_ = 0;
    uint32_t mensajesPublicados_ = 0;
};

#endif  // CONEXION_MQTT_H
