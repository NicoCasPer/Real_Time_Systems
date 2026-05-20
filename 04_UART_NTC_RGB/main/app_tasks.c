#include "app_tasks.h"

#include <stdint.h>

#include "ADC_calibrate_full.h"
#include "UART_usage.h"
#include "app_adc.h"
#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mode_button.h"
#include "rgb_led.h"

typedef enum {
    RGB2_STATE_CONFIG_RED = 0,
    RGB2_STATE_CONFIG_BLUE,
    RGB2_STATE_CONFIG_GREEN,
    RGB2_STATE_SHOW_COLOR,
} rgb2_state_t;

static const char *TAG = "APP_TASKS";

static const char *rgb2_state_name(rgb2_state_t state)
{
    switch (state) {
    case RGB2_STATE_CONFIG_RED:
        return "ConfigRED";
    case RGB2_STATE_CONFIG_BLUE:
        return "ConfigBLUE";
    case RGB2_STATE_CONFIG_GREEN:
        return "ConfigGREEN";
    case RGB2_STATE_SHOW_COLOR:
        return "Show color";
    default:
        return "Estado desconocido";
    }
}

static esp_err_t create_periodic_tasks(void);

static void startup_adc_check_task(void *arg)
{
    (void)arg;

    app_adc_print_initial_check();
    ESP_ERROR_CHECK(create_periodic_tasks());
    vTaskDelete(NULL);
}

static void temperature_rgb1_task(void *arg)
{
    (void)arg;

    TickType_t last_wake_tick = xTaskGetTickCount();

    while (true) {
        adc_calibrate_full_reading_t reading = {0};
        esp_err_t err = app_adc_read_temperature(&reading);
        if (err == ESP_OK) {
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;

            uart_usage_temperature_to_rgb(reading.temperature_c,
                                          (uint8_t)LEDC_MAX_DUTY,
                                          &red,
                                          &green,
                                          &blue);
            ESP_ERROR_CHECK(rgb_led_set(RGB_LED_TEMPERATURE, red, green, blue));
            uart_usage_update_temperature_snapshot(reading.raw, reading.temperature_c);

            ESP_LOGI(TAG, "NTC raw=%d | V=%dmV | R=%.1fohm | T=%.2fC | RGB1 R=%u G=%u B=%u",
                     reading.raw,
                     reading.voltage_mv,
                     reading.ntc_resistance_ohms,
                     reading.temperature_c,
                     red,
                     green,
                     blue);
        } else {
            ESP_LOGE(TAG, "Error leyendo NTC: %s", esp_err_to_name(err));
        }

        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(TEMP_TASK_PERIOD_MS));
    }
}

static void rgb2_state_machine_task(void *arg)
{
    (void)arg;

    rgb2_state_t state = RGB2_STATE_CONFIG_RED;
    uint8_t saved_red = 0;
    uint8_t saved_green = 0;
    uint8_t saved_blue = 0;
    uint8_t current_pwm = 0;
    mode_button_t mode_button;
    TickType_t last_wake_tick = xTaskGetTickCount();

    mode_button_debounce_init(&mode_button);
    ESP_LOGI(TAG, "RGB2 estado inicial: %s", rgb2_state_name(state));

    while (true) {
        int pot_raw = 0;
        esp_err_t err = app_adc_read_raw_average(POT_ADC_CHANNEL,
                                                 ADC_AVERAGE_SAMPLES,
                                                 &pot_raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error leyendo potenciometro: %s", esp_err_to_name(err));
            vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(RGB2_TASK_PERIOD_MS));
            continue;
        }

        current_pwm = app_adc_raw_to_pwm_8bit(pot_raw);

        uint8_t visible_red = 0;
        uint8_t visible_green = 0;
        uint8_t visible_blue = 0;

        switch (state) {
        case RGB2_STATE_CONFIG_RED:
            visible_red = current_pwm;
            break;

        case RGB2_STATE_CONFIG_BLUE:
            visible_blue = current_pwm;
            break;

        case RGB2_STATE_CONFIG_GREEN:
            visible_green = current_pwm;
            break;

        case RGB2_STATE_SHOW_COLOR:
            visible_red = saved_red;
            visible_green = saved_green;
            visible_blue = saved_blue;
            break;

        default:
            state = RGB2_STATE_CONFIG_RED;
            break;
        }

        ESP_ERROR_CHECK(rgb_led_set(RGB_LED_CONFIGURABLE,
                                    visible_red,
                                    visible_green,
                                    visible_blue));
        uart_usage_update_rgb2_snapshot(pot_raw, visible_red, visible_green, visible_blue);

        if (mode_button_pressed_event(&mode_button)) {
            switch (state) {
            case RGB2_STATE_CONFIG_RED:
                saved_red = current_pwm;
                state = RGB2_STATE_CONFIG_BLUE;
                break;

            case RGB2_STATE_CONFIG_BLUE:
                saved_blue = current_pwm;
                state = RGB2_STATE_CONFIG_GREEN;
                break;

            case RGB2_STATE_CONFIG_GREEN:
                saved_green = current_pwm;
                state = RGB2_STATE_SHOW_COLOR;
                break;

            case RGB2_STATE_SHOW_COLOR:
                state = RGB2_STATE_CONFIG_RED;
                break;

            default:
                state = RGB2_STATE_CONFIG_RED;
                break;
            }

            ESP_LOGI(TAG, "RGB2 -> %s | R=%u G=%u B=%u",
                     rgb2_state_name(state),
                     saved_red,
                     saved_green,
                     saved_blue);
        }

        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(RGB2_TASK_PERIOD_MS));
    }
}

static esp_err_t create_periodic_tasks(void)
{
    if (xTaskCreate(temperature_rgb1_task,
                    "temp_rgb1",
                    TEMP_TASK_STACK_SIZE,
                    NULL,
                    TEMP_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(rgb2_state_machine_task,
                    "rgb2_sm",
                    RGB2_TASK_STACK_SIZE,
                    NULL,
                    RGB2_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t app_tasks_start(void)
{
    if (xTaskCreate(startup_adc_check_task,
                    "adc_check",
                    STARTUP_ADC_CHECK_TASK_STACK_SIZE,
                    NULL,
                    STARTUP_ADC_CHECK_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
