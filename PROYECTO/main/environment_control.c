#include "environment_control.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char TAG[] = "env_control";

#define ENV_NAMESPACE "env"
#define RGB_TIMER LEDC_TIMER_0
#define FAN_TIMER LEDC_TIMER_1
#define SERVO_TIMER LEDC_TIMER_2
#define RGB_DUTY_MAX 255
#define FAN_DUTY_MAX 1023
// El "servo" es en realidad un motor DC de 2 pines: se controla por
// velocidad (PWM duty 0-100%) a traves de un MOSFET, igual que el ventilador.
#define SERVO_DUTY_MAX 1023
#define NTC_ADC_CHANNEL ADC_CHANNEL_6
#define NTC_ADC_ATTEN ADC_ATTEN_DB_12
#define NTC_ADC_BITWIDTH ADC_BITWIDTH_12
#define NTC_ADC_SAMPLES 64
#define NTC_ADC_MAX_RAW 4095
#define NTC_VCC_MV 3300.0f
// Hardware real: NTC de 10k con resistencia serie fija de 15k.
#define NTC_SERIES_RESISTOR_OHMS 15000.0f
// Calibracion de 1 punto: el NTC midio 34303 ohm a 21 C de ambiente.
// Anclamos la curva Beta a ese punto medido (mas fiable que el R0 teorico).
#define NTC_NOMINAL_RESISTANCE_OHMS 34303.0f
#define NTC_BETA_COEFFICIENT 3950.0f
#define NTC_NOMINAL_TEMPERATURE_C 21.0f

static env_control_state_t s_state = {
    .temperature_c = 0.0f,
    .desired_temp_c = 24.0f,
    .max_temp_c = 32.0f,
    .fan_speed_percent = 0,
    .manual_fan_speed_percent = 0,
    .curtain_opening_percent = 0,
    .curtain_mode = ENV_CURTAIN_MODE_MANUAL,
    .fan_mode = ENV_FAN_MODE_AUTO,
    .rgb_red = 255,
    .rgb_green = 180,
    .rgb_blue = 80,
    .rgb_brightness_percent = 60,
    .alarm_active = false,
};

static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_adc_lock;
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;
static bool s_adc_calibrated;

static uint8_t clamp_percent(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return (uint8_t)value;
}

static void lock_state(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
}

static void unlock_state(void)
{
    xSemaphoreGive(s_state_lock);
}

static void env_save_state(void)
{
    nvs_handle_t handle;

    if (nvs_open(ENV_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed while saving state");
        return;
    }

    nvs_set_blob(handle, "state", &s_state, sizeof(s_state));
    nvs_commit(handle);
    nvs_close(handle);
}

static void env_load_state(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(s_state);

    if (nvs_open(ENV_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    if (nvs_get_blob(handle, "state", &s_state, &size) != ESP_OK || size != sizeof(s_state)) {
        ESP_LOGI(TAG, "Using default environmental settings");
    }

    nvs_close(handle);
}

static void rgb_apply_locked(void)
{
    uint32_t red = (uint32_t)s_state.rgb_red * s_state.rgb_brightness_percent / 100;
    uint32_t green = (uint32_t)s_state.rgb_green * s_state.rgb_brightness_percent / 100;
    uint32_t blue = (uint32_t)s_state.rgb_blue * s_state.rgb_brightness_percent / 100;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, red);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, green);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, blue);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static void fan_apply_locked(uint8_t speed_percent)
{
    uint32_t duty = (uint32_t)clamp_percent(speed_percent) * FAN_DUTY_MAX / 100;

    s_state.fan_speed_percent = clamp_percent(speed_percent);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
}

static void servo_apply_locked(uint8_t opening_percent)
{
    uint32_t duty = (uint32_t)clamp_percent(opening_percent) * SERVO_DUTY_MAX / 100;

    s_state.curtain_opening_percent = clamp_percent(opening_percent);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4);
}

