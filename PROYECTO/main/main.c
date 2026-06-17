/**
 * Application entry point.
 */

#include "nvs_flash.h"
//#include "http_server.h"
#include "environment_control.h"
#include "wifi_app.h"


void app_main(void)
{
    // Initialize NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// Start Wifi
	init_obtain_time();
	env_control_start();
	wifi_app_start();
}
