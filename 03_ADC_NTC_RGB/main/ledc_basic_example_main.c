/*
 * Laboratorio: ADC + NTC + potenciometro + dos LED RGB en ESP32.
 *
 * - RGB 1 muestra el rango de temperatura medido con un termistor NTC.
 * - RGB 2 usa una maquina de estados: configura rojo, azul, verde y luego
 *   muestra el color guardado usando PWM con el periferico LEDC.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ----------------------------- Pinout usado ----------------------------- */

#define NTC_ADC_CHANNEL         ADC_CHANNEL_6   // GPIO_NUM_34, ADC1_CH6
#define POT_ADC_CHANNEL         ADC_CHANNEL_7   // GPIO_NUM_35, ADC1_CH7
#define BUTTON_MODE_GPIO        GPIO_NUM_16     // Boton con pull-up interno

#define LED_RGB1_RED_GPIO       GPIO_NUM_27
#define LED_RGB1_GREEN_GPIO     GPIO_NUM_4
#define LED_RGB1_BLUE_GPIO      GPIO_NUM_5

#define LED_RGB2_RED_GPIO       GPIO_NUM_12
#define LED_RGB2_GREEN_GPIO     GPIO_NUM_13
#define LED_RGB2_BLUE_GPIO      GPIO_NUM_14

/* ------------------------- Configuracion general ------------------------- */

#define ADC_MAX_RAW                 4095
#define ADC_AVERAGE_SAMPLES         16
#define ADC_INITIAL_CHECK_SAMPLES   10

// En ESP32, 11 dB permite leer practicamente todo el rango 0 V - 3.3 V.
#define ADC_ATTENUATION             ADC_ATTEN_DB_11
#define ADC_BITWIDTH_USED           ADC_BITWIDTH_12

// PWM de 8 bits para mapear directamente el ADC a valores de 0 a 255.
#define LEDC_TIMER                  LEDC_TIMER_0
#define LEDC_MODE                   LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RESOLUTION        LEDC_TIMER_8_BIT
#define LEDC_MAX_DUTY               ((1U << LEDC_DUTY_RESOLUTION) - 1U)
#define LEDC_FREQUENCY_HZ           5000

#define LED_RGB1_RED_CHANNEL        LEDC_CHANNEL_0
#define LED_RGB1_GREEN_CHANNEL      LEDC_CHANNEL_1
#define LED_RGB1_BLUE_CHANNEL       LEDC_CHANNEL_2
#define LED_RGB2_RED_CHANNEL        LEDC_CHANNEL_3
#define LED_RGB2_GREEN_CHANNEL      LEDC_CHANNEL_4
#define LED_RGB2_BLUE_CHANNEL       LEDC_CHANNEL_5

// Cambiar a 1 si el LED RGB es de anodo comun y se enciende con nivel bajo.
#define RGB_LED_COMMON_ANODE        0

#define BUTTON_PRESSED_LEVEL        0
#define BUTTON_DEBOUNCE_TIME_MS     50
#define RGB2_TASK_PERIOD_MS         20
#define TEMP_TASK_PERIOD_MS         1000

/* --------------------------- Parametros del NTC -------------------------- */

// Valores tipicos para un termistor NTC 10k/B3950.
#define NTC_NOMINAL_RESISTANCE_OHMS     10000.0f
#define NTC_SERIES_RESISTOR_OHMS        10000.0f
#define NTC_BETA_COEFFICIENT            3950.0f
#define NTC_NOMINAL_TEMPERATURE_C       25.0f

/*
 * Conexion asumida para el divisor:
 *
 *   3.3 V --- resistencia fija 10k --- ADC(GPIO34) --- NTC --- GND
 *
 * Si el NTC esta conectado a 3.3 V y la resistencia fija a GND, cambiar a 0.
 */
#define NTC_CONNECTED_TO_GND        1

typedef struct {
    ledc_channel_t red_channel;
    ledc_channel_t green_channel;
    ledc_channel_t blue_channel;
} rgb_channels_t;

typedef enum {
    RGB2_STATE_CONFIG_RED = 0,
    RGB2_STATE_CONFIG_BLUE,
    RGB2_STATE_CONFIG_GREEN,
    RGB2_STATE_SHOW_COLOR,
} rgb2_state_t;

typedef struct {
    int stable_level;
    int last_sample_level;
    TickType_t last_change_tick;
} debounce_button_t;

static const char *TAG = "ADC_NTC_RGB";

static const rgb_channels_t LED_RGB1 = {
    .red_channel = LED_RGB1_RED_CHANNEL,
    .green_channel = LED_RGB1_GREEN_CHANNEL,
    .blue_channel = LED_RGB1_BLUE_CHANNEL,
};

static const rgb_channels_t LED_RGB2 = {
    .red_channel = LED_RGB2_RED_CHANNEL,
    .green_channel = LED_RGB2_GREEN_CHANNEL,
    .blue_channel = LED_RGB2_BLUE_CHANNEL,
};

static adc_oneshot_unit_handle_t s_adc1_handle;
static SemaphoreHandle_t s_adc_mutex;

