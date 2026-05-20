/*
 * ADC_calibrate_full.c
 *
 * Modulo reutilizable para leer un NTC con la calibracion de ADC del ESP-IDF.
 * Convierte la lectura raw a mV, calcula la resistencia del NTC y finalmente
 * la temperatura con la ecuacion Beta.
 */

#include "ADC_calibrate_full.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "ADC_CAL_FULL";

typedef enum {
    ADC_CAL_FULL_SCHEME_NONE = 0,
    ADC_CAL_FULL_SCHEME_CURVE,
    ADC_CAL_FULL_SCHEME_LINE,
} adc_calibrate_full_scheme_t;

typedef struct {
    adc_calibrate_full_config_t config;
    adc_cali_handle_t cali_handle;
    adc_calibrate_full_scheme_t scheme;
    bool adc_calibrated;
    bool initialized;
} adc_calibrate_full_state_t;

static adc_calibrate_full_state_t s_state;

static int adc_raw_max_for_bitwidth(adc_bitwidth_t bitwidth)
{
    int bits = (int)bitwidth;

    if (bits <= 0 || bits > 16) {
        bits = 12;
    }

    return (1 << bits) - 1;
}

static int raw_to_voltage_mv_approx(int raw)
{
    const int raw_max = adc_raw_max_for_bitwidth(s_state.config.bitwidth);

    if (raw < 0) {
        raw = 0;
    } else if (raw > raw_max) {
        raw = raw_max;
    }

    return (int)(((float)raw * s_state.config.vcc_mv) / (float)raw_max);
}

static float voltage_to_ntc_resistance(float voltage_mv)
{
    if (voltage_mv <= 0.0f || voltage_mv >= s_state.config.vcc_mv) {
        return -1.0f;
    }

    return (voltage_mv * s_state.config.series_resistor_ohms) /
           (s_state.config.vcc_mv - voltage_mv);
}

static float ntc_resistance_to_temperature(float resistance_ohms)
{
    if (resistance_ohms <= 0.0f) {
        return -999.0f;
    }

    const float nominal_temperature_k = s_state.config.nominal_temperature_c + 273.15f;
    const float inv_temperature_k = (1.0f / nominal_temperature_k) +
                                    (logf(resistance_ohms / s_state.config.ntc_nominal_ohms) /
                                     s_state.config.ntc_beta);

    return (1.0f / inv_temperature_k) - 273.15f;
}

static bool init_adc_calibration(const adc_calibrate_full_config_t *config,
                                 adc_cali_handle_t *out_handle,
                                 adc_calibrate_full_scheme_t *out_scheme)
{
    bool calibrated = false;

    *out_handle = NULL;
    *out_scheme = ADC_CAL_FULL_SCHEME_NONE;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_cfg = {
        .unit_id = config->adc_unit,
        .chan = config->ntc_channel,
        .atten = config->atten,
        .bitwidth = config->bitwidth,
    };
    if (adc_cali_create_scheme_curve_fitting(&curve_cfg, out_handle) == ESP_OK) {
        ESP_LOGI(TAG, "Calibracion ADC: Curve Fitting");
        *out_scheme = ADC_CAL_FULL_SCHEME_CURVE;
        calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t line_cfg = {
            .unit_id = config->adc_unit,
            .atten = config->atten,
            .bitwidth = config->bitwidth,
        };
        if (adc_cali_create_scheme_line_fitting(&line_cfg, out_handle) == ESP_OK) {
            ESP_LOGI(TAG, "Calibracion ADC: Line Fitting");
            *out_scheme = ADC_CAL_FULL_SCHEME_LINE;
            calibrated = true;
        }
    }
#endif

    if (!calibrated) {
        ESP_LOGW(TAG, "Sin calibracion ADC de fabrica; se usara aproximacion lineal");
    }

    return calibrated;
}

static esp_err_t read_raw_average(int *out_raw)
{
    if (out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int samples = s_state.config.samples;
    if (samples <= 0) {
        samples = 1;
    }

    int raw = 0;
    int64_t total = 0;
    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_state.config.adc_mutex, portMAX_DELAY);
    for (int i = 0; i < samples; i++) {
        err = adc_oneshot_read(s_state.config.adc_handle, s_state.config.ntc_channel, &raw);
        if (err != ESP_OK) {
            break;
        }
        total += raw;
    }
    xSemaphoreGive(s_state.config.adc_mutex);

    if (err != ESP_OK) {
        return err;
    }

    *out_raw = (int)(total / samples);
    return ESP_OK;
}

esp_err_t adc_calibrate_full_init(const adc_calibrate_full_config_t *config)
{
    if (config == NULL ||
        config->adc_handle == NULL ||
        config->adc_mutex == NULL ||
        config->samples <= 0 ||
        config->vcc_mv <= 0.0f ||
        config->series_resistor_ohms <= 0.0f ||
        config->ntc_nominal_ohms <= 0.0f ||
        config->ntc_beta <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_calibrate_full_deinit();

    memset(&s_state, 0, sizeof(s_state));
    s_state.config = *config;
    s_state.adc_calibrated = init_adc_calibration(config, &s_state.cali_handle, &s_state.scheme);
    s_state.initialized = true;

    ESP_LOGI(TAG,
             "NTC listo: canal=%d muestras=%d Vcc=%.0fmV Rserie=%.1fohm",
             config->ntc_channel,
             config->samples,
             config->vcc_mv,
             config->series_resistor_ohms);

    return ESP_OK;
}

esp_err_t adc_calibrate_full_read_temperature(adc_calibrate_full_reading_t *out_reading)
{
    if (out_reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t err = read_raw_average(&raw);
    if (err != ESP_OK) {
        return err;
    }

    return adc_calibrate_full_raw_to_temperature(raw, out_reading);
}

esp_err_t adc_calibrate_full_raw_to_temperature(int raw, adc_calibrate_full_reading_t *out_reading)
{
    if (out_reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int voltage_mv = 0;
    if (s_state.adc_calibrated) {
        esp_err_t err = adc_cali_raw_to_voltage(s_state.cali_handle, raw, &voltage_mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        voltage_mv = raw_to_voltage_mv_approx(raw);
    }

    const float resistance_ohms = voltage_to_ntc_resistance((float)voltage_mv);
    const float temperature_c = ntc_resistance_to_temperature(resistance_ohms);

    out_reading->raw = raw;
    out_reading->voltage_mv = voltage_mv;
    out_reading->ntc_resistance_ohms = resistance_ohms;
    out_reading->temperature_c = temperature_c;
    out_reading->adc_calibrated = s_state.adc_calibrated;

    return ESP_OK;
}

bool adc_calibrate_full_is_adc_calibrated(void)
{
    return s_state.initialized && s_state.adc_calibrated;
}

void adc_calibrate_full_deinit(void)
{
    if (!s_state.initialized || s_state.cali_handle == NULL) {
        memset(&s_state, 0, sizeof(s_state));
        return;
    }

    switch (s_state.scheme) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    case ADC_CAL_FULL_SCHEME_CURVE:
        adc_cali_delete_scheme_curve_fitting(s_state.cali_handle);
        break;
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    case ADC_CAL_FULL_SCHEME_LINE:
        adc_cali_delete_scheme_line_fitting(s_state.cali_handle);
        break;
#endif

    default:
        break;
    }

    memset(&s_state, 0, sizeof(s_state));
}
