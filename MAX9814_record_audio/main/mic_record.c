// mic_record.c
// ---------------------------------------------------------------------------
// PRUEBA DE VERIFICACIÓN — cadena de entrada del micrófono (MAX9814 + ADC).
//
// Graba ~RECORD_SECONDS de audio del micrófono analógico (MAX9814 -> ADC1) y
// lo transmite en streaming por UART0 (el MISMO USB de programación) como PCM
// int16 mono. Un script en el PC (host/save_audio.py) recibe el stream, lo
// guarda como .wav en ~/Downloads y permite escucharlo. Si el audio grabado
// suena bien, la etapa de captura (mic -> ADC -> Q15) está validada de forma
// independiente del FPGA.
//
// Se usa UART0 para que baste con un solo cable USB, al MISMO baudios que la
// consola (115200): cambiar la velocidad de UART0 en caliente no es fiable. Se
// silencian los logs durante el envío para no corromper el PCM binario; el host
// abre el puerto a 115200 y busca el magic "ESPMIC01" descartando los logs de
// arranque. A 115200, 10 s de audio tardan ~28 s en transferirse (es una
// prueba puntual, no streaming en tiempo real).
//
// Protocolo (todo little-endian):
//   [8]  magic  = "ESPMIC01"
//   [4]  uint32 sample_rate_hz
//   [4]  uint32 num_samples         (total de muestras int16 que siguen)
//   [2]  uint16 bits_per_sample     (= 16)
//   [2]  uint16 channels            (= 1)
//   [num_samples * 2] int16 PCM little-endian
//
// El ESP32 clásico no puede escribir directamente en la carpeta Descargas del
// PC, por eso el WAV lo genera el script del host a partir de este stream.
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_continuous.h"
#include "driver/uart.h"
#include "soc/soc_caps.h"

static const char *TAG = "mic_record";

/* ---- Parámetros de grabación ---- */
#define SAMPLE_RATE_HZ   20000          /* 20 kHz: mínimo del ADC-DMA del ESP32
                                         * (SOC_ADC_SAMPLE_FREQ_THRES_LOW); por
                                         * debajo, adc_continuous_config da
                                         * ESP_ERR_INVALID_ARG. Voz nítida. */
#define RECORD_SECONDS   10
#define NUM_SAMPLES      ((uint32_t)SAMPLE_RATE_HZ * RECORD_SECONDS)

/* ---- ADC (MAX9814 OUT -> GPIO36 = ADC1_CH0) ---- */
#define ADC_CHANNEL      ADC_CHANNEL_0
#define ADC_ATTEN        ADC_ATTEN_DB_12
#define ADC_BITWIDTH     ADC_BITWIDTH_12

/* ---- UART de salida hacia el PC (UART0 = consola/USB de programación) ----
 * Mismo cable que se usa para flashear, al mismo baudios que la consola para
 * no tener que cambiar la velocidad en caliente. Pines por defecto (GPIO1/3). */
#define UART_PORT        UART_NUM_0
#define UART_BAUD        115200

#define READ_LEN         512            /* bytes por adc_continuous_read (múltiplo de 4) */

static adc_continuous_handle_t s_adc = NULL;

static void adc_start(void)
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 4096,
        .conv_frame_size    = 256,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &s_adc));

    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN,
        .channel   = ADC_CHANNEL,
        .unit      = ADC_UNIT_1,
        .bit_width = ADC_BITWIDTH,
    };
    adc_continuous_config_t dig_cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = SAMPLE_RATE_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    ESP_ERROR_CHECK(adc_continuous_config(s_adc, &dig_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(s_adc));
}

static void uart_start(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 1024, 8192, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    /* UART0: se conservan los pines de consola por defecto (GPIO1/GPIO3). */
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void put_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

static void send_header(void)
{
    uint8_t hdr[20];
    memcpy(hdr, "ESPMIC01", 8);
    put_u32(hdr + 8,  SAMPLE_RATE_HZ);
    put_u32(hdr + 12, NUM_SAMPLES);
    put_u16(hdr + 16, 16);   /* bits/sample */
    put_u16(hdr + 18, 1);    /* channels    */
    uart_write_bytes(UART_PORT, (const char *)hdr, sizeof(hdr));
}

void app_main(void)
{
    adc_start();

    /* Banner legible (consola a 115200) por si se mira con idf.py monitor. */
    ESP_LOGI(TAG, "Grabando %d s @ %d Hz (%u muestras). Volcado por UART0 a %d baud.",
             RECORD_SECONDS, SAMPLE_RATE_HZ, (unsigned)NUM_SAMPLES, UART_BAUD);
    ESP_LOGI(TAG, "En el PC:  python3 host/save_audio.py -r   (~35 s de transferencia)");

    /* Deja salir los logs antes de pasar el puerto a binario. */
    vTaskDelay(pdMS_TO_TICKS(300));

    /* A partir de aquí el puerto es binario: silenciamos TODO log para que
     * ningún texto se cuele en medio del PCM y lo corrompa. */
    esp_log_level_set("*", ESP_LOG_NONE);

    /* Instala el driver en UART0 (mismo baudios que la consola) para poder
     * escribir bytes binarios con uart_write_bytes. */
    uart_start();

    /* Respiro para que el ADC entregue muestras estables. */
    vTaskDelay(pdMS_TO_TICKS(300));

    send_header();

    uint8_t  raw[READ_LEN];
    int16_t  pcm[READ_LEN / SOC_ADC_DIGI_RESULT_BYTES];
    uint32_t sent = 0;

    /* Bloqueador de DC (filtro paso-altos de 1 polo) para quitar el offset del
     * MAX9814 y que el audio se escuche centrado, no saturado hacia un lado. */
    int32_t dc = 2048 << 8;  /* estimación de la media en punto fijo (<<8) */

    while (sent < NUM_SAMPLES) {
        uint32_t got = 0;
        esp_err_t err = adc_continuous_read(s_adc, raw, READ_LEN, &got, 1000);
        if (err != ESP_OK) {
            continue;  /* timeout: reintenta */
        }

        int n = 0;
        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got;
             i += SOC_ADC_DIGI_RESULT_BYTES) {
            adc_digi_output_data_t *d = (adc_digi_output_data_t *)&raw[i];
            if (d->type1.channel != ADC_CHANNEL) continue;

            int32_t x = (int32_t)(d->type1.data & 0x0FFF);   /* 0..4095 */
            dc += (((x << 8) - dc) >> 9);                    /* media lenta */
            int32_t s = x - (dc >> 8);                       /* sin DC: ~ -2048..2047 */

            int32_t v = s * 16;                              /* a rango int16 */
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            pcm[n++] = (int16_t)v;

            if (sent + n >= NUM_SAMPLES) break;              /* no pasarse del total */
        }

        if (n > 0) {
            uart_write_bytes(UART_PORT, (const char *)pcm, (size_t)n * 2);
            sent += n;
        }
    }

    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(2000));
    adc_continuous_stop(s_adc);
    ESP_LOGI(TAG, "Listo: %u muestras enviadas. Revisa ~/Downloads en el PC.", (unsigned)sent);

    /* Fin de la prueba: no repetir para no pisar el WAV ya guardado. */
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
