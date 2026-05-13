/*
 * ADC_calibrate.c
 *
 * Calibración en tiempo de arranque usando 1 punto (temperatura ambiente) tomada
 * de una referencia externa (por ejemplo, el celular).
 *
 * Enfoque:
 *   - Se lee el NTC por ADC (raw)
 *   - Se calcula T_model con el modelo Beta
 *   - Se calcula offset en °C:
 *       T_offset = T_ref - T_model_at_boot
 *   - En runtime:
 *       T_corrected = T_model + T_offset
 *
 * Nota:
 *   - Esto no calibra “físicamente” el ADC del ESP32; calibra la conversión
 *     temperatura-resultado usando el NTC como sensor.
 */

#include "ADC_calibrate.h"

#include <math.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ADC_CALIBRATE";

/* ------------------------- Constantes del modelo ------------------------- */
/* Mantener consistentes con ledc_basic_example_main.c */
#define ADC_MAX_RAW_LOCAL                 4095
#define ADC_BITWIDTH_USED_LOCAL          ADC_BITWIDTH_12

// En ESP-IDF 5.x, ADC_ATTEN_DB_12 reemplaza a ADC_ATTEN_DB_11 para este rango.
// (No se usa aquí porque el raw ya llega “escalado” por la config del driver)
#define ADC_ATTENUATION_LOCAL            ADC_ATTEN_DB_12

#define NTC_NOMINAL_RESISTANCE_OHMS     10000.0f
#define NTC_SERIES_RESISTOR_OHMS        10000.0f
#define NTC_BETA_COEFFICIENT            3950.0f
#define NTC_NOMINAL_TEMPERATURE_C       25.0f

#ifndef NTC_CONNECTED_TO_GND_LOCAL
#define NTC_CONNECTED_TO_GND_LOCAL     1
#endif

/* --------------------------- Estado interno ------------------------------ */

typedef struct {
    float t_ref_c;
    float t_offset_c;
    bool is_initialized;

    adc_oneshot_unit_handle_t adc_handle;
    adc_channel_t ntc_channel;

    SemaphoreHandle_t adc_mutex;

    int avg_samples;
} adc_calibrate_state_t;

static adc_calibrate_state_t s_state;

/* ----------------------------- Utilidades -------------------------------- */

static int read_ntc_raw_average(int samples)
{
    if (samples <= 0) {
        samples = 1;
    }

    int raw = 0;
    int total = 0;

    xSemaphoreTake(s_state.adc_mutex, portMAX_DELAY);
    for (int i = 0; i < samples; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(s_state.adc_handle, s_state.ntc_channel, &raw));
        total += raw;
    }
    xSemaphoreGive(s_state.adc_mutex);

    return total / samples;
}

static float ntc_raw_to_celsius(int raw)
{
    // Evitar divisiones entre cero en los extremos del ADC.
    if (raw <= 0) {
        raw = 1;
    } else if (raw >= ADC_MAX_RAW_LOCAL) {
        raw = ADC_MAX_RAW_LOCAL - 1;
    }

#if NTC_CONNECTED_TO_GND_LOCAL
    const float ntc_resistance = NTC_SERIES_RESISTOR_OHMS *
                                  ((float)raw / (float)(ADC_MAX_RAW_LOCAL - raw));
#else
    const float ntc_resistance = NTC_SERIES_RESISTOR_OHMS *
                                  ((float)(ADC_MAX_RAW_LOCAL - raw) / (float)raw);
#endif

    // Ecuacion Beta: 1/T = 1/T0 + (1/B) * ln(R/R0)
    const float nominal_temperature_k = NTC_NOMINAL_TEMPERATURE_C + 273.15f;
    const float inv_temperature_k = (1.0f / nominal_temperature_k) +
                                     (logf(ntc_resistance / NTC_NOMINAL_RESISTANCE_OHMS) /
                                      NTC_BETA_COEFFICIENT);
    return (1.0f / inv_temperature_k) - 273.15f;
}

/* ----------------------------- API externa ------------------------------- */

esp_err_t adc_calibrate_init(adc_oneshot_unit_handle_t adc_handle,
                               adc_channel_t ntc_channel,
                               SemaphoreHandle_t adc_mutex,
                               float t_ref_c,
                               int avg_samples)
{
    if (adc_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (avg_samples <= 0) {
        avg_samples = 1;
    }

    s_state.adc_handle = adc_handle;
    s_state.ntc_channel = ntc_channel;
    s_state.adc_mutex = adc_mutex;
    s_state.t_ref_c = t_ref_c;
    s_state.avg_samples = avg_samples;

    s_state.is_initialized = false;
    s_state.t_offset_c = 0.0f;

    // Calibration en arranque (1 punto)
    const int ntc_raw = read_ntc_raw_average(s_state.avg_samples);
    const float t_model = ntc_raw_to_celsius(ntc_raw);

    s_state.t_offset_c = s_state.t_ref_c - t_model;
    s_state.is_initialized = true;

    ESP_LOGI(TAG,
             "Boot calib (1 punto): raw=%d | T_model=%.2fC | T_ref=%.2fC | offset=%.2fC",
             ntc_raw, t_model, s_state.t_ref_c, s_state.t_offset_c);

    return ESP_OK;
}

esp_err_t adc_calibrate_get_offset(float *out_offset_c)
{
    if (out_offset_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_offset_c = s_state.t_offset_c;
    return ESP_OK;
}

esp_err_t adc_calibrate_apply_temperature(float t_model_c, float *out_t_corrected_c)
{
    if (out_t_corrected_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_t_corrected_c = t_model_c + s_state.t_offset_c;
    return ESP_OK;
}
