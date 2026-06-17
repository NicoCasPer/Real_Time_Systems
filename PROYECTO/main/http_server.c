/*
 * http_server.c
 *
 *  Created on: Oct 20, 2021
 *      Author: kjagu
 */

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "sys/param.h"
#include <stdlib.h>

#include "http_server.h"
#include "environment_control.h"
#include "tasks_common.h"
#include "wifi_app.h"
#include "cJSON.h"
#include "driver/gpio.h"



#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "esp_sntp.h"


// Tag used for ESP serial console messages
static const char TAG[] = "http_server";


// Wifi connect status
static int g_wifi_connect_status = NONE;

// Firmware update status
static int g_fw_update_status = OTA_UPDATE_PENDING;

// HTTP server task handle
static httpd_handle_t http_server_handle = NULL;

// HTTP server monitor task handle
static TaskHandle_t task_http_server_monitor = NULL;

// Queue handle used to manipulate the main queue of events
static QueueHandle_t http_server_monitor_queue_handle;

/**
 * ESP32 timer configuration passed to esp_timer_create.
 */
const esp_timer_create_args_t fw_update_reset_args = {
		.callback = &http_server_fw_update_reset_callback,
		.arg = NULL,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "fw_update_reset"
};
esp_timer_handle_t fw_update_reset;

// Embedded files: JQuery, index.html, app.css, app.js and favicon.ico files
extern const uint8_t jquery_3_3_1_min_js_start[]	asm("_binary_jquery_3_3_1_min_js_start");
extern const uint8_t jquery_3_3_1_min_js_end[]		asm("_binary_jquery_3_3_1_min_js_end");
extern const uint8_t index_html_start[]				asm("_binary_index_html_start");
extern const uint8_t index_html_end[]				asm("_binary_index_html_end");
extern const uint8_t app_css_start[]				asm("_binary_app_css_start");
extern const uint8_t app_css_end[]					asm("_binary_app_css_end");
extern const uint8_t app_js_start[]					asm("_binary_app_js_start");
extern const uint8_t app_js_end[]					asm("_binary_app_js_end");
extern const uint8_t favicon_ico_start[]			asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]				asm("_binary_favicon_ico_end");

uint8_t s_led_state = 0;







void toogle_led( void )
{
	

	s_led_state = !s_led_state;
	gpio_set_level(BLINK_GPIO, s_led_state);

}

static cJSON *http_server_parse_json_body(httpd_req_t *req)
{
	if (req->content_len <= 0 || req->content_len > 2048)
	{
		return NULL;
	}

	char *buffer = (char *)calloc(req->content_len + 1, 1);
	if (buffer == NULL)
	{
		return NULL;
	}

	int received = 0;
	while (received < req->content_len)
	{
		int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
		if (ret <= 0)
		{
			free(buffer);
			return NULL;
		}
		received += ret;
	}

	cJSON *root = cJSON_Parse(buffer);
	free(buffer);
	return root;
}

static esp_err_t http_server_send_json(httpd_req_t *req, cJSON *root)
{
	char *payload = cJSON_PrintUnformatted(root);
	if (payload == NULL)
	{
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON encode failed");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, payload, strlen(payload));
	free(payload);
	return ESP_OK;
}

static esp_err_t http_server_send_ok(httpd_req_t *req)
{
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, "{\"ok\":true}");
	return ESP_OK;
}

static esp_err_t http_server_send_bad_request(httpd_req_t *req, const char *message)
{
	httpd_resp_set_status(req, "400 Bad Request");
	httpd_resp_set_type(req, "application/json");

	char response[128];
	snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", message);
	httpd_resp_sendstr(req, response);
	return ESP_FAIL;
}

static esp_err_t http_server_env_state_handler(httpd_req_t *req)
{
	env_control_state_t state;
	env_control_get_state(&state);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "temperature", state.temperature_c);
	cJSON_AddNumberToObject(root, "desiredTemp", state.desired_temp_c);
	cJSON_AddNumberToObject(root, "maxTemp", state.max_temp_c);
	cJSON_AddNumberToObject(root, "fanSpeed", state.fan_speed_percent);
	cJSON_AddNumberToObject(root, "manualFanSpeed", state.manual_fan_speed_percent);
	cJSON_AddStringToObject(root, "fanMode", state.fan_mode == ENV_FAN_MODE_AUTO ? "auto" : "manual");
	cJSON_AddNumberToObject(root, "curtainOpening", state.curtain_opening_percent);
	cJSON_AddStringToObject(root, "curtainMode", state.curtain_mode == ENV_CURTAIN_MODE_SCHEDULE ? "schedule" : "manual");
	cJSON_AddNumberToObject(root, "rgbRed", state.rgb_red);
	cJSON_AddNumberToObject(root, "rgbGreen", state.rgb_green);
	cJSON_AddNumberToObject(root, "rgbBlue", state.rgb_blue);
	cJSON_AddNumberToObject(root, "rgbBrightness", state.rgb_brightness_percent);
	cJSON_AddBoolToObject(root, "alarmActive", state.alarm_active);

	cJSON *schedule = cJSON_AddArrayToObject(root, "schedule");
	for (int i = 0; i < ENV_SCHEDULE_COUNT; i++)
	{
		cJSON *record = cJSON_CreateObject();
		cJSON_AddNumberToObject(record, "record", i + 1);
		cJSON_AddBoolToObject(record, "enabled", state.schedule[i].enabled);
		cJSON_AddNumberToObject(record, "hour", state.schedule[i].hour);
		cJSON_AddNumberToObject(record, "minute", state.schedule[i].minute);
		cJSON_AddNumberToObject(record, "opening", state.schedule[i].opening_percent);
		cJSON_AddItemToArray(schedule, record);
	}

	esp_err_t ret = http_server_send_json(req, root);
	cJSON_Delete(root);
	return ret;
}

