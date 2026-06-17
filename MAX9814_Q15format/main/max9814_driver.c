#include "max9814_driver.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_continuous.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "max9814";

/* Sampling and UART parameters */
#define SAMPLE_RATE_HZ        44100
#define BLOCK_SAMPLES         2048
#define UART_PORT             UART_NUM_2
#define UART_BAUDRATE         921600

/* Pins - adjust if needed */
#define TXD_PIN               17
#define RXD_PIN               -1

/* Framing bytes */
#define FRAME_START_0         0xAA
#define FRAME_START_1         0x55
#define FRAME_END_0           0x55
#define FRAME_END_1           0xAA

static adc_continuous_handle_t s_adc_handle = NULL;

esp_err_t max9814_init(void)
{
	esp_err_t err;

	/* Configure adc_continuous handle.
	 * Buffer dimensionado para absorber jitter de la tarea de TX: con
	 * 32768 bytes guardamos ~8 frames (~370 ms a 44.1 kHz) en lugar de los
	 * ~93 ms (2 bloques) originales, que se desbordaban en 1-2 s.
	 * flush_pool=1: si llega a desbordar, descarta lo viejo y sigue en vez
	 * de quedarse atascado devolviendo errores. */
	adc_continuous_handle_cfg_t handle_cfg = {
		.max_store_buf_size = 32768,
		.conv_frame_size = 2048,
		.flags = { .flush_pool = 1 },
	};

	err = adc_continuous_new_handle(&handle_cfg, &s_adc_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "adc_continuous_new_handle failed: %s", esp_err_to_name(err));
		return err;
	}

	/* Configure pattern for ADC1 channel 0 (GPIO36) */
	adc_digi_pattern_config_t pattern[1];
	pattern[0].atten = ADC_ATTEN_DB_12;
	pattern[0].channel = ADC_CHANNEL_0;
	pattern[0].unit = ADC_UNIT_1;
	pattern[0].bit_width = ADC_BITWIDTH_12;

	adc_continuous_config_t cont_cfg = {
		.pattern_num = 1,
		.adc_pattern = pattern,
		.sample_freq_hz = SAMPLE_RATE_HZ,
		.conv_mode = ADC_CONV_SINGLE_UNIT_1,
		.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
	};

	err = adc_continuous_config(s_adc_handle, &cont_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "adc_continuous_config failed: %s", esp_err_to_name(err));
		adc_continuous_deinit(s_adc_handle);
		s_adc_handle = NULL;
		return err;
	}

	err = adc_continuous_start(s_adc_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "adc_continuous_start failed: %s", esp_err_to_name(err));
		adc_continuous_deinit(s_adc_handle);
		s_adc_handle = NULL;
		return err;
	}

	/* Configure UART */
	uart_config_t uart_config = {
		.baud_rate = UART_BAUDRATE,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_APB,
	};

	err = uart_param_config(UART_PORT, &uart_config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
		return err;
	}

	err = uart_set_pin(UART_PORT, TXD_PIN, RXD_PIN, -1, -1);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
		return err;
	}

	err = uart_driver_install(UART_PORT, 4096, 0, 0, NULL, 0);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGI(TAG, "ADC continuous initialized at %d Hz, UART %d bps", SAMPLE_RATE_HZ, UART_BAUDRATE);

	/* Quick speed test: send one synthetic block and measure time on TX line */
	{
		ESP_LOGI(TAG, "Starting quick UART speed test: sending one %d-sample block...", BLOCK_SAMPLES);
		size_t payload_bytes = BLOCK_SAMPLES * 2;
		uint8_t *test_payload = (uint8_t*)heap_caps_malloc(payload_bytes, MALLOC_CAP_8BIT);
		if (test_payload) {
			for (size_t i = 0; i < payload_bytes; ++i) test_payload[i] = (uint8_t)(i & 0xFF);

			uint8_t header[4] = { FRAME_START_0, FRAME_START_1, (uint8_t)((BLOCK_SAMPLES>>8)&0xFF), (uint8_t)(BLOCK_SAMPLES&0xFF) };
			uint8_t tail[2] = { FRAME_END_0, FRAME_END_1 };

			int64_t t0 = esp_timer_get_time();
			ssize_t hlen = uart_write_bytes(UART_PORT, (const char*)header, sizeof(header));
			ssize_t plen = uart_write_bytes(UART_PORT, (const char*)test_payload, payload_bytes);
			ssize_t tlen = uart_write_bytes(UART_PORT, (const char*)tail, sizeof(tail));
			uart_wait_tx_done(UART_PORT, 2000 / portTICK_PERIOD_MS);
			int64_t t1 = esp_timer_get_time();

			if (hlen < 0 || plen < 0 || tlen < 0) {
				ESP_LOGE(TAG, "uart_write_bytes returned error during test");
			} else {
				int64_t dt_us = t1 - t0;
				int64_t bytes_sent = (ssize_t)sizeof(header) + plen + (ssize_t)sizeof(tail);
				double bps = (double)bytes_sent * 8.0 * 1e6 / (double)dt_us;
				ESP_LOGI(TAG, "Test send: %lld bytes in %lld us => %.0f bps (%.1f kbps)", bytes_sent, dt_us, bps, bps/1000.0);
			}

			heap_caps_free(test_payload);
		} else {
			ESP_LOGW(TAG, "Could not allocate test payload for speed test");
		}
	}

	return ESP_OK;
}

