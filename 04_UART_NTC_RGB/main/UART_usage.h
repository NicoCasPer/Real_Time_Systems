#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*uart_usage_read_temperature_cb_t)(int *out_ntc_raw,
                                                      float *out_temperature_c);
typedef esp_err_t (*uart_usage_read_potentiometer_cb_t)(int *out_pot_raw,
                                                        uint8_t *out_pwm_value);

typedef struct {
    uart_usage_read_temperature_cb_t read_temperature;
    uart_usage_read_potentiometer_cb_t read_potentiometer;
} uart_usage_callbacks_t;

esp_err_t uart_usage_init(const uart_usage_callbacks_t *callbacks);

void uart_usage_temperature_to_rgb(float temperature_c,
                                   uint8_t on_intensity,
                                   uint8_t *out_red,
                                   uint8_t *out_green,
                                   uint8_t *out_blue);

void uart_usage_update_temperature_snapshot(int ntc_raw, float temperature_c);
void uart_usage_update_rgb2_snapshot(int pot_raw,
                                     uint8_t red,
                                     uint8_t green,
                                     uint8_t blue);

#ifdef __cplusplus
}
#endif