static esp_err_t http_server_curtain_handler(httpd_req_t *req)
{
	cJSON *root = http_server_parse_json_body(req);
	if (root == NULL)
	{
		return http_server_send_bad_request(req, "Invalid JSON");
	}

	cJSON *mode = cJSON_GetObjectItem(root, "mode");
	cJSON *opening = cJSON_GetObjectItem(root, "opening");
	esp_err_t ret = ESP_OK;

	if (mode != NULL && cJSON_IsString(mode) && strcmp(mode->valuestring, "schedule") == 0)
	{
		ret = env_control_set_curtain_mode(ENV_CURTAIN_MODE_SCHEDULE);
	}
	else if (opening != NULL && cJSON_IsNumber(opening))
	{
		ret = env_control_set_curtain_manual((uint8_t)opening->valueint);
	}
	else if (mode != NULL && cJSON_IsString(mode) && strcmp(mode->valuestring, "manual") == 0)
	{
		ret = env_control_set_curtain_mode(ENV_CURTAIN_MODE_MANUAL);
	}
	else
	{
		ret = ESP_ERR_INVALID_ARG;
	}

	cJSON_Delete(root);
	return ret == ESP_OK ? http_server_send_ok(req) : http_server_send_bad_request(req, "Invalid curtain parameters");
}

static esp_err_t http_server_schedule_handler(httpd_req_t *req)
{
	cJSON *root = http_server_parse_json_body(req);
	if (root == NULL)
	{
		return http_server_send_bad_request(req, "Invalid JSON");
	}

	cJSON *record = cJSON_GetObjectItem(root, "record");
	cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
	cJSON *hour = cJSON_GetObjectItem(root, "hour");
	cJSON *minute = cJSON_GetObjectItem(root, "minute");
	cJSON *opening = cJSON_GetObjectItem(root, "opening");

	if (!cJSON_IsNumber(record) || !cJSON_IsBool(enabled) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) || !cJSON_IsNumber(opening))
	{
		cJSON_Delete(root);
		return http_server_send_bad_request(req, "Missing schedule parameters");
	}

	esp_err_t ret = env_control_set_schedule_record((uint8_t)(record->valueint - 1), cJSON_IsTrue(enabled), (uint8_t)hour->valueint, (uint8_t)minute->valueint, (uint8_t)opening->valueint);
	cJSON_Delete(root);
	return ret == ESP_OK ? http_server_send_ok(req) : http_server_send_bad_request(req, "Invalid schedule values");
}

static esp_err_t http_server_fan_handler(httpd_req_t *req)
{
	cJSON *root = http_server_parse_json_body(req);
	if (root == NULL)
	{
		return http_server_send_bad_request(req, "Invalid JSON");
	}

	cJSON *mode = cJSON_GetObjectItem(root, "mode");
	esp_err_t ret = ESP_ERR_INVALID_ARG;

	if (cJSON_IsString(mode) && strcmp(mode->valuestring, "manual") == 0)
	{
		cJSON *speed = cJSON_GetObjectItem(root, "speed");
		if (cJSON_IsNumber(speed))
		{
			ret = env_control_set_fan_manual((uint8_t)speed->valueint);

			// En modo manual tambien permitimos fijar los umbrales de
			// temperatura (la maxima define el disparo de la alarma).
			cJSON *desired = cJSON_GetObjectItem(root, "desiredTemp");
			cJSON *max = cJSON_GetObjectItem(root, "maxTemp");
			if (ret == ESP_OK && cJSON_IsNumber(desired) && cJSON_IsNumber(max))
			{
				ret = env_control_set_temperatures((float)desired->valuedouble, (float)max->valuedouble);
			}
		}
	}
	else if (cJSON_IsString(mode) && strcmp(mode->valuestring, "auto") == 0)
	{
		cJSON *desired = cJSON_GetObjectItem(root, "desiredTemp");
		cJSON *max = cJSON_GetObjectItem(root, "maxTemp");
		if (cJSON_IsNumber(desired) && cJSON_IsNumber(max))
		{
			ret = env_control_set_fan_auto((float)desired->valuedouble, (float)max->valuedouble);
		}
		else
		{
			ret = env_control_set_fan_mode(ENV_FAN_MODE_AUTO);
		}
	}

	cJSON_Delete(root);
	return ret == ESP_OK ? http_server_send_ok(req) : http_server_send_bad_request(req, "Invalid fan parameters");
}

static esp_err_t http_server_rgb_handler(httpd_req_t *req)
{
	cJSON *root = http_server_parse_json_body(req);
	if (root == NULL)
	{
		return http_server_send_bad_request(req, "Invalid JSON");
	}

	cJSON *red = cJSON_GetObjectItem(root, "red");
	cJSON *green = cJSON_GetObjectItem(root, "green");
	cJSON *blue = cJSON_GetObjectItem(root, "blue");
	cJSON *brightness = cJSON_GetObjectItem(root, "brightness");

	if (!cJSON_IsNumber(red) || !cJSON_IsNumber(green) || !cJSON_IsNumber(blue) || !cJSON_IsNumber(brightness))
	{
		cJSON_Delete(root);
		return http_server_send_bad_request(req, "Missing RGB parameters");
	}

	esp_err_t ret = env_control_set_rgb((uint8_t)red->valueint, (uint8_t)green->valueint, (uint8_t)blue->valueint, (uint8_t)brightness->valueint);
	cJSON_Delete(root);
	return ret == ESP_OK ? http_server_send_ok(req) : http_server_send_bad_request(req, "Invalid RGB values");
}