void max9814_task(void *arg)
{
	(void)arg;

	/* Todos los buffers se reservan UNA sola vez, fuera del lazo caliente.
	 * Antes se hacía malloc/free de ~8 KB (parsed) en cada vuelta (cada
	 * ~46 ms), lo que añadía latencia y fragmentaba el heap; un fallo de
	 * malloc dejaba la tarea sin transmitir. */
	const size_t payload_bytes = BLOCK_SAMPLES * 2;
	uint8_t *payload = (uint8_t*)heap_caps_malloc(payload_bytes, MALLOC_CAP_8BIT);
	adc_continuous_data_t *parsed = (adc_continuous_data_t*)heap_caps_malloc(sizeof(adc_continuous_data_t) * BLOCK_SAMPLES, MALLOC_CAP_8BIT);
	if (!payload || !parsed) {
		ESP_LOGE(TAG, "Failed to allocate task buffers");
		heap_caps_free(payload);
		heap_caps_free(parsed);
		vTaskDelete(NULL);
		return;
	}

	const uint8_t header[4] = {
		FRAME_START_0,
		FRAME_START_1,
		(uint8_t)((BLOCK_SAMPLES >> 8) & 0xFF),
		(uint8_t)(BLOCK_SAMPLES & 0xFF),
	};
	const uint8_t tail[2] = { FRAME_END_0, FRAME_END_1 };

	/* Telemetría limitada a 1 de cada LOG_EVERY bloques: el ESP_LOGI por
	 * bloque salía bloqueando por la consola (~5 ms) y rompía el tiempo
	 * real (presupuesto por bloque ~46 ms, UART ya consume ~44.5 ms),
	 * provocando el desborde del ADC y la congelación de la gráfica. */
	const uint32_t LOG_EVERY = 100;
	uint32_t block_count = 0;

	while (1) {
		uint32_t num_samples = 0;
		esp_err_t err = adc_continuous_read_parse(s_adc_handle, parsed, BLOCK_SAMPLES, &num_samples, 1000);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "adc_continuous_read_parse failed: %s", esp_err_to_name(err));
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}

		if (num_samples < BLOCK_SAMPLES) {
			/* not enough samples yet */
			continue;
		}

		for (size_t i = 0; i < BLOCK_SAMPLES; ++i) {
			uint32_t adc12 = parsed[i].raw_data & 0x0FFF;
			int32_t signed_val = (int32_t)adc12 - 2048;
			int32_t q15 = (signed_val * 32767) / 2048;
			if (q15 > 32767) q15 = 32767;
			if (q15 < -32768) q15 = -32768;
			int16_t sample = (int16_t)q15;

			payload[2*i + 0] = (uint8_t)((sample >> 8) & 0xFF);
			payload[2*i + 1] = (uint8_t)(sample & 0xFF);
		}

		ssize_t hlen = uart_write_bytes(UART_PORT, (const char*)header, sizeof(header));
		ssize_t plen = uart_write_bytes(UART_PORT, (const char*)payload, payload_bytes);
		ssize_t tlen = uart_write_bytes(UART_PORT, (const char*)tail, sizeof(tail));
		uart_wait_tx_done(UART_PORT, 2000 / portTICK_PERIOD_MS);

		if (hlen < 0 || plen < 0 || tlen < 0) {
			ESP_LOGE(TAG, "uart_write_bytes error during block send");
		} else if ((++block_count % LOG_EVERY) == 0) {
			ESP_LOGI(TAG, "TX alive: %lu blocks sent", (unsigned long)block_count);
		}
	}

	heap_caps_free(parsed);
	heap_caps_free(payload);
	vTaskDelete(NULL);
}

