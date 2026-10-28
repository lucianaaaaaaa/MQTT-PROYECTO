/**
 * @file SensorUltrasonico.h
 * @brief Medición de distancia con un sensor ultrasónico HC-SR04 (RF1).
 *
 * Cómo funciona el HC-SR04:
 *  1. Se pone TRIG en alto 10 µs: el sensor emite un pulso de ultrasonido.
 *  2. ECHO queda en alto mientras el sonido va hasta el objeto y vuelve.
 *  3. distancia = (duración del pulso × velocidad del sonido) / 2   (ida y vuelta).
 *
 * Para cumplir la exactitud de ±3 cm la clase aplica:
 *  - Compensación de la velocidad del sonido según la temperatura ambiente.
 *  - Descarte de lecturas fuera del rango físico del sensor (sin eco o ecos falsos).
 *  - Filtro de mediana sobre las últimas N muestras, que elimina picos aislados.
 *  - Un offset de calibración opcional, obtenido comparando contra una cinta métrica.
 *
 * No bloquea el programa: actualizar() se llama en cada loop() y solo dispara el
 * sensor cuando pasó el intervalo mínimo entre mediciones (~60 ms según la hoja de
 * datos, para que el eco anterior no se confunda con el nuevo).
 */
#ifndef SENSOR_ULTRASONICO_H
#define SENSOR_ULTRASONICO_H

#include <Arduino.h>
#include <math.h>

class SensorUltrasonico {
public:
    /// Cantidad máxima de muestras que puede guardar el filtro de mediana.
    static constexpr uint8_t MAX_MUESTRAS = 15;

    /**
     * @param pinTrig             GPIO conectado a TRIG.
     * @param pinEcho             GPIO conectado a ECHO.
     * @param numMuestras         Muestras del filtro de mediana (1..MAX_MUESTRAS, impar recomendado).
     * @param intervaloMuestreoMs Tiempo mínimo entre dos disparos del sensor (≥ 60 ms recomendado).
     * @param distanciaMinCm      Distancia mínima válida (HC-SR04: 2 cm).
     * @param distanciaMaxCm      Distancia máxima válida (HC-SR04: 400 cm).
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
        const float velocidadMetrosPorSeg = 331.3f + 0.606f * temperaturaC;
        velocidadCmPorUs_ = velocidadMetrosPorSeg / 10000.0f;  // m/s → cm/µs

        // Tiempo máximo de espera del eco: ida y vuelta a la distancia máxima, +20 % de margen.
        timeoutUs_ = static_cast<uint32_t>((2.0f * distanciaMaxCm_ / velocidadCmPorUs_) * 1.2f);
    }

    /// Corrección fija (cm) que se suma a cada medición; se obtiene al calibrar con cinta métrica.
    void setOffsetCalibracion(float offsetCm) { offsetCm_ = offsetCm; }

    /// Toma una nueva muestra si ya pasó el intervalo de muestreo. Llamar en cada loop().
    void actualizar() {
        const uint32_t ahora = millis();
        if (ahora - ultimoMuestreoMs_ < intervaloMuestreoMs_) {
            return;  // Todavía no toca medir.
        }
        ultimoMuestreoMs_ = ahora;

        // Buffer circular: la muestra nueva reemplaza a la más antigua.
        muestras_[indice_] = medirUnaVez();
        indice_ = (indice_ + 1) % numMuestras_;
        if (muestrasTomadas_ < numMuestras_) {
            muestrasTomadas_++;
        }
    }

    /**
     * Distancia filtrada: mediana de las muestras válidas guardadas.
     * @return Distancia en cm, o NAN si menos de la mitad de las muestras son válidas
     *         (objeto fuera de rango o sensor desconectado).
     */
    float distanciaCm() const {
        // 1. Copiar solo las muestras válidas (las que no son NAN).
        float validas[MAX_MUESTRAS];
        uint8_t n = 0;
        for (uint8_t i = 0; i < muestrasTomadas_; i++) {
            if (!isnan(muestras_[i])) {
                validas[n++] = muestras_[i];
            }
        }

        // 2. Exigir mayoría de muestras válidas para no reportar un valor poco confiable.
        const uint8_t minimoValidas = numMuestras_ / 2 + 1;
        if (n < minimoValidas) {
            return NAN;
        }

        // 3. Ordenar y tomar el valor del medio.
        ordenar(validas, n);
        float mediana;
        if (n % 2 == 1) {
            mediana = validas[n / 2];
        } else {
            mediana = (validas[n / 2 - 1] + validas[n / 2]) / 2.0f;
        }
        return mediana + offsetCm_;
    }

private:
    /**
     * Dispara el sensor una vez y mide cuánto dura el pulso de ECHO.
     * @return Distancia sin filtrar en cm, o NAN si no volvió eco o está fuera de rango.
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
            return NAN;  // Se agotó el tiempo: no volvió eco.
        }

        // El sonido recorre ida y vuelta, por eso se divide entre 2.
        const float distancia = duracionUs * velocidadCmPorUs_ / 2.0f;
        if (distancia < distanciaMinCm_ || distancia > distanciaMaxCm_) {
            return NAN;  // Fuera del rango físico del sensor.
        }
        return distancia;
    }

    /// Ordena de menor a mayor (inserción): suficiente para pocas muestras.
    static void ordenar(float* datos, uint8_t n) {
        for (uint8_t i = 1; i < n; i++) {
            const float actual = datos[i];
            int8_t j = i - 1;
            while (j >= 0 && datos[j] > actual) {
                datos[j + 1] = datos[j];  // Corre a la derecha los mayores.
                j--;
            }
            datos[j + 1] = actual;
        }
    }

    // Configuración (fija desde el constructor).
    const uint8_t pinTrig_;
    const uint8_t pinEcho_;
    const uint8_t numMuestras_;
    const uint32_t intervaloMuestreoMs_;
    const float distanciaMinCm_;
    const float distanciaMaxCm_;

    // Calibración.
    float velocidadCmPorUs_ = 0.0f;
    uint32_t timeoutUs_ = 0;
    float offsetCm_ = 0.0f;

    // Filtro de mediana. Solo se leen las primeras `muestrasTomadas_` posiciones,
    // que siempre ya fueron escritas por actualizar().
    float muestras_[MAX_MUESTRAS] = {};
    uint8_t indice_ = 0;
    uint8_t muestrasTomadas_ = 0;
    uint32_t ultimoMuestreoMs_ = 0;
};

#endif  // SENSOR_ULTRASONICO_H