static esp_err_t http_server_ap_config_handler(httpd_req_t *req)
{
	cJSON *root = http_server_parse_json_body(req);
	if (root == NULL)
	{
		return http_server_send_bad_request(req, "Invalid JSON");
	}

	cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
	cJSON *password = cJSON_GetObjectItem(root, "password");

	if (!cJSON_IsString(ssid) || !cJSON_IsString(password))
	{
		cJSON_Delete(root);
		return http_server_send_bad_request(req, "Missing AP credentials");
	}

	esp_err_t ret = wifi_app_set_ap_credentials(ssid->valuestring, password->valuestring);
	cJSON_Delete(root);
	return ret == ESP_OK ? http_server_send_ok(req) : http_server_send_bad_request(req, "Invalid AP credentials");
}

static esp_err_t http_server_get_dht_sensor_readings_json_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "/dhtSensor.json requested");

	char dhtSensorJSON[100];
	env_control_state_t state;
	env_control_get_state(&state);

	sprintf(dhtSensorJSON, "{\"temp\":\"%.1f\",\"humidity\":\"0.0\"}", state.temperature_c);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, dhtSensorJSON, strlen(dhtSensorJSON));

	return ESP_OK;
}

static esp_err_t http_server_toogle_led_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "/toogle_led.json requested");

	toogle_led();

	// Cerrar la conexion
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    
	return ESP_OK;
}

static esp_err_t http_server_read_register_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "/readreg.json requested");

	char register_information_read_1[12];
	register_information_read_1[11] = 0x00;
	if (read_reg_data( &register_information_read_1[0], 1 ) != ESP_OK ){
		memset(&register_information_read_1[0], '9', 6);
	}

	char register_information_read_2[12];
	register_information_read_2[11] = 0x00;
	if (read_reg_data( &register_information_read_2[0], 2 ) != ESP_OK ){
		memset(&register_information_read_2[0], '9', 6);
	}


	char register_information_read_3[12];
	register_information_read_3[11] = 0x00;
	if (read_reg_data( &register_information_read_3[0], 3 ) != ESP_OK ){
		memset(&register_information_read_3[0], '9', 6);
	}


	char register_information_read_4[12];
	register_information_read_4[11] = 0x00;
	if (read_reg_data( &register_information_read_4[0], 4 ) != ESP_OK ){
		memset(&register_information_read_4[0], '9', 6);
	}

	char register_information_read_5[12];
	register_information_read_5[11] = 0x00;
	if (read_reg_data( &register_information_read_5[0], 5 ) != ESP_OK ){
		memset(&register_information_read_5[0], '9', 6);
	}


	char register_information_read_6[12];
	register_information_read_6[11] = 0x00;
	if (read_reg_data( &register_information_read_6[0], 6 ) != ESP_OK ){
		memset(&register_information_read_6[0], '9', 6);
	}

	char register_information_read_7[12];
	register_information_read_7[11] = 0x00;
	if (read_reg_data( &register_information_read_7[0], 7 ) != ESP_OK ){
		memset(&register_information_read_7[0], '9', 6);
	}


	char register_information_read_8[12];
	register_information_read_8[11] = 0x00;
	if (read_reg_data( &register_information_read_8[0], 8 ) != ESP_OK ){
		memset(&register_information_read_8[0], '9', 6);
	}


	char register_information_read_9[12];
	register_information_read_9[11] = 0x00;
	if (read_reg_data( &register_information_read_9[0], 9 ) != ESP_OK ){
		memset(&register_information_read_9[0], '9', 6);
	}

	char register_information_read_10[12];
	register_information_read_10[11] = 0x00;
	if (read_reg_data( &register_information_read_10[0], 10 ) != ESP_OK ){
		memset(&register_information_read_10[0], '9', 6);
	}




	//sprintf(read_regs, "{\"reg1\":\"%s\",\"reg2":\"%s\",\"reg3\":\"%s\",\"reg4\":\"%s\",\"reg5\":\"%s\",\"reg6\":\"%s\",\"reg7\":\"%s\",\"reg8\":\"%s\",\"reg9\":\"%s\",\"reg10\":\"%s\"}", 
	//	register_information_read_1, register_information_read_2, register_information_read_3, register_information_read_4, register_information_read_5, register_information_read_6,register_information_read_7, register_information_read_8,
	//	register_information_read_9, register_information_read_10);

	//httpd_resp_set_type(req, "application/json");
	//httpd_resp_send(req, read_regs, strlen(read_regs));

	return ESP_OK;


}



/**
 * Checks the g_fw_update_status and creates the fw_update_reset timer if g_fw_update_status is true.
 */
static void http_server_fw_update_reset_timer(void)
{
	if (g_fw_update_status == OTA_UPDATE_SUCCESSFUL)
	{
		ESP_LOGI(TAG, "http_server_fw_update_reset_timer: FW updated successful starting FW update reset timer");

		// Give the web page a chance to receive an acknowledge back and initialize the timer
		ESP_ERROR_CHECK(esp_timer_create(&fw_update_reset_args, &fw_update_reset));
		ESP_ERROR_CHECK(esp_timer_start_once(fw_update_reset, 8000000));
	}
	else
	{
		ESP_LOGI(TAG, "http_server_fw_update_reset_timer: FW update unsuccessful");
	}
}

