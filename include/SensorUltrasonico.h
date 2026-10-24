/**
 * @file SensorUltrasonico.h
 * @brief Medición de distancia con un sensor ultrasónico HC-SR04 (RF1).
 *
 * Para cumplir la exactitud de ±3 cm la clase aplica:
 *  - Compensación de la velocidad del sonido según la temperatura ambiente.
 *  - Descarte de lecturas fuera del rango físico del sensor (timeouts, ecos falsos).
 *  - Filtro de mediana móvil sobre las últimas N muestras, que elimina picos aislados.
 *  - Un offset de calibración opcional, obtenido comparando contra una cinta métrica.
 *
 * El muestreo no bloquea el programa: actualizar() se llama en cada loop() y solo
 * dispara el sensor cuando pasó el intervalo mínimo entre mediciones (~60 ms según
 * la hoja de datos, para que el eco anterior no se confunda con el nuevo).
 */
#ifndef SENSOR_ULTRASONICO_H
#define SENSOR_ULTRASONICO_H

#include <Arduino.h>
#include <math.h>

class SensorUltrasonico {
public:
    /// Capacidad máxima del buffer del filtro de mediana.
    static constexpr uint8_t MAX_MUESTRAS = 15;

    /**
     * @param pinTrig            GPIO conectado a TRIG.
     * @param pinEcho            GPIO conectado a ECHO.
     * @param numMuestras        Tamaño de la ventana del filtro de mediana (1..MAX_MUESTRAS, impar recomendado).
     * @param intervaloMuestreoMs Tiempo mínimo entre disparos del sensor (≥ 60 ms recomendado).
     * @param distanciaMinCm     Distancia mínima válida (HC-SR04: 2 cm).
     * @param distanciaMaxCm     Distancia máxima válida (HC-SR04: 400 cm).
     */
    SensorUltrasonico(uint8_t pinTrig, uint8_t pinEcho,
                      uint8_t numMuestras = 5,
                      uint32_t intervaloMuestreoMs = 60,
                      float distanciaMinCm = 2.0f,
                      float distanciaMaxCm = 400.0f)
        : pinTrig_(pinTrig),
          pinEcho_(pinEcho),
          numMuestras_(constrain(numMuestras, 1, MAX_MUESTRAS)),
          intervaloMuestreoMs_(intervaloMuestreoMs),
          distanciaMinCm_(distanciaMinCm),
          distanciaMaxCm_(distanciaMaxCm) {
        setTemperatura(20.0f);
        for (uint8_t i = 0; i < MAX_MUESTRAS; i++) {
            muestras_[i] = NAN;
        }
    }

    /// Configura los pines. Llamar una vez en setup().
    void begin() {
        pinMode(pinTrig_, OUTPUT);
        pinMode(pinEcho_, INPUT);
        digitalWrite(pinTrig_, LOW);
    }

    /**
     * Ajusta la velocidad del sonido a la temperatura ambiente:
     * v = 331.3 + 0.606·T  [m/s]. Entre 10 °C y 30 °C la diferencia es ~3.5 %,
     * lo que a 1 m de distancia ya equivale a ~3.5 cm de error.
     */
    void setTemperatura(float temperaturaC) {
        const float velocidadMs = 331.3f + 0.606f * temperaturaC;
        velocidadCmPorUs_ = velocidadMs * 100.0f / 1000000.0f;
        // Tiempo de ida y vuelta para la distancia máxima, con 20 % de margen.
        timeoutUs_ = static_cast<uint32_t>((2.0f * distanciaMaxCm_ / velocidadCmPorUs_) * 1.2f);
    }

    /// Corrección fija (cm) que se suma a cada medición; se obtiene al calibrar con cinta métrica.
    void setOffsetCalibracion(float offsetCm) { offsetCm_ = offsetCm; }

    /**
     * Toma una nueva muestra si ya pasó el intervalo de muestreo.
     * @return true si se tomó una muestra en esta llamada.
     */
    bool actualizar() {
        const uint32_t ahora = millis();
        if (ahora - ultimoMuestreoMs_ < intervaloMuestreoMs_) {
            return false;
        }
        ultimoMuestreoMs_ = ahora;

        muestras_[indice_] = medirUnaVez();
        indice_ = (indice_ + 1) % numMuestras_;
        if (muestrasTomadas_ < numMuestras_) {
            muestrasTomadas_++;
        }
        return true;
    }

    /**
     * Distancia filtrada (mediana de las muestras válidas de la ventana).
     * @return Distancia en cm, o NAN si no hay suficientes muestras válidas
     *         (objeto fuera de rango o sensor desconectado).
     */
    float distanciaCm() const {
        float validas[MAX_MUESTRAS];
        uint8_t n = 0;
        for (uint8_t i = 0; i < muestrasTomadas_; i++) {
            if (!isnan(muestras_[i])) {
                validas[n++] = muestras_[i];
            }
        }
        // Se exige mayoría de muestras válidas para no reportar un valor poco confiable.
        if (n == 0 || n < (numMuestras_ / 2 + 1)) {
            return NAN;
        }
        ordenar(validas, n);
        const float mediana = (n % 2 == 1)
                                  ? validas[n / 2]
                                  : (validas[n / 2 - 1] + validas[n / 2]) / 2.0f;
        return mediana + offsetCm_;
    }

    /// true si distanciaCm() tiene un valor válido.
    bool hayLecturaValida() const { return !isnan(distanciaCm()); }

private:
    /**
     * Dispara el sensor una vez y mide el ancho del pulso de ECHO.
     * @return Distancia sin filtrar en cm, o NAN si hubo timeout o está fuera de rango.
     */
    float medirUnaVez() const {
        // Pulso de disparo: TRIG en alto 10 µs.
        digitalWrite(pinTrig_, LOW);
        delayMicroseconds(2);
        digitalWrite(pinTrig_, HIGH);
        delayMicroseconds(10);
        digitalWrite(pinTrig_, LOW);

        const uint32_t duracionUs = pulseIn(pinEcho_, HIGH, timeoutUs_);
        if (duracionUs == 0) {
            return NAN;  // Timeout: no volvió eco.
        }
        // El sonido recorre ida y vuelta, por eso se divide entre 2.
        const float distancia = duracionUs * velocidadCmPorUs_ / 2.0f;
        if (distancia < distanciaMinCm_ || distancia > distanciaMaxCm_) {
            return NAN;
        }
        return distancia;
    }

    /// Ordenamiento por inserción: suficiente para ventanas de pocas muestras.
    static void ordenar(float* datos, uint8_t n) {
        for (uint8_t i = 1; i < n; i++) {
            const float clave = datos[i];
            int8_t j = i - 1;
            while (j >= 0 && datos[j] > clave) {
                datos[j + 1] = datos[j];
                j--;
            }
            datos[j + 1] = clave;
        }
    }

    const uint8_t pinTrig_;
    const uint8_t pinEcho_;
    const uint8_t numMuestras_;
    const uint32_t intervaloMuestreoMs_;
    const float distanciaMinCm_;
    const float distanciaMaxCm_;

    float velocidadCmPorUs_ = 0.0f;
    uint32_t timeoutUs_ = 0;
    float offsetCm_ = 0.0f;

    float muestras_[MAX_MUESTRAS];
    uint8_t indice_ = 0;
    uint8_t muestrasTomadas_ = 0;
    uint32_t ultimoMuestreoMs_ = 0;
};

#endif  // SENSOR_ULTRASONICO_H