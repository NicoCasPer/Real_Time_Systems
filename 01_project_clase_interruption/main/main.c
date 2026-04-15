#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"

static QueueHandle_t led_queue = NULL;
static QueueHandle_t button_queue = NULL;

#define LED_GPIO GPIO_NUM_2
#define BUTTON_GPIO GPIO_NUM_0
#define BUTTON_DEBOUNCE_TICKS pdMS_TO_TICKS(200)

typedef enum {
    LED_2_2,
    LED_2_1,
    LED_1_1,
    LED_05_05,
    LED_OFF,
} led_enum_state;

static volatile TickType_t s_last_button_isr_tick = 0;

static const char *led_state_to_string(led_enum_state state)
{
    switch (state) {
        case LED_2_2:
            return "LED_2_2";
        case LED_2_1:
            return "LED_2_1";
        case LED_1_1:
            return "LED_1_1";
        case LED_05_05:
            return "LED_05_05";
        case LED_OFF:
            return "LED_OFF";
        default:
            return "LED_UNKNOWN";
    }
}

static led_enum_state next_state(led_enum_state current)
{
    switch (current) {
        case LED_2_2:
            return LED_2_1;
        case LED_2_1:
            return LED_1_1;
        case LED_1_1:
            return LED_05_05;
        case LED_05_05:
            return LED_OFF;
        case LED_OFF:
        default:
            return LED_2_2;
    }
}

static BaseType_t wait_for_state_change(led_enum_state *state, TickType_t timeout_ticks)
{
    led_enum_state new_state = LED_2_2;

    if (xQueueReceive(led_queue, &new_state, timeout_ticks) == pdTRUE) {
        *state = new_state;
        printf("[led_task] Cambio de estado recibido -> %s\n", led_state_to_string(*state));
        return pdTRUE;
    }

    return pdFALSE;
}

static void config_LED_and_Button(void)
{
    gpio_config_t io_conf = { 0 };

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1ULL << LED_GPIO;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << BUTTON_GPIO;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;

    BaseType_t high_task_wakeup = pdFALSE;
    uint32_t button_event = 1;
    TickType_t now = xTaskGetTickCountFromISR();

    if ((now - s_last_button_isr_tick) >= BUTTON_DEBOUNCE_TICKS) {
        s_last_button_isr_tick = now;
        xQueueSendFromISR(button_queue, &button_event, &high_task_wakeup);
    }

    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void button_task(void *pvParameter)
{
    (void)pvParameter;

    led_enum_state led_state = LED_2_2;
    uint32_t button_event = 0;

    printf("[button_task] Estado inicial -> %s\n", led_state_to_string(led_state));

    while (1) {
        if (xQueueReceive(button_queue, &button_event, portMAX_DELAY) == pdTRUE) {
            printf("[button_task] Evento valido de boton al soltar\n");
            led_state = next_state(led_state);
            printf("[button_task] Nuevo estado -> %s\n", led_state_to_string(led_state));
            (void)xQueueOverwrite(led_queue, &led_state);
        }
    }
}

void led_task(void *pvParameter)
{
    (void)pvParameter;

    led_enum_state led_state = LED_2_2;

    if (xQueueReceive(led_queue, &led_state, 0) == pdTRUE) {
        printf("[led_task] Estado inicial recibido -> %s\n", led_state_to_string(led_state));
    } else {
        printf("[led_task] Usando estado inicial por defecto -> %s\n", led_state_to_string(led_state));
    }

    while (1) {
        switch (led_state) {
            case LED_2_2:
                printf("[led_task] Caso %s\n", led_state_to_string(led_state));
                gpio_set_level(LED_GPIO, 1);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    break;
                }

                gpio_set_level(LED_GPIO, 0);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    break;
                }
                break;

            case LED_2_1:
                printf("[led_task] Caso %s\n", led_state_to_string(led_state));
                gpio_set_level(LED_GPIO, 1);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    break;
                }

                gpio_set_level(LED_GPIO, 0);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    break;
                }
                break;

            case LED_1_1:
                printf("[led_task] Caso %s\n", led_state_to_string(led_state));
                gpio_set_level(LED_GPIO, 1);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    break;
                }

                gpio_set_level(LED_GPIO, 0);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    break;
                }
                break;

            case LED_05_05:
                printf("[led_task] Caso %s\n", led_state_to_string(led_state));
                gpio_set_level(LED_GPIO, 1);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(500)) == pdTRUE) {
                    break;
                }

                gpio_set_level(LED_GPIO, 0);
                if (wait_for_state_change(&led_state, pdMS_TO_TICKS(500)) == pdTRUE) {
                    break;
                }
                break;

            case LED_OFF:
                printf("[led_task] Caso %s\n", led_state_to_string(led_state));
                gpio_set_level(LED_GPIO, 0);
                (void)wait_for_state_change(&led_state, portMAX_DELAY);
                break;

            default:
                led_state = LED_2_2;
                break;
        }
    }
}

void app_main(void)
{
    led_enum_state initial_state = LED_2_2;

    config_LED_and_Button();

    led_queue = xQueueCreate(1, sizeof(led_enum_state));
    button_queue = xQueueCreate(10, sizeof(uint32_t));
    (void)xQueueOverwrite(led_queue, &initial_state);

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL));

    xTaskCreate(&button_task, "button_task", 2048, NULL, 5, NULL);
    xTaskCreate(&led_task, "led_task", 2048, NULL, 5, NULL);
}
