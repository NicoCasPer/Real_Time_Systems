#include "UART_usage.h"
#include "app_adc.h"
#include "app_tasks.h"
#include "esp_err.h"
#include "mode_button.h"
#include "rgb_led.h"

void app_main(void)
{
    ESP_ERROR_CHECK(app_adc_init());
    ESP_ERROR_CHECK(rgb_led_init());
    ESP_ERROR_CHECK(mode_button_init());
    ESP_ERROR_CHECK(rgb_led_set(RGB_LED_TEMPERATURE, 0, 0, 0));
    ESP_ERROR_CHECK(rgb_led_set(RGB_LED_CONFIGURABLE, 0, 0, 0));

    const uart_usage_callbacks_t uart_callbacks = {
        .read_temperature = app_adc_read_temperature_for_uart,
        .read_potentiometer = app_adc_read_potentiometer_for_uart,
    };
    ESP_ERROR_CHECK(uart_usage_init(&uart_callbacks));

    ESP_ERROR_CHECK(app_tasks_start());
}