static void env_pwm_init(void)
{
    ledc_timer_config_t rgb_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = RGB_TIMER,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&rgb_timer);

    ledc_timer_config_t fan_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = FAN_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&fan_timer);

    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = SERVO_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&servo_timer);

    const ledc_channel_config_t channels[] = {
        {.gpio_num = ENV_RGB_RED_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .intr_type = LEDC_INTR_DISABLE, .timer_sel = RGB_TIMER, .duty = 0, .hpoint = 0},
        {.gpio_num = ENV_RGB_GREEN_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .intr_type = LEDC_INTR_DISABLE, .timer_sel = RGB_TIMER, .duty = 0, .hpoint = 0},
        {.gpio_num = ENV_RGB_BLUE_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_2, .intr_type = LEDC_INTR_DISABLE, .timer_sel = RGB_TIMER, .duty = 0, .hpoint = 0},
        {.gpio_num = ENV_FAN_PWM_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_3, .intr_type = LEDC_INTR_DISABLE, .timer_sel = FAN_TIMER, .duty = 0, .hpoint = 0},
        {.gpio_num = ENV_SERVO_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_4, .intr_type = LEDC_INTR_DISABLE, .timer_sel = SERVO_TIMER, .duty = 0, .hpoint = 0},
    };

    for (int i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        ledc_channel_config(&channels[i]);
    }
}

static void env_adc_init(void)
{
    s_adc_lock = xSemaphoreCreateMutex();
    if (s_adc_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create ADC mutex");
        return;
    }

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = NTC_ADC_ATTEN,
        .bitwidth = NTC_ADC_BITWIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, NTC_ADC_CHANNEL, &channel_config));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_config = {
        .unit_id = ADC_UNIT_1,
        .chan = NTC_ADC_CHANNEL,
        .atten = NTC_ADC_ATTEN,
        .bitwidth = NTC_ADC_BITWIDTH,
    };
    s_adc_calibrated = adc_cali_create_scheme_curve_fitting(&curve_config, &s_adc_cali_handle) == ESP_OK;
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_adc_calibrated) {
        adc_cali_line_fitting_config_t line_config = {
            .unit_id = ADC_UNIT_1,
            .atten = NTC_ADC_ATTEN,
            .bitwidth = NTC_ADC_BITWIDTH,
        };
        s_adc_calibrated = adc_cali_create_scheme_line_fitting(&line_config, &s_adc_cali_handle) == ESP_OK;
    }
#endif

    if (s_adc_calibrated) {
        ESP_LOGI(TAG, "NTC ADC calibrated on GPIO%d / ADC1_CH6", ENV_TEMP_SENSOR_GPIO);
    } else {
        ESP_LOGW(TAG, "NTC ADC calibration unavailable, using raw approximation");
    }
}

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

static esp_err_t read_adc_raw_average(int *out_raw)
{
    int raw = 0;
    int samples[NTC_ADC_SAMPLES];
    int count = 0;
    esp_err_t err = ESP_OK;

    if (out_raw == NULL || s_adc_handle == NULL || s_adc_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_adc_lock, portMAX_DELAY);
    for (int i = 0; i < NTC_ADC_SAMPLES; i++) {
        err = adc_oneshot_read(s_adc_handle, NTC_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            break;
        }
        samples[count++] = raw;
    }
    xSemaphoreGive(s_adc_lock);

    if (err != ESP_OK || count == 0) {
        return err != ESP_OK ? err : ESP_FAIL;
    }

    // Filtro de mediana: rechaza los picos/outliers tipicos del ruido WiFi
    // y de la alta impedancia del NTC, mucho mejor que un promedio simple.
    qsort(samples, count, sizeof(samples[0]), cmp_int);
    *out_raw = samples[count / 2];
    return ESP_OK;
}

static int raw_to_voltage_mv_approx(int raw)
{
    if (raw < 0) {
        raw = 0;
    } else if (raw > NTC_ADC_MAX_RAW) {
        raw = NTC_ADC_MAX_RAW;
    }

    return (int)(((float)raw * NTC_VCC_MV) / (float)NTC_ADC_MAX_RAW);
}

static float voltage_to_ntc_resistance(float voltage_mv)
{
    if (voltage_mv <= 0.0f || voltage_mv >= NTC_VCC_MV) {
        return -1.0f;
    }

    return (voltage_mv * NTC_SERIES_RESISTOR_OHMS) / (NTC_VCC_MV - voltage_mv);
}

static float ntc_resistance_to_temperature(float resistance_ohms)
{
    const float nominal_temperature_k = NTC_NOMINAL_TEMPERATURE_C + 273.15f;
    float inv_temperature_k;

    if (resistance_ohms <= 0.0f) {
        return s_state.temperature_c;
    }

    inv_temperature_k = (1.0f / nominal_temperature_k) +
                        (logf(resistance_ohms / NTC_NOMINAL_RESISTANCE_OHMS) / NTC_BETA_COEFFICIENT);

    return (1.0f / inv_temperature_k) - 273.15f;
}