/**
 * HTTP server monitor task used to track events of the HTTP server
 * @param pvParameters parameter which can be passed to the task.
 */
static void http_server_monitor(void *parameter)
{
	http_server_queue_message_t msg;

	for (;;)
	{
		if (xQueueReceive(http_server_monitor_queue_handle, &msg, portMAX_DELAY))
		{
			switch (msg.msgID)
			{
				case HTTP_MSG_WIFI_CONNECT_INIT:
					ESP_LOGI(TAG, "HTTP_MSG_WIFI_CONNECT_INIT");
					
					g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECTING;

					break;

				case HTTP_MSG_WIFI_CONNECT_SUCCESS:
					ESP_LOGI(TAG, "HTTP_MSG_WIFI_CONNECT_SUCCESS");

					g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECT_SUCCESS;
					//mqtt_app_start(); dissable MQTT
					break;

				case HTTP_MSG_WIFI_CONNECT_FAIL:
					ESP_LOGI(TAG, "HTTP_MSG_WIFI_CONNECT_FAIL");

					g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECT_FAILED;

					break;

				case HTTP_MSG_OTA_UPDATE_SUCCESSFUL:
					ESP_LOGI(TAG, "HTTP_MSG_OTA_UPDATE_SUCCESSFUL");
					g_fw_update_status = OTA_UPDATE_SUCCESSFUL;
					http_server_fw_update_reset_timer();

					break;

				case HTTP_MSG_OTA_UPDATE_FAILED:
					ESP_LOGI(TAG, "HTTP_MSG_OTA_UPDATE_FAILED");
					g_fw_update_status = OTA_UPDATE_FAILED;

					break;

				default:
					break;
			}
		}
	}
}

/**
 * Jquery get handler is requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_jquery_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "Jquery requested");

	httpd_resp_set_type(req, "application/javascript");
	httpd_resp_send(req, (const char *)jquery_3_3_1_min_js_start, jquery_3_3_1_min_js_end - jquery_3_3_1_min_js_start);

	return ESP_OK;
}

/**
 * Sends the index.html page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_index_html_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "index.html requested");

	httpd_resp_set_type(req, "text/html");
	httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);

	return ESP_OK;
}

/**
 * app.css get handler is requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_app_css_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "app.css requested");

	httpd_resp_set_type(req, "text/css");
	httpd_resp_send(req, (const char *)app_css_start, app_css_end - app_css_start);

	return ESP_OK;
}

/**
 * app.js get handler is requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_app_js_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "app.js requested");

	httpd_resp_set_type(req, "application/javascript");
	httpd_resp_send(req, (const char *)app_js_start, app_js_end - app_js_start);

	return ESP_OK;
}

/**
 * Sends the .ico (icon) file when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_favicon_ico_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "favicon.ico requested");

	httpd_resp_set_type(req, "image/x-icon");
	httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_end - favicon_ico_start);

	return ESP_OK;
}

/**
 * Receives the .bin file fia the web page and handles the firmware update
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK, otherwise ESP_FAIL if timeout occurs and the update cannot be started.
 */
esp_err_t http_server_OTA_update_handler(httpd_req_t *req)
{
	esp_ota_handle_t ota_handle;

	char ota_buff[1024];
	int content_length = req->content_len;
	int content_received = 0;
	int recv_len;
	bool is_req_body_started = false;
	bool flash_successful = false;

	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

	do
	{
		// Read the data for the request
		if ((recv_len = httpd_req_recv(req, ota_buff, MIN(content_length, sizeof(ota_buff)))) < 0)
		{
			// Check if timeout occurred
			if (recv_len == HTTPD_SOCK_ERR_TIMEOUT)
			{
				ESP_LOGI(TAG, "http_server_OTA_update_handler: Socket Timeout");
				continue; ///> Retry receiving if timeout occurred
			}
			ESP_LOGI(TAG, "http_server_OTA_update_handler: OTA other Error %d", recv_len);
			return ESP_FAIL;
		}
		printf("http_server_OTA_update_handler: OTA RX: %d of %d\r", content_received, content_length);

		// Is this the first data we are receiving
		// If so, it will have the information in the header that we need.
		if (!is_req_body_started)
		{
			is_req_body_started = true;

			// Get the location of the .bin file content (remove the web form data)
			char *body_start_p = strstr(ota_buff, "\r\n\r\n") + 4;
			int body_part_len = recv_len - (body_start_p - ota_buff);

			printf("http_server_OTA_update_handler: OTA file size: %d\r\n", content_length);

			esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
			if (err != ESP_OK)
			{
				printf("http_server_OTA_update_handler: Error with OTA begin, cancelling OTA\r\n");
				return ESP_FAIL;
			}
			else
			{
				printf("http_server_OTA_update_handler: Writing to partition subtype %d at offset 0x%lx\r\n", update_partition->subtype, update_partition->address);
			}

			// Write this first part of the data
			esp_ota_write(ota_handle, body_start_p, body_part_len);
			content_received += body_part_len;
		}
		else
		{
			// Write OTA data
			esp_ota_write(ota_handle, ota_buff, recv_len);
			content_received += recv_len;
		}

	} while (recv_len > 0 && content_received < content_length);

	if (esp_ota_end(ota_handle) == ESP_OK)
	{
		// Lets update the partition
		if (esp_ota_set_boot_partition(update_partition) == ESP_OK)
		{
			const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
			ESP_LOGI(TAG, "http_server_OTA_update_handler: Next boot partition subtype %d at offset 0x%lx", boot_partition->subtype, boot_partition->address);
			flash_successful = true;
		}
		else
		{
			ESP_LOGI(TAG, "http_server_OTA_update_handler: FLASHED ERROR!!!");
		}
	}
	else
	{
		ESP_LOGI(TAG, "http_server_OTA_update_handler: esp_ota_end ERROR!!!");
	}

	// We won't update the global variables throughout the file, so send the message about the status
	if (flash_successful) { http_server_monitor_send_message(HTTP_MSG_OTA_UPDATE_SUCCESSFUL); } else { http_server_monitor_send_message(HTTP_MSG_OTA_UPDATE_FAILED); }

	return ESP_OK;
}

