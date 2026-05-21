#pragma once

#include <stdint.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

#include "ADC_calibrate_full.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_adc_init(void);

esp_err_t app_adc_read_raw_average(adc_channel_t channel,
                                   int samples,
                                   int *out_raw);
uint8_t app_adc_raw_to_pwm_8bit(int raw);

esp_err_t app_adc_read_temperature(adc_calibrate_full_reading_t *out_reading);
esp_err_t app_adc_read_temperature_for_uart(int *out_ntc_raw,
                                            float *out_temperature_c);
esp_err_t app_adc_read_potentiometer_for_uart(int *out_pot_raw,
                                              uint8_t *out_pwm_value);

uint8_t app_adc_raw_to_temperature_threshold(int raw);

esp_err_t app_led_integrated_init(void);

esp_err_t app_led_integrated_set(bool level);

void app_adc_print_initial_check(void);

#ifdef __cplusplus
}
#endif