static float read_temperature_ntc_c(void)
{
    int raw = 0;
    int voltage_mv = 0;
    float resistance_ohms;

    if (read_adc_raw_average(&raw) != ESP_OK) {
        return s_state.temperature_c;
    }

    if (s_adc_calibrated) {
        adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &voltage_mv);
    } else {
        voltage_mv = raw_to_voltage_mv_approx(raw);
    }

    resistance_ohms = voltage_to_ntc_resistance((float)voltage_mv);
    float temperature_c = ntc_resistance_to_temperature(resistance_ohms);

    // Log temporal de diagnostico de calibracion del NTC.
    ESP_LOGI(TAG, "NTC diag -> raw=%d | V=%dmV | R=%.0fohm | T=%.2fC",
             raw, voltage_mv, resistance_ohms, temperature_c);

    return temperature_c;
}

static void update_auto_fan_locked(void)
{
    float span;
    float ratio;

    if (s_state.fan_mode != ENV_FAN_MODE_AUTO) {
        return;
    }

    if (s_state.temperature_c <= s_state.desired_temp_c) {
        fan_apply_locked(0);
        return;
    }

    if (s_state.temperature_c >= s_state.max_temp_c) {
        fan_apply_locked(100);
        return;
    }

    span = s_state.max_temp_c - s_state.desired_temp_c;
    ratio = (s_state.temperature_c - s_state.desired_temp_c) / span;
    fan_apply_locked((uint8_t)lroundf(ratio * 100.0f));
}

