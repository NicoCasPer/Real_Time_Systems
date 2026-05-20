#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int stable_level;
    int last_sample_level;
    TickType_t last_change_tick;
} mode_button_t;

esp_err_t mode_button_init(void);
void mode_button_debounce_init(mode_button_t *button);
bool mode_button_pressed_event(mode_button_t *button);

#ifdef __cplusplus
}
#endif