/**
 * OTA status handler responds with the firmware update status after the OTA update is started
 * and responds with the compile time/date when the page is first requested
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */
esp_err_t http_server_OTA_status_handler(httpd_req_t *req)
{
	char otaJSON[100];

	ESP_LOGI(TAG, "OTAstatus requested");

	sprintf(otaJSON, "{\"ota_update_status\":%d,\"compile_time\":\"%s\",\"compile_date\":\"%s\"}", g_fw_update_status, __TIME__, __DATE__);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, otaJSON, strlen(otaJSON));

	return ESP_OK;
}


/**
 * regchange.json handler is invoked after the "enviar registro" button is pressed
 * and handles receiving the data entered by the user
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */


static esp_err_t http_server_register_change_handler(httpd_req_t *req)
{
    size_t header_len;
    char* header_value;
    char* hour_str = NULL;
	char* reg_str = NULL;
    char* min_str = NULL;
    int content_length;

    ESP_LOGI(TAG, "/regchange.json requested");

    // Get the "Content-Length" header to determine the length of the request body
    header_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (header_len <= 0) {
        // Content-Length header not found or invalid
        //httpd_resp_send_err(req, HTTP_STATUS_411_LENGTH_REQUIRED, "Content-Length header is missing or invalid");
        ESP_LOGI(TAG, "Content-Length header is missing or invalid");
        return ESP_FAIL;
    }

    // Allocate memory to store the header value
    header_value = (char*)malloc(header_len + 1);
    if (httpd_req_get_hdr_value_str(req, "Content-Length", header_value, header_len + 1) != ESP_OK) {
        // Failed to get Content-Length header value
        free(header_value);
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Failed to get Content-Length header value");
        ESP_LOGI(TAG, "Failed to get Content-Length header value");
        return ESP_FAIL;
    }

    // Convert the Content-Length header value to an integer
    content_length = atoi(header_value);
    free(header_value);

    if (content_length <= 0) {
        // Content length is not a valid positive integer
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid Content-Length value");
        ESP_LOGI(TAG, "Invalid Content-Length value");
        return ESP_FAIL;
    }

    // Allocate memory for the data buffer based on the content length
    char* data_buffer = (char*)malloc(content_length + 1);

    // Read the request body into the data buffer
    if (httpd_req_recv(req, data_buffer, content_length) <= 0) {
        // Handle error while receiving data
        free(data_buffer);
        //httpd_resp_send_err(req, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to receive request body");
        ESP_LOGI(TAG, "Failed to receive request body");
        return ESP_FAIL;
    }

    // Null-terminate the data buffer to treat it as a string
    data_buffer[content_length] = '\0';

    // Parse the received JSON data
    cJSON* root = cJSON_Parse(data_buffer);
    free(data_buffer);

    if (root == NULL) {
        // JSON parsing error
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid JSON data");
        ESP_LOGI(TAG, "Invalid JSON data");
        return ESP_FAIL;
    }

	cJSON* reg_number_json = cJSON_GetObjectItem(root, "selectedNumber");
    cJSON* hour_json = cJSON_GetObjectItem(root, "hours");
    cJSON* min_json = cJSON_GetObjectItem(root, "minutes");
	cJSON* selectedDays_json = cJSON_GetObjectItem(root, "selectedDays");
	
    if (hour_json == NULL || min_json == NULL || selectedDays_json == NULL|| !cJSON_IsString(hour_json) || !cJSON_IsString(min_json) || !cJSON_IsArray(selectedDays_json)) {
        cJSON_Delete(root);
        // Missing or invalid JSON fields
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Missing or invalid JSON data fields");
        ESP_LOGI(TAG, "Missing or invalid JSON data fields");
        return ESP_FAIL;
    }

    // Extract SSID and password from JSON
	reg_str = strdup(reg_number_json->valuestring);
    hour_str = strdup(hour_json->valuestring);
    min_str = strdup(min_json->valuestring);

    

	ESP_LOGI(TAG, "Received reg: %s", reg_str);
    ESP_LOGI(TAG, "Received hour: %s", hour_str);
    ESP_LOGI(TAG, "Received min: %s", min_str);
 
    
	char str_to_save[12];
	//str_to_save[11]=0x00;
	memset(str_to_save, 0x00, 12);
	
	if (cJSON_IsArray(selectedDays_json)) {
    cJSON* day_item;

    // Iterate over each element in the array
	
	
	//strcat(str_to_save, reg_str);
	strcat(str_to_save, hour_str);
	strcat(str_to_save, min_str);
    cJSON_ArrayForEach(day_item, selectedDays_json) {
        // Check if the array element is a string
        if (cJSON_IsString(day_item)) {
            const char* day_str = day_item->valuestring;
			strcat(str_to_save, day_str);
            // Perform actions with the day_str
            printf("Selected Day: %s\n", day_str);
        }
    }
	
	} else {
		printf("SelectedDays is not an array\n");
	}
	printf("%s\n", str_to_save);
	save_reg_data(atoi(reg_str), &str_to_save[0]);
	update_register(atoi(reg_str));
	// Process the selected days array
   // cJSON* day_item;
    //cJSON_ArrayForEach(day_item, selectedDays_json) {
       // if (cJSON_IsString(day_item)) {
         //   const char* day_str = day_item->valuestring;
           // ESP_LOGI(TAG, "Selected Day: %s", day_str);

            // Perform any additional actions based on the selected day
            // ...

            // Release memory when no longer needed
        //}
		
    //}
	//free(day_item);

	//free(selectedDays_json);

	// Send a success response to the client
	// Cerrar la conexion
	free(reg_str);
	free(hour_str);
    free(min_str);
	cJSON_Delete(root);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
	

    return ESP_OK;
}