/* ----------------------------- Utilidades LED ---------------------------- */

static uint32_t led_intensity_to_duty(uint8_t intensity)
{
#if RGB_LED_COMMON_ANODE
    return LEDC_MAX_DUTY - intensity;
#else
    return intensity;
#endif
}

static void configure_ledc_channel(gpio_num_t gpio_num, ledc_channel_t channel)
{
    ledc_channel_config_t ledc_channel = {
        .gpio_num = gpio_num,
        .speed_mode = LEDC_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = led_intensity_to_duty(0),
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

static void configure_ledc_pwm(void)
{
    // 1. Configurar el temporizador compartido por los 6 canales PWM.
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 2. Asignar cada color RGB a un canal LEDC independiente.
    configure_ledc_channel(LED_RGB1_RED_GPIO, LED_RGB1_RED_CHANNEL);
    configure_ledc_channel(LED_RGB1_GREEN_GPIO, LED_RGB1_GREEN_CHANNEL);
    configure_ledc_channel(LED_RGB1_BLUE_GPIO, LED_RGB1_BLUE_CHANNEL);

    configure_ledc_channel(LED_RGB2_RED_GPIO, LED_RGB2_RED_CHANNEL);
    configure_ledc_channel(LED_RGB2_GREEN_GPIO, LED_RGB2_GREEN_CHANNEL);
    configure_ledc_channel(LED_RGB2_BLUE_GPIO, LED_RGB2_BLUE_CHANNEL);
}

static void set_rgb_pwm(const rgb_channels_t *rgb, uint8_t red, uint8_t green, uint8_t blue)
{
    // 3. Escribir los ciclos de trabajo y luego actualizarlos en hardware.
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, rgb->red_channel, led_intensity_to_duty(red)));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, rgb->green_channel, led_intensity_to_duty(green)));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, rgb->blue_channel, led_intensity_to_duty(blue)));

    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, rgb->red_channel));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, rgb->green_channel));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, rgb->blue_channel));
}

/* ----------------------------- Utilidades ADC ---------------------------- */

static void configure_adc(void)
{
    adc_oneshot_unit_init_cfg_t adc_unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_unit_config, &s_adc1_handle));

    adc_oneshot_chan_cfg_t adc_channel_config = {
        .bitwidth = ADC_BITWIDTH_USED,
        .atten = ADC_ATTENUATION,
    };

    // 4. GPIO34 y GPIO35 pertenecen al ADC1, canales 6 y 7 respectivamente.
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1_handle, NTC_ADC_CHANNEL, &adc_channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1_handle, POT_ADC_CHANNEL, &adc_channel_config));
}

static int read_adc_raw_average(adc_channel_t channel, int samples)
{
    int raw = 0;
    int total = 0;

    if (samples <= 0) {
        samples = 1;
    }

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    for (int i = 0; i < samples; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc1_handle, channel, &raw));
        total += raw;
    }
    xSemaphoreGive(s_adc_mutex);

    return total / samples;
}

static uint8_t adc_raw_to_pwm_8bit(int raw)
{
    if (raw < 0) {
        raw = 0;
    } else if (raw > ADC_MAX_RAW) {
        raw = ADC_MAX_RAW;
    }

    return (uint8_t)((raw * (int)LEDC_MAX_DUTY + (ADC_MAX_RAW / 2)) / ADC_MAX_RAW);
}

static float ntc_raw_to_celsius(int raw)
{
    // Evitar divisiones entre cero en los extremos del ADC.
    if (raw <= 0) {
        raw = 1;
    } else if (raw >= ADC_MAX_RAW) {
        raw = ADC_MAX_RAW - 1;
    }

#if NTC_CONNECTED_TO_GND
    const float ntc_resistance = NTC_SERIES_RESISTOR_OHMS *
                                 ((float)raw / (float)(ADC_MAX_RAW - raw));
#else
    const float ntc_resistance = NTC_SERIES_RESISTOR_OHMS *
                                 ((float)(ADC_MAX_RAW - raw) / (float)raw);
#endif

    // Ecuacion Beta: 1/T = 1/T0 + (1/B) * ln(R/R0)
    const float nominal_temperature_k = NTC_NOMINAL_TEMPERATURE_C + 273.15f;
    const float inv_temperature_k = (1.0f / nominal_temperature_k) +
                                    (logf(ntc_resistance / NTC_NOMINAL_RESISTANCE_OHMS) /
                                     NTC_BETA_COEFFICIENT);
    return (1.0f / inv_temperature_k) - 273.15f;
}

static void print_initial_adc_check(void)
{
    printf("\n========== Verificacion inicial ADC ==========\n");
    printf("NTC: GPIO34 / ADC1_CH6 | Potenciometro: GPIO35 / ADC1_CH7\n");

    for (int i = 0; i < ADC_INITIAL_CHECK_SAMPLES; i++) {
        const int ntc_raw = read_adc_raw_average(NTC_ADC_CHANNEL, 1);
        const int pot_raw = read_adc_raw_average(POT_ADC_CHANNEL, 1);

        printf("Muestra %02d -> NTC raw: %4d | POT raw: %4d\n",
               i + 1, ntc_raw, pot_raw);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    printf("==============================================\n\n");
}

/* ------------------------------ Boton modo ------------------------------- */

static void configure_mode_button(void)
{
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_MODE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));
}