static void env_temperature_task(void *pvParameters)
{
    bool alarm_output = false;
    float filtered_temp = NAN;
    const float ema_alpha = 0.15f;  // Suavizado: menor = mas estable, mas lento.

    while (true) {
        float raw_temp = read_temperature_ntc_c();

        // Filtro de media movil exponencial (EMA) para reducir el baile de
        // la lectura del NTC por ruido. La primera muestra inicializa el filtro.
        if (isnan(filtered_temp)) {
            filtered_temp = raw_temp;
        } else {
            filtered_temp = ema_alpha * raw_temp + (1.0f - ema_alpha) * filtered_temp;
        }
        float temperature = filtered_temp;

        lock_state();
        s_state.temperature_c = temperature;
        s_state.alarm_active = s_state.temperature_c > s_state.max_temp_c;

        if (s_state.alarm_active) {
            // Alarma activa: ventilador al maximo para bajar la temperatura,
            // sin importar el modo (auto o manual).
            fan_apply_locked(100);
            alarm_output = !alarm_output;
            gpio_set_level(ENV_ALARM_LED_GPIO, alarm_output);
        } else {
            // Sin alarma: el ventilador vuelve a su control normal.
            if (s_state.fan_mode == ENV_FAN_MODE_AUTO) {
                update_auto_fan_locked();
            } else {
                fan_apply_locked(s_state.manual_fan_speed_percent);
            }
            alarm_output = false;
            gpio_set_level(ENV_ALARM_LED_GPIO, 0);
        }

        unlock_state();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void env_schedule_task(void *pvParameters)
{
    int last_day = -1;
    int last_hour = -1;
    int last_minute = -1;
    int last_index = -1;

    while (true) {
        time_t now = 0;
        struct tm timeinfo = {0};

        time(&now);
        localtime_r(&now, &timeinfo);

        lock_state();
        if (s_state.curtain_mode == ENV_CURTAIN_MODE_SCHEDULE) {
            ESP_LOGI(TAG, "Agenda: hora actual %02d:%02d (year_ok=%d)",
                     timeinfo.tm_hour, timeinfo.tm_min,
                     timeinfo.tm_year >= (2016 - 1900));
        }
        if (s_state.curtain_mode == ENV_CURTAIN_MODE_SCHEDULE && timeinfo.tm_year >= (2016 - 1900)) {
            for (int i = 0; i < ENV_SCHEDULE_COUNT; i++) {
                env_schedule_record_t *record = &s_state.schedule[i];
                bool already_ran = last_day == timeinfo.tm_yday &&
                                   last_hour == timeinfo.tm_hour &&
                                   last_minute == timeinfo.tm_min &&
                                   last_index == i;

                if (record->enabled &&
                    record->hour == timeinfo.tm_hour &&
                    record->minute == timeinfo.tm_min &&
                    !already_ran) {
                    servo_apply_locked(record->opening_percent);
                    last_day = timeinfo.tm_yday;
                    last_hour = timeinfo.tm_hour;
                    last_minute = timeinfo.tm_min;
                    last_index = i;
                    break;
                }
            }
        }
        unlock_state();

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void env_control_start(void)
{
    if (s_state_lock != NULL) {
        return;
    }

    s_state_lock = xSemaphoreCreateMutex();
    env_load_state();

    gpio_reset_pin(ENV_ALARM_LED_GPIO);
    gpio_set_direction(ENV_ALARM_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ENV_ALARM_LED_GPIO, 0);

    env_pwm_init();
    env_adc_init();

    lock_state();
    rgb_apply_locked();
    servo_apply_locked(s_state.curtain_opening_percent);
    fan_apply_locked(s_state.fan_mode == ENV_FAN_MODE_MANUAL ? s_state.manual_fan_speed_percent : s_state.fan_speed_percent);
    unlock_state();

    xTaskCreatePinnedToCore(env_temperature_task, "env_temp_task", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(env_schedule_task, "env_schedule_task", 4096, NULL, 4, NULL, 1);
}

void env_control_get_state(env_control_state_t *state)
{
    if (state == NULL) {
        return;
    }

    lock_state();
    *state = s_state;
    unlock_state();
}

esp_err_t env_control_set_curtain_manual(uint8_t opening_percent)
{
    lock_state();
    s_state.curtain_mode = ENV_CURTAIN_MODE_MANUAL;
    servo_apply_locked(opening_percent);
    env_save_state();
    unlock_state();

    return ESP_OK;
}

esp_err_t env_control_set_curtain_mode(env_curtain_mode_t mode)
{
    if (mode != ENV_CURTAIN_MODE_MANUAL && mode != ENV_CURTAIN_MODE_SCHEDULE) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    s_state.curtain_mode = mode;
    env_save_state();
    unlock_state();

    return ESP_OK;
}

esp_err_t env_control_set_schedule_record(uint8_t index, bool enabled, uint8_t hour, uint8_t minute, uint8_t opening_percent)
{
    if (index >= ENV_SCHEDULE_COUNT || hour > 23 || minute > 59 || opening_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    s_state.schedule[index].enabled = enabled;
    s_state.schedule[index].hour = hour;
    s_state.schedule[index].minute = minute;
    s_state.schedule[index].opening_percent = opening_percent;
    env_save_state();
    unlock_state();

    return ESP_OK;
}

esp_err_t env_control_set_fan_manual(uint8_t speed_percent)
{
    if (speed_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    s_state.fan_mode = ENV_FAN_MODE_MANUAL;
    s_state.manual_fan_speed_percent = speed_percent;
    fan_apply_locked(speed_percent);
    env_save_state();
    unlock_state();

    return ESP_OK;
}

esp_err_t env_control_set_fan_auto(float desired_temp_c, float max_temp_c)
{
    if (desired_temp_c < -40.0f || max_temp_c > 125.0f || desired_temp_c >= max_temp_c) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    s_state.fan_mode = ENV_FAN_MODE_AUTO;
    s_state.desired_temp_c = desired_temp_c;
    s_state.max_temp_c = max_temp_c;
    update_auto_fan_locked();
    env_save_state();
    unlock_state();

    return ESP_OK;
}

esp_err_t env_control_set_temperatures(float desired_temp_c, float max_temp_c)
{
    if (desired_temp_c < -40.0f || max_temp_c > 125.0f || desired_temp_c >= max_temp_c) {
        return ESP_ERR_INVALID_ARG;
    }

    // Actualiza los umbrales (deseada / maxima) sin cambiar el modo del
    // ventilador. La maxima tambien define el disparo de la alarma.
    lock_state();
    s_state.desired_temp_c = desired_temp_c;
    s_state.max_temp_c = max_temp_c;
    if (s_state.fan_mode == ENV_FAN_MODE_AUTO) {
        update_auto_fan_locked();
    }
    env_save_state();
    unlock_state();

    return ESP_OK;
}

esp_err_t env_control_set_fan_mode(env_fan_mode_t mode)
{
    if (mode != ENV_FAN_MODE_MANUAL && mode != ENV_FAN_MODE_AUTO) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    s_state.fan_mode = mode;
    if (mode == ENV_FAN_MODE_MANUAL) {
        fan_apply_locked(s_state.manual_fan_speed_percent);
    } else {
        update_auto_fan_locked();
    }
    env_save_state();
    unlock_state();

    return ESP_OK;
}

esp_err_t env_control_set_rgb(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness_percent)
{
    if (brightness_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    s_state.rgb_red = red;
    s_state.rgb_green = green;
    s_state.rgb_blue = blue;
    s_state.rgb_brightness_percent = brightness_percent;
    rgb_apply_locked();
    env_save_state();
    unlock_state();

    return ESP_OK;
}
