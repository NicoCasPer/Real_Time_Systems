#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RGB_LED_TEMPERATURE = 0,
    RGB_LED_CONFIGURABLE,
    RGB_LED_COUNT,
} rgb_led_id_t;

esp_err_t rgb_led_init(void);
esp_err_t rgb_led_set(rgb_led_id_t led_id, uint8_t red, uint8_t green, uint8_t blue);

#ifdef __cplusplus
}
#endif
