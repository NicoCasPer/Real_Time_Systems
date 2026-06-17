/**
 * @file    ntc_adc.c
 * @brief   Lectura de temperatura con NTC 10k y divisor de tensión en GPIO34
 *          Compatible con ESP-IDF v5.x (API nueva de ADC)
 *
 * Circuito:
 *   3.3V ──── [Potenciómetro 10kΩ] ──── GPIO34 ──── [NTC 10kΩ] ──── GND
 *
 *   Vout  = Vcc * R_ntc / (R_pot + R_ntc)
 *   R_ntc = Vout * R_pot / (Vcc - Vout)
 *
 * Temperatura (ecuación Beta de Steinhart-Hart simplificada):
 *   1/T = 1/T0 + (1/Beta) * ln(R_ntc / R0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"    // API nueva IDF v5.x
#include "esp_adc/adc_cali.h"       // Calibración nueva IDF v5.x
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

/* ─────────────────────────────────────────────
   PARÁMETROS DEL HARDWARE — ajusta si es necesario
   ───────────────────────────────────────────── */
#define ADC_UNIT            ADC_UNIT_1
#define ADC_CHANNEL         ADC_CHANNEL_6    // GPIO34 → ADC1_CH6
#define ADC_ATTEN           ADC_ATTEN_DB_12  // Rango: 0 – 3.3 V (DB_12 en IDF v5.x)
#define ADC_SAMPLES         64               // Número de muestras promediadas por lectura

#define VCC_MV              3300.0f          // Tensión de alimentación (mV)
#define R_POT_OHMS          10000.0f         // Resistencia del potenciómetro (Ω) — mide con multímetro
#define R_NTC_NOMINAL       10000.0f         // Resistencia NTC a 25 °C (Ω)
#define NTC_BETA            3950.0f          // Coeficiente Beta del NTC (ver datasheet)
#define T0_KELVIN           298.15f          // Temperatura de referencia: 25 °C en Kelvin

static const char *TAG = "NTC_ADC";

/* ─────────────────────────────────────────────
   Promedia N lecturas crudas y las convierte a mV
   ───────────────────────────────────────────── */
static uint32_t leer_voltaje_mv(adc_oneshot_unit_handle_t adc_handle,
                                adc_cali_handle_t         cali_handle,
                                bool                      calibrado)
{
    int64_t acumulado = 0;
    int raw = 0;

    for (int i = 0; i < ADC_SAMPLES; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL, &raw));
        acumulado += raw;
    }
    int raw_prom = (int)(acumulado / ADC_SAMPLES);

    int voltaje_mv = 0;
    if (calibrado) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, raw_prom, &voltaje_mv));
    } else {
        /* Sin calibración: aproximación lineal básica (menos precisa) */
        voltaje_mv = (int)((raw_prom * VCC_MV) / 4095.0f);
    }

    return (uint32_t)voltaje_mv;
}

/* ─────────────────────────────────────────────
   Calcula R_ntc a partir de la tensión medida
   ───────────────────────────────────────────── */
static float calcular_resistencia_ntc(float voltaje_mv)
{
    if (voltaje_mv <= 0.0f || voltaje_mv >= VCC_MV) {
        return -1.0f;   // Fuera de rango
    }
    return (voltaje_mv * R_POT_OHMS) / (VCC_MV - voltaje_mv);
}

/* ─────────────────────────────────────────────
   Convierte R_ntc a temperatura en °C (Beta equation)
   ───────────────────────────────────────────── */
static float calcular_temperatura(float r_ntc)
{
    if (r_ntc <= 0.0f) return -999.0f;

    float inv_T = (1.0f / T0_KELVIN) + (1.0f / NTC_BETA) * logf(r_ntc / R_NTC_NOMINAL);
    return (1.0f / inv_T) - 273.15f;
}

/* ─────────────────────────────────────────────
   Inicializar calibración (Curve Fitting o Line Fitting
   según el chip; el SDK elige automáticamente)
   ───────────────────────────────────────────── */
static bool init_calibracion(adc_unit_t unit, adc_channel_t channel,
                              adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    bool calibrado = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = unit,
        .chan     = channel,
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, out_handle) == ESP_OK) {
        ESP_LOGI(TAG, "Calibración: Curve Fitting ✓");
        calibrado = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrado) {
        adc_cali_line_fitting_config_t cfg = {
            .unit_id   = unit,
            .atten     = atten,
            .bitwidth  = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cfg, out_handle) == ESP_OK) {
            ESP_LOGI(TAG, "Calibración: Line Fitting ✓");
            calibrado = true;
        }
    }
#endif

    if (!calibrado) {
        ESP_LOGW(TAG, "Sin calibración de fábrica — precisión reducida");
    }
    return calibrado;
}

/* ─────────────────────────────────────────────
   Punto de entrada principal
   ───────────────────────────────────────────── */
void app_main(void)
{
    /* 1. Crear unidad ADC en modo one-shot */
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    /* 2. Configurar el canal (atenuación y resolución) */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT, // 12 bits en ESP32
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL, &chan_cfg));

    /* 3. Inicializar calibración */
    adc_cali_handle_t cali_handle = NULL;
    bool calibrado = init_calibracion(ADC_UNIT, ADC_CHANNEL, ADC_ATTEN, &cali_handle);

    ESP_LOGI(TAG, "Iniciando — GPIO34 | 115200 baud");
    ESP_LOGI(TAG, "Circuito: 3.3V—[POT %.0f Ω]—GPIO34—[NTC %.0f Ω]—GND",
             R_POT_OHMS, R_NTC_NOMINAL);
    ESP_LOGI(TAG, "─────────────────────────────────────────────────────────");

    /* 4. Bucle de lectura cada 1 segundo */
    while (1) {
        uint32_t v_mv        = leer_voltaje_mv(adc_handle, cali_handle, calibrado);
        float    r_ntc       = calcular_resistencia_ntc((float)v_mv);
        float    temperatura = calcular_temperatura(r_ntc);

        if (r_ntc > 0.0f) {
            printf("[NTC] Voltaje: %4lu mV  |  R_NTC: %8.1f Ω  |  Temp: %.2f °C\n",
                   (unsigned long)v_mv, r_ntc, temperatura);
        } else {
            printf("[NTC] Voltaje: %4lu mV  |  ⚠ Fuera de rango — revisa el circuito\n",
                   (unsigned long)v_mv);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Limpieza (inalcanzable en este programa, pero buena práctica) */
    adc_oneshot_del_unit(adc_handle);
    if (calibrado) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(cali_handle);
#endif
    }
}