/**
 * erasereg.json handler is invoked after the "enviar registro" button is pressed
 * and handles receiving the data entered by the user
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */


static esp_err_t http_server_register_erase_handler(httpd_req_t *req)
{
    size_t header_len;
    char* header_value;
	char* reg_str = NULL;
    int content_length;

    ESP_LOGI(TAG, "/regerase.json requested");

    // Get the "Content-Length" header to determine the length of the request body
    header_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (header_len <= 0) {
        // Content-Length header not found or invalid
        //httpd_resp_send_err(req, HTTP_STATUS_411_LENGTH_REQUIRED, "Content-Length header is missing or invalid");
        ESP_LOGI(TAG, "Content-Length header is missing or invalid");
        return ESP_FAIL;
    }

    // Allocate memory to store the header value
    header_value = (char*)malloc(header_len + 1);
    if (httpd_req_get_hdr_value_str(req, "Content-Length", header_value, header_len + 1) != ESP_OK) {
        // Failed to get Content-Length header value
        free(header_value);
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Failed to get Content-Length header value");
        ESP_LOGI(TAG, "Failed to get Content-Length header value");
        return ESP_FAIL;
    }

    // Convert the Content-Length header value to an integer
    content_length = atoi(header_value);
    free(header_value);

    if (content_length <= 0) {
        // Content length is not a valid positive integer
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid Content-Length value");
        ESP_LOGI(TAG, "Invalid Content-Length value");
        return ESP_FAIL;
    }

    // Allocate memory for the data buffer based on the content length
    char* data_buffer = (char*)malloc(content_length + 1);

    // Read the request body into the data buffer
    if (httpd_req_recv(req, data_buffer, content_length) <= 0) {
        // Handle error while receiving data
        free(data_buffer);
        //httpd_resp_send_err(req, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to receive request body");
        ESP_LOGI(TAG, "Failed to receive request body");
        return ESP_FAIL;
    }

    // Null-terminate the data buffer to treat it as a string
    data_buffer[content_length] = '\0';

    // Parse the received JSON data
    cJSON* root = cJSON_Parse(data_buffer);
    free(data_buffer);

    if (root == NULL) {
        // JSON parsing error
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid JSON data");
        ESP_LOGI(TAG, "Invalid JSON data");
        return ESP_FAIL;
    }

	cJSON* reg_number_json = cJSON_GetObjectItem(root, "selectedNumber");
	
    if (reg_number_json == NULL || !cJSON_IsString(reg_number_json) ) {
        cJSON_Delete(root);
        // Missing or invalid JSON fields
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Missing or invalid JSON data fields");
        ESP_LOGI(TAG, "Missing or invalid JSON data fields");
        return ESP_FAIL;
    }


	reg_str = strdup(reg_number_json->valuestring);
    

	ESP_LOGI(TAG, "received reg: %s", reg_str);

 
    
	char str_to_save[12];
	//str_to_save[11]=0x00;
	memset(str_to_save, 0x00, 12);
	


    // Iterate over each element in the array
	
	
	//strcat(str_to_save, reg_str);
	strcat(str_to_save, "99");
	strcat(str_to_save, "99");
	strcat(str_to_save, "0000000");
    
	printf("%s\n", str_to_save);
	save_reg_data(atoi(reg_str), &str_to_save[0]);
	update_register(atoi(reg_str));
	// Process the selected days array
   // cJSON* day_item;
    //cJSON_ArrayForEach(day_item, selectedDays_json) {
       // if (cJSON_IsString(day_item)) {
         //   const char* day_str = day_item->valuestring;
           // ESP_LOGI(TAG, "Selected Day: %s", day_str);

            // Perform any additional actions based on the selected day
            // ...

            // Release memory when no longer needed
        //}
		
    //}
	//free(day_item);

	//free(selectedDays_json);

	// Send a success response to the client
	// Cerrar la conexion
	free(reg_str);	
	cJSON_Delete(root);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
	

    return ESP_OK;
}






/**
 * wifiConnect.json handler is invoked after the connect button is pressed
 * and handles receiving the SSID and password entered by the user
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */


