#pragma once

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    adc_oneshot_unit_handle_t adc_handle;
    adc_unit_t adc_unit;
    adc_channel_t ntc_channel;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
    SemaphoreHandle_t adc_mutex;
    int samples;
    float vcc_mv;
    float series_resistor_ohms;
    float ntc_nominal_ohms;
    float ntc_beta;
    float nominal_temperature_c;
} adc_calibrate_full_config_t;

typedef struct {
    int raw;
    int voltage_mv;
    float ntc_resistance_ohms;
    float temperature_c;
    bool adc_calibrated;
} adc_calibrate_full_reading_t;

esp_err_t adc_calibrate_full_init(const adc_calibrate_full_config_t *config);
esp_err_t adc_calibrate_full_read_temperature(adc_calibrate_full_reading_t *out_reading);
esp_err_t adc_calibrate_full_raw_to_temperature(int raw, adc_calibrate_full_reading_t *out_reading);
bool adc_calibrate_full_is_adc_calibrated(void);
void adc_calibrate_full_deinit(void);

#ifdef __cplusplus
}
#endif
