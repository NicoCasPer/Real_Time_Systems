#include "mode_button.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "freertos/task.h"

esp_err_t mode_button_init(void)
{
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_MODE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&button_config);
}

void mode_button_debounce_init(mode_button_t *button)
{
    if (button == NULL) {
        return;
    }

    const int initial_level = gpio_get_level(BUTTON_MODE_GPIO);
    button->stable_level = initial_level;
    button->last_sample_level = initial_level;
    button->last_change_tick = xTaskGetTickCount();
}

bool mode_button_pressed_event(mode_button_t *button)
{
    if (button == NULL) {
        return false;
    }

    const int sample_level = gpio_get_level(BUTTON_MODE_GPIO);
    const TickType_t now = xTaskGetTickCount();

    if (sample_level != button->last_sample_level) {
        button->last_sample_level = sample_level;
        button->last_change_tick = now;
    }

    if ((now - button->last_change_tick) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_TIME_MS)) {
        if (sample_level != button->stable_level) {
            button->stable_level = sample_level;

            if (button->stable_level == BUTTON_PRESSED_LEVEL) {
                return true;
            }
        }
    }

    return false;
}