static esp_err_t http_server_wifi_connect_json_handler(httpd_req_t *req)
{
    size_t header_len;
    char* header_value;
    char* ssid_str = NULL;
    char* pass_str = NULL;
    int content_length;

    ESP_LOGI(TAG, "/wifiConnect.json requested");

    // Get the "Content-Length" header to determine the length of the request body
    header_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (header_len <= 0) {
        // Content-Length header not found or invalid
        //httpd_resp_send_err(req, HTTP_STATUS_411_LENGTH_REQUIRED, "Content-Length header is missing or invalid");
        ESP_LOGI(TAG, "Content-Length header is missing or invalid");
        return ESP_FAIL;
    }

    // Allocate memory to store the header value
    header_value = (char*)malloc(header_len + 1);
    if (httpd_req_get_hdr_value_str(req, "Content-Length", header_value, header_len + 1) != ESP_OK) {
        // Failed to get Content-Length header value
        free(header_value);
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Failed to get Content-Length header value");
        ESP_LOGI(TAG, "Failed to get Content-Length header value");
        return ESP_FAIL;
    }

    // Convert the Content-Length header value to an integer
    content_length = atoi(header_value);
    free(header_value);

    if (content_length <= 0) {
        // Content length is not a valid positive integer
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid Content-Length value");
        ESP_LOGI(TAG, "Invalid Content-Length value");
        return ESP_FAIL;
    }

    // Allocate memory for the data buffer based on the content length
    char* data_buffer = (char*)malloc(content_length + 1);

    // Read the request body into the data buffer
    if (httpd_req_recv(req, data_buffer, content_length) <= 0) {
        // Handle error while receiving data
        free(data_buffer);
        //httpd_resp_send_err(req, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to receive request body");
        ESP_LOGI(TAG, "Failed to receive request body");
        return ESP_FAIL;
    }

    // Null-terminate the data buffer to treat it as a string
    data_buffer[content_length] = '\0';

    // Parse the received JSON data
    cJSON* root = cJSON_Parse(data_buffer);
    free(data_buffer);

    if (root == NULL) {
        // JSON parsing error
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid JSON data");
        ESP_LOGI(TAG, "Invalid JSON data");
        return ESP_FAIL;
    }

    cJSON* ssid_json = cJSON_GetObjectItem(root, "selectedSSID");
    cJSON* pwd_json = cJSON_GetObjectItem(root, "pwd");

    if (ssid_json == NULL || pwd_json == NULL || !cJSON_IsString(ssid_json) || !cJSON_IsString(pwd_json)) {
        cJSON_Delete(root);
        // Missing or invalid JSON fields
        //httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Missing or invalid JSON data fields");
        ESP_LOGI(TAG, "Missing or invalid JSON data fields");
        return ESP_FAIL;
    }

    // Extract SSID and password from JSON
    ssid_str = strdup(ssid_json->valuestring);
    pass_str = strdup(pwd_json->valuestring);

    cJSON_Delete(root);

    // Now, you have the SSID and password in ssid_str and pass_str
    ESP_LOGI(TAG, "Received SSID: %s", ssid_str);
    ESP_LOGI(TAG, "Received Password: %s", pass_str);

    // Update the Wifi networks configuration and let the wifi application know
    wifi_config_t* wifi_config = wifi_app_get_wifi_config();
	memset(wifi_config, 0x00, sizeof(wifi_config_t));
    //memset(wifi_config->sta.ssid, 0x00, sizeof(wifi_config->sta.ssid));
	//memset(wifi_config->sta.password, 0x00, sizeof(wifi_config->sta.password));
    memcpy(wifi_config->sta.ssid, ssid_str, strlen(ssid_str));
    memcpy(wifi_config->sta.password, pass_str, strlen(pass_str));
	save_wifi_credentials(ssid_str, pass_str);
	esp_wifi_disconnect();
    //wifi_app_send_message(WIFI_APP_MSG_CONNECTING_FROM_HTTP_SERVER); // if doesn't work this need to be checked
	connect_to_wifi();


    free(ssid_str);
    free(pass_str);

    return http_server_send_ok(req);
}


/**
 * wifiConnectStatus handler updates the connection status for the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_wifi_connect_status_json_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "/wifiConnectStatus requested");

	char statusJSON[100];

	sprintf(statusJSON, "{\"wifi_connect_status\":%d}", g_wifi_connect_status);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, statusJSON, strlen(statusJSON));

	return ESP_OK;
}

/**
 * Sets up the default httpd server configuration.
 * @return http server instance handle if successful, NULL otherwise.
 */
