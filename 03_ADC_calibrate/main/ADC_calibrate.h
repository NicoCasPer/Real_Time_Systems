#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inicializa la calibración en tiempo de arranque (1 punto):
 *   - Lee el NTC por ADC (promedio de avg_samples)
 *   - Calcula T_model con el modelo Beta (misma conversión que el proyecto)
 *   - Calcula un offset:
 *       T_offset = T_ref - T_model_boot
 *
 * Luego, se aplica en runtime:
 *   T_corrected = T_model + T_offset
 */
esp_err_t adc_calibrate_init(adc_oneshot_unit_handle_t adc_handle,
                             adc_channel_t ntc_channel,
                             SemaphoreHandle_t adc_mutex,
                             float t_ref_c,
                             int avg_samples);

/**
 * Obtiene el offset (°C) calculado en init.
 */
esp_err_t adc_calibrate_get_offset(float *out_offset_c);

/**
 * Aplica la corrección a la temperatura modelo.
 */
esp_err_t adc_calibrate_apply_temperature(float t_model_c, float *out_t_corrected_c);

#ifdef __cplusplus
}
#endif