static bool button_pressed_event(debounce_button_t *button)
{
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

/* ------------------------------- Tarea NTC ------------------------------- */

static void temperature_rgb1_task(void *arg)
{
    (void)arg;

    while (true) {
        // 5. Leer el NTC, convertir a Celsius y elegir un color por rango.
        const int ntc_raw = read_adc_raw_average(NTC_ADC_CHANNEL, ADC_AVERAGE_SAMPLES);
        const float temperature_c = ntc_raw_to_celsius(ntc_raw);

        if (temperature_c < 25.0f) {
            set_rgb_pwm(&LED_RGB1, 0, 0, LEDC_MAX_DUTY);          // Azul
        } else if (temperature_c <= 35.0f) {
            set_rgb_pwm(&LED_RGB1, 0, LEDC_MAX_DUTY, 0);          // Verde
        } else {
            set_rgb_pwm(&LED_RGB1, LEDC_MAX_DUTY, 0, 0);          // Rojo
        }

        ESP_LOGI(TAG, "NTC raw=%d | Temperatura=%.2f C", ntc_raw, temperature_c);
        vTaskDelay(pdMS_TO_TICKS(TEMP_TASK_PERIOD_MS));
    }
}

/* ------------------------- Tarea maquina de estados ---------------------- */

static void rgb2_state_machine_task(void *arg)
{
    (void)arg;

    rgb2_state_t state = RGB2_STATE_CONFIG_RED;
    uint8_t saved_red = 0;
    uint8_t saved_green = 0;
    uint8_t saved_blue = 0;
    uint8_t current_pwm = 0;

    const int initial_button_level = gpio_get_level(BUTTON_MODE_GPIO);
    debounce_button_t mode_button = {
        .stable_level = initial_button_level,
        .last_sample_level = initial_button_level,
        .last_change_tick = xTaskGetTickCount(),
    };

    ESP_LOGI(TAG, "RGB2 estado inicial: %s", rgb2_state_name(state));

    while (true) {
        /*
         * 6. En los estados de configuracion se lee el potenciometro por ADC
         *    y se usa LEDC para variar solo el color activo.
         */
        switch (state) {
        case RGB2_STATE_CONFIG_RED: {
            const int pot_raw = read_adc_raw_average(POT_ADC_CHANNEL, ADC_AVERAGE_SAMPLES);
            current_pwm = adc_raw_to_pwm_8bit(pot_raw);
            set_rgb_pwm(&LED_RGB2, current_pwm, 0, 0);
            break;
        }

        case RGB2_STATE_CONFIG_BLUE: {
            const int pot_raw = read_adc_raw_average(POT_ADC_CHANNEL, ADC_AVERAGE_SAMPLES);
            current_pwm = adc_raw_to_pwm_8bit(pot_raw);
            set_rgb_pwm(&LED_RGB2, 0, 0, current_pwm);
            break;
        }

        case RGB2_STATE_CONFIG_GREEN: {
            const int pot_raw = read_adc_raw_average(POT_ADC_CHANNEL, ADC_AVERAGE_SAMPLES);
            current_pwm = adc_raw_to_pwm_8bit(pot_raw);
            set_rgb_pwm(&LED_RGB2, 0, current_pwm, 0);
            break;
        }

        case RGB2_STATE_SHOW_COLOR:
            // El potenciometro no se lee en este estado: solo se muestra el color final.
            set_rgb_pwm(&LED_RGB2, saved_red, saved_green, saved_blue);
            break;

        default:
            state = RGB2_STATE_CONFIG_RED;
            break;
        }

        /*
         * 7. Cada pulsacion validada con debounce guarda el valor del estado
         *    actual y avanza a la siguiente etapa de la maquina de estados.
         */
        if (button_pressed_event(&mode_button)) {
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
                     rgb2_state_name(state), saved_red, saved_green, saved_blue);
        }

        vTaskDelay(pdMS_TO_TICKS(RGB2_TASK_PERIOD_MS));
    }
}

void app_main(void)
{
    // 8. Inicializar todos los perifericos antes de crear las tareas.
    s_adc_mutex = xSemaphoreCreateMutex();
    configASSERT(s_adc_mutex != NULL);

    configure_adc();
    configure_ledc_pwm();
    configure_mode_button();

    set_rgb_pwm(&LED_RGB1, 0, 0, 0);
    set_rgb_pwm(&LED_RGB2, 0, 0, 0);

    // 9. Verificacion solicitada: imprimir lecturas crudas al arrancar.
    print_initial_adc_check();

    xTaskCreate(temperature_rgb1_task, "temperature_rgb1", 4096, NULL, 5, NULL);
    xTaskCreate(rgb2_state_machine_task, "rgb2_state_machine", 4096, NULL, 5, NULL);
}
