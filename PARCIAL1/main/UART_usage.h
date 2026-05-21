#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*uart_usage_read_temperature_cb_t)(int *out_ntc_raw,
                                                      float *out_temperature_c);
typedef esp_err_t (*uart_usage_read_potentiometer_cb_t)(int *out_pot_raw,
                                                        uint8_t *out_pwm_value);

typedef enum {
    UART_USAGE_TEMPERATURE_UNIT_CELSIUS = 0,
    UART_USAGE_TEMPERATURE_UNIT_FAHRENHEIT,
    UART_USAGE_TEMPERATURE_UNIT_KELVIN,
} uart_usage_temperature_unit_t;

typedef struct {
    uint32_t interval_seconds;
    uart_usage_temperature_unit_t unit;
} uart_usage_temperature_print_config_t;

typedef struct {
    uart_usage_read_temperature_cb_t read_temperature;
    uart_usage_read_potentiometer_cb_t read_potentiometer;
} uart_usage_callbacks_t;

esp_err_t uart_usage_init(const uart_usage_callbacks_t *callbacks);

void uart_usage_get_rgb1_levels(float temperature_c,
                                uint8_t *out_red,
                                uint8_t *out_green,
                                uint8_t *out_blue);
uart_usage_temperature_print_config_t uart_usage_get_temperature_print_config(void);
uart_usage_temperature_unit_t uart_usage_cycle_temperature_unit(void);
float uart_usage_temperature_to_unit(float temperature_c,
                                     uart_usage_temperature_unit_t unit);
const char *uart_usage_temperature_unit_symbol(uart_usage_temperature_unit_t unit);

void uart_usage_update_temperature_snapshot(int ntc_raw, float temperature_c);
void uart_usage_update_rgb2_snapshot(int pot_raw,
                                     uint8_t red,
                                     uint8_t green,
                                     uint8_t blue);

void uart_usage_update_integrated_led_state(uint8_t temperature_threshold,
                                             bool led_is_on);

uint8_t uart_usage_get_temperature_threshold(void);

bool uart_usage_get_integrated_led_state(void);

#ifdef __cplusplus
}
#endif