static httpd_handle_t http_server_configure(void)
{
	// Generate the default configuration
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	// Create the message queue
	http_server_monitor_queue_handle = xQueueCreate(3, sizeof(http_server_queue_message_t));

	// Create HTTP server monitor task
	xTaskCreatePinnedToCore(&http_server_monitor, "http_server_monitor", HTTP_SERVER_MONITOR_STACK_SIZE, NULL, HTTP_SERVER_MONITOR_PRIORITY, &task_http_server_monitor, HTTP_SERVER_MONITOR_CORE_ID);

	

	// The core that the HTTP server will run on
	config.core_id = HTTP_SERVER_TASK_CORE_ID;

	// Adjust the default priority to 1 less than the wifi application task
	config.task_priority = HTTP_SERVER_TASK_PRIORITY;

	// Bump up the stack size (default is 4096)
	config.stack_size = HTTP_SERVER_TASK_STACK_SIZE;

	// Increase uri handlers
	config.max_uri_handlers = 24;

	// Increase the timeout limits
	config.recv_wait_timeout = 10;
	config.send_wait_timeout = 10;

	ESP_LOGI(TAG,
			"http_server_configure: Starting server on port: '%d' with task priority: '%d'",
			config.server_port,
			config.task_priority);

	// Start the httpd server
	if (httpd_start(&http_server_handle, &config) == ESP_OK)
	{
		ESP_LOGI(TAG, "http_server_configure: Registering URI handlers");

		// register query handler
		httpd_uri_t jquery_js = {
				.uri = "/jquery-3.3.1.min.js",
				.method = HTTP_GET,
				.handler = http_server_jquery_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &jquery_js);

		// register index.html handler
		httpd_uri_t index_html = {
				.uri = "/",
				.method = HTTP_GET,
				.handler = http_server_index_html_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &index_html);

		// register app.css handler
		httpd_uri_t app_css = {
				.uri = "/app.css",
				.method = HTTP_GET,
				.handler = http_server_app_css_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &app_css);

		// register app.js handler
		httpd_uri_t app_js = {
				.uri = "/app.js",
				.method = HTTP_GET,
				.handler = http_server_app_js_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &app_js);

		// register favicon.ico handler
		httpd_uri_t favicon_ico = {
				.uri = "/favicon.ico",
				.method = HTTP_GET,
				.handler = http_server_favicon_ico_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &favicon_ico);

		httpd_uri_t env_state = {
				.uri = "/api/state",
				.method = HTTP_GET,
				.handler = http_server_env_state_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &env_state);

		httpd_uri_t curtain_control = {
				.uri = "/api/curtain",
				.method = HTTP_POST,
				.handler = http_server_curtain_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &curtain_control);

		httpd_uri_t schedule_control = {
				.uri = "/api/schedule",
				.method = HTTP_POST,
				.handler = http_server_schedule_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &schedule_control);

		httpd_uri_t fan_control = {
				.uri = "/api/fan",
				.method = HTTP_POST,
				.handler = http_server_fan_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &fan_control);

		httpd_uri_t rgb_control = {
				.uri = "/api/rgb",
				.method = HTTP_POST,
				.handler = http_server_rgb_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &rgb_control);

		httpd_uri_t ap_config = {
				.uri = "/apConfig.json",
				.method = HTTP_POST,
				.handler = http_server_ap_config_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &ap_config);

		httpd_uri_t toogle_led = {
				.uri = "/toogle_led.json",
				.method = HTTP_POST,
				.handler = http_server_toogle_led_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &toogle_led );


		// register wifiConnect.json handler
		httpd_uri_t wifi_connect_json = {
				.uri = "/wifiConnect.json",
				.method = HTTP_POST,
				.handler = http_server_wifi_connect_json_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &wifi_connect_json);
		

		// register OTAupdate handler
		httpd_uri_t OTA_update = {
				.uri = "/OTAupdate",
				.method = HTTP_POST,
				.handler = http_server_OTA_update_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &OTA_update);

		// register OTAstatus handler
		httpd_uri_t OTA_status = {
				.uri = "/OTAstatus",
				.method = HTTP_POST,
				.handler = http_server_OTA_status_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &OTA_status);


		// register OTAstatus handler
		httpd_uri_t register_change = {
				.uri = "/regchange.json",
				.method = HTTP_POST,
				.handler = http_server_register_change_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &register_change);

		// register erase handler
		httpd_uri_t register_erase = {
				.uri = "/regerase.json",
				.method = HTTP_POST,
				.handler = http_server_register_erase_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &register_erase);
		

		 //register dhtSensor.json handler
		httpd_uri_t dht_sensor_json = {
				.uri = "/dhtSensor.json",
				.method = HTTP_GET,
				.handler = http_server_get_dht_sensor_readings_json_handler,
				.user_ctx = NULL
		};
	httpd_register_uri_handler(http_server_handle, &dht_sensor_json);



		

		// register wifiConnectStatus.json handler
		httpd_uri_t wifi_connect_status_json = {
				.uri = "/wifiConnectStatus",
				.method = HTTP_POST,
				.handler = http_server_wifi_connect_status_json_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &wifi_connect_status_json);

		httpd_uri_t read_range_uri = {
				.uri = "/readreg.json",
				.method = HTTP_POST,
				.handler = http_server_read_register_handler,
				.user_ctx = NULL
		};
		httpd_register_uri_handler(http_server_handle, &read_range_uri );





		

		return http_server_handle;
	}

	return NULL;
}

void http_server_start(void)
{
	if (http_server_handle == NULL)
	{
		http_server_handle = http_server_configure();
	}
}

void http_server_stop(void)
{
	if (http_server_handle)
	{
		httpd_stop(http_server_handle);
		ESP_LOGI(TAG, "http_server_stop: stopping HTTP server");
		http_server_handle = NULL;
	}
	if (task_http_server_monitor)
	{
		vTaskDelete(task_http_server_monitor);
		ESP_LOGI(TAG, "http_server_stop: stopping HTTP server monitor");
		task_http_server_monitor = NULL;
	}
}

BaseType_t http_server_monitor_send_message(http_server_message_e msgID)
{
	http_server_queue_message_t msg;
	msg.msgID = msgID;
	return xQueueSend(http_server_monitor_queue_handle, &msg, portMAX_DELAY);
}

void http_server_fw_update_reset_callback(void *arg)
{
	ESP_LOGI(TAG, "http_server_fw_update_reset_callback: Timer timed-out, restarting the device");
	esp_restart();
}

























