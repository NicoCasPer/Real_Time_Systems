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

    /* Inicializar el LED integrado */
    ESP_ERROR_CHECK(app_led_integrated_init());

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

            uart_usage_get_rgb1_levels(reading.temperature_c,
                                       &red,
                                       &green,
                                       &blue);
            ESP_ERROR_CHECK(rgb_led_set(RGB_LED_TEMPERATURE, red, green, blue));
            uart_usage_update_temperature_snapshot(reading.raw, reading.temperature_c);

            // Obtener el umbral actual
            uint8_t threshold = uart_usage_get_temperature_threshold();

            const uart_usage_temperature_print_config_t print_config =
                uart_usage_get_temperature_print_config();
            const float display_temperature =
                uart_usage_temperature_to_unit(reading.temperature_c, print_config.unit);

            ESP_LOGI(TAG, "NTC raw=%d | V=%dmV | R=%.1fohm | T=%.2f%s | RGB1 R=%u G=%u B=%u | Umbral=%u°C",
                     reading.raw,
                     reading.voltage_mv,
                     reading.ntc_resistance_ohms,
                     display_temperature,
                     uart_usage_temperature_unit_symbol(print_config.unit),
                     red,
                     green,
                     blue,
                     (unsigned int)threshold);
        } else {
            ESP_LOGE(TAG, "Error leyendo NTC: %s", esp_err_to_name(err));
        }

        const uart_usage_temperature_print_config_t delay_config =
            uart_usage_get_temperature_print_config();
        TickType_t period_ticks = pdMS_TO_TICKS(delay_config.interval_seconds * 1000U);
        if (period_ticks == 0) {
            period_ticks = 1;
        }

        vTaskDelayUntil(&last_wake_tick, period_ticks);
    }
}

/*
 * integrated_led_control_task()
 * =============================
 * Tarea periódica que controla el LED integrado del ESP32 basado en el umbral
 * de temperatura definido por el potenciómetro y la temperatura actual del NTC.
 *
 * Algoritmo (On-Off Control):
 * ===========================
 * 1. Lee el valor raw del potenciómetro (0-4095 ADC)
 * 2. Mapea linealmente el valor a un umbral de temperatura (0-100°C) usando
 *    app_adc_raw_to_temperature_threshold()
 * 3. Lee la temperatura actual mediante app_adc_read_temperature()
 * 4. Compara: si T_actual > T_umbral → encender LED; si no → apagar LED
 * 5. Ejecuta gpio_set_level() para conmutaciones del GPIO
 * 6. Almacena el umbral y estado del LED en el snapshot protegido por mutex
 *    mediante uart_usage_update_integrated_led_state()
 *
 * Sincronización con FreeRTOS/Mutex:
 * ==================================
 * El acceso al umbral (temperature_threshold) y estado del LED (integrated_led_state)
 * se realiza a través de funciones protegidas por mutex (uart_usage_*).
 * Esto garantiza que:
 *   - La tarea UART puede leer el umbral/estado de forma segura (comando THRESHOLD)
 *   - No hay race conditions entre app_tasks.c y UART_usage.c
 *   - El potenciómetro se lee continuamente sin bloquear la UART
 *
 * Justificación de la Sincronización:
 * ===================================
 * Sin mutex, si dos tareas acceden simultáneamente:
 *   - TAREA 1: Lee temperature_threshold = 50
 *   - TAREA 2: Escribe temperature_threshold = 60 (nuevo valor del potenciómetro)
 *   - TAREA 1: Lee solo 60 (actualización parcial si hay cambio en bits)
 * Esto generaría comportamientos impredecibles. Con mutex, la actualización es
 * atómica desde la perspectiva de otras tareas.
 */
static void integrated_led_control_task(void *arg)
{
    (void)arg;

    TickType_t last_wake_tick = xTaskGetTickCount();

    while (true) {
        int pot_raw = 0;
        uint8_t temperature_threshold = 0;
        adc_calibrate_full_reading_t reading = {0};
        bool led_should_be_on = false;

        /* Paso 1: Leer potenciómetro */
        esp_err_t err = app_adc_read_raw_average(POT_ADC_CHANNEL,
                                                 ADC_AVERAGE_SAMPLES,
                                                 &pot_raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error leyendo potenciometro para umbral: %s", esp_err_to_name(err));
            vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(INTEGRATED_LED_TASK_PERIOD_MS));
            continue;
        }

        /* Paso 2: Mapear potenciómetro a umbral 0-100°C */
        temperature_threshold = app_adc_raw_to_temperature_threshold(pot_raw);

        /* Paso 3: Leer temperatura actual del NTC */
        err = app_adc_read_temperature(&reading);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error leyendo temperatura NTC para LED integrado: %s",
                     esp_err_to_name(err));
            vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(INTEGRATED_LED_TASK_PERIOD_MS));
            continue;
        }

        /* Paso 4: Control On-Off: T_actual > T_umbral → LED ON */
        led_should_be_on = (reading.temperature_c > (float)temperature_threshold);

        /* Paso 5: Conmutar el GPIO del LED integrado */
        err = app_led_integrated_set(led_should_be_on);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error cambiando LED integrado: %s", esp_err_to_name(err));
        }

        /* Paso 6: Almacenar estado en snapshot protegido por mutex */
        uart_usage_update_integrated_led_state(temperature_threshold, led_should_be_on);

        // Mensaje eliminado, solo se imprime en la tarea de temperatura

        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(INTEGRATED_LED_TASK_PERIOD_MS));
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
            const uart_usage_temperature_unit_t temperature_unit =
                uart_usage_cycle_temperature_unit();

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

            ESP_LOGI(TAG, "Unidad de temperatura -> %s",
                     uart_usage_temperature_unit_symbol(temperature_unit));
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

    if (xTaskCreate(integrated_led_control_task,
                    "integrated_led",
                    INTEGRATED_LED_TASK_STACK_SIZE,
                    NULL,
                    INTEGRATED_LED_TASK_PRIORITY,
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
