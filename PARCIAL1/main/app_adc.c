#include "app_adc.h"

#include <stdbool.h>
#include <stdio.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "APP_ADC";

static adc_oneshot_unit_handle_t s_adc1_handle;
static SemaphoreHandle_t s_adc_mutex;
static bool s_initialized;

static esp_err_t configure_adc_channels(void)
{
    adc_oneshot_unit_init_cfg_t adc_unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&adc_unit_config, &s_adc1_handle);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t adc_channel_config = {
        .bitwidth = ADC_BITWIDTH_USED,
        .atten = ADC_ATTENUATION,
    };

    // GPIO34 y GPIO35 pertenecen al ADC1, canales 6 y 7 respectivamente.
    err = adc_oneshot_config_channel(s_adc1_handle, NTC_ADC_CHANNEL, &adc_channel_config);
    if (err != ESP_OK) {
        return err;
    }

    return adc_oneshot_config_channel(s_adc1_handle, POT_ADC_CHANNEL, &adc_channel_config);
}

esp_err_t app_adc_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_adc_mutex = xSemaphoreCreateMutex();
    if (s_adc_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = configure_adc_channels();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar ADC: %s", esp_err_to_name(err));
        return err;
    }

    const adc_calibrate_full_config_t ntc_config = {
        .adc_handle = s_adc1_handle,
        .adc_unit = ADC_UNIT_1,
        .ntc_channel = NTC_ADC_CHANNEL,
        .atten = ADC_ATTENUATION,
        .bitwidth = ADC_BITWIDTH_USED,
        .adc_mutex = s_adc_mutex,
        .samples = NTC_ADC_SAMPLES,
        .vcc_mv = NTC_VCC_MV,
        .series_resistor_ohms = NTC_SERIES_RESISTOR_OHMS,
        .ntc_nominal_ohms = NTC_NOMINAL_RESISTANCE_OHMS,
        .ntc_beta = NTC_BETA_COEFFICIENT,
        .nominal_temperature_c = NTC_NOMINAL_TEMPERATURE_C,
    };

    err = adc_calibrate_full_init(&ntc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar NTC: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t app_adc_read_raw_average(adc_channel_t channel,
                                   int samples,
                                   int *out_raw)
{
    if (out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_adc1_handle == NULL || s_adc_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples <= 0) {
        samples = 1;
    }

    int raw = 0;
    int64_t total = 0;
    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    for (int i = 0; i < samples; i++) {
        err = adc_oneshot_read(s_adc1_handle, channel, &raw);
        if (err != ESP_OK) {
            break;
        }
        total += raw;
    }
    xSemaphoreGive(s_adc_mutex);

    if (err != ESP_OK) {
        return err;
    }

    *out_raw = (int)(total / samples);
    return ESP_OK;
}

uint8_t app_adc_raw_to_pwm_8bit(int raw)
{
    if (raw < 0) {
        raw = 0;
    } else if (raw > ADC_MAX_RAW) {
        raw = ADC_MAX_RAW;
    }

    return (uint8_t)((raw * (int)LEDC_MAX_DUTY + (ADC_MAX_RAW / 2)) / ADC_MAX_RAW);
}

/*
 * MAPEO LINEAL ADC → TEMPERATURA (0-100°C)
 *
 * Fórmula Matemática:
 * ==================
 *
 * Entrada: valor ADC raw (12-bit) = [0, 4095]
 * Salida: temperatura umbral = [0, 100] °C
 *
 * Mapeo lineal simple:
 *   threshold_C = (raw / ADC_MAX_RAW) * 100
 *
 * Donde:
 *   - raw ∈ [0, 4095]  (rango ADC de 12 bits)
 *   - ADC_MAX_RAW = 4095 (máximo valor ADC)
 *   - threshold_C ∈ [0, 100]  (rango de umbral de temperatura)
 *
 * Para evitar truncado en enteros, usamos:
 *   threshold_C = (raw * 100 + ADC_MAX_RAW/2) / ADC_MAX_RAW
 *   (esto añade redondeo: +2047 a numerador antes de dividir entre 4095)
 *
 * Justificación del Mapeo Lineal:
 * ==============================
 * - El potenciómetro proporciona lectura continua sin "zonas muertas"
 * - Una función lineal y = mx + b simplifica la interpretación del usuario
 * - Precisión suficiente: paso mínimo = 100°C / 4095 ≈ 0.0244°C por unidad ADC
 */
uint8_t app_adc_raw_to_temperature_threshold(int raw)
{
    /* Asegurar que raw esté en rango válido */
    if (raw < 0) {
        raw = 0;
    } else if (raw > ADC_MAX_RAW) {
        raw = ADC_MAX_RAW;
    }

    /*
     * Mapeo lineal de [0, 4095] a [0, 100]:
     *   threshold = (raw * 100 + 2047) / 4095
     *
     * El término +2047 (ADC_MAX_RAW/2) realiza redondeo automático,
     * asegurando que cada valor raw se mapee a su entero más cercano en [0, 100].
     */
    return (uint8_t)((raw * 100 + (ADC_MAX_RAW / 2)) / ADC_MAX_RAW);
}

esp_err_t app_adc_read_temperature(adc_calibrate_full_reading_t *out_reading)
{
    return adc_calibrate_full_read_temperature(out_reading);
}

esp_err_t app_adc_read_temperature_for_uart(int *out_ntc_raw,
                                            float *out_temperature_c)
{
    if (out_ntc_raw == NULL || out_temperature_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_calibrate_full_reading_t reading = {0};
    esp_err_t err = app_adc_read_temperature(&reading);
    if (err != ESP_OK) {
        return err;
    }

    *out_ntc_raw = reading.raw;
    *out_temperature_c = reading.temperature_c;
    return ESP_OK;
}

esp_err_t app_adc_read_potentiometer_for_uart(int *out_pot_raw,
                                              uint8_t *out_pwm_value)
{
    if (out_pot_raw == NULL || out_pwm_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int pot_raw = 0;
    esp_err_t err = app_adc_read_raw_average(POT_ADC_CHANNEL,
                                             ADC_AVERAGE_SAMPLES,
                                             &pot_raw);
    if (err != ESP_OK) {
        return err;
    }

    *out_pot_raw = pot_raw;
    *out_pwm_value = app_adc_raw_to_pwm_8bit(pot_raw);
    return ESP_OK;
}

void app_adc_print_initial_check(void)
{
    printf("\n========== Verificacion inicial ADC ==========\n");
    printf("NTC: GPIO34 / ADC1_CH6 | Potenciometro: GPIO35 / ADC1_CH7\n");

    for (int i = 0; i < ADC_INITIAL_CHECK_SAMPLES; i++) {
        int ntc_raw = 0;
        int pot_raw = 0;

        ESP_ERROR_CHECK(app_adc_read_raw_average(NTC_ADC_CHANNEL, 1, &ntc_raw));
        ESP_ERROR_CHECK(app_adc_read_raw_average(POT_ADC_CHANNEL, 1, &pot_raw));

        printf("Muestra %02d -> NTC raw: %4d | POT raw: %4d\n",
               i + 1,
               ntc_raw,
               pot_raw);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    printf("==============================================\n\n");
}

/*
 * app_led_integrated_init()
 * =========================
 * Inicializa el GPIO del LED integrado del ESP32 como salida digital.
 *
 * Configuración:
 *   - GPIO: INTEGRATED_LED_GPIO (definido en app_config.h, típicamente GPIO_NUM_2)
 *   - Modo: GPIO_MODE_OUTPUT
 *   - Estado inicial: nivel bajo (LED apagado)
 *
 * Retorna ESP_OK si la inicialización fue exitosa, o un código de error si falló.
 */
esp_err_t app_led_integrated_init(void)
{
    const gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << INTEGRATED_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&led_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando GPIO del LED integrado: %s", esp_err_to_name(err));
        return err;
    }

    /* Inicializar el LED en estado bajo (apagado) */
    err = gpio_set_level(INTEGRATED_LED_GPIO, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando nivel del LED integrado: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "LED integrado (GPIO %d) inicializado correctamente", INTEGRATED_LED_GPIO);
    return ESP_OK;
}

/*
 * app_led_integrated_set()
 * ========================
 * Establece el nivel lógico del LED integrado.
 *
 * Parámetros:
 *   - level: true para encender (nivel alto), false para apagar (nivel bajo)
 *
 * Retorna ESP_OK si la operación fue exitosa, o un código de error si falló.
 */
esp_err_t app_led_integrated_set(bool level)
{
    esp_err_t err = gpio_set_level(INTEGRATED_LED_GPIO, level ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error cambiando nivel del LED integrado: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
