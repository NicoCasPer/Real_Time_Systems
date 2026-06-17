/* LEDC (LED Controller) basic example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "driver/ledc.h"
#include "esp_err.h"
#include "library_led_c.h"

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          GPIO_NUM_2 //(5) // Define the output GPIO
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY               (819) // Set duty to 50%. (2 ** 13) * 50% = 4096
#define LEDC_FREQUENCY          (20) // Frequency in Hertz. Set frequency at 4 kHz
#define LED_RGB1_RED_GPIO       GPIO_NUM_2
#define LED_RGB1_GREEN_GPIO     GPIO_NUM_4
#define LED_RGB1_BLUE_GPIO      GPIO_NUM_5

/* Warning:
 * For ESP32, ESP32S2, ESP32S3, ESP32C3, ESP32C2, ESP32C6, ESP32H2 (rev < 1.2), ESP32P4 (rev < 3.0) targets,
 * when LEDC_DUTY_RES selects the maximum duty resolution (i.e. value equal to SOC_LEDC_TIMER_BIT_WIDTH),
 * 100% duty cycle is not reachable (duty cannot be set to (2 ** SOC_LEDC_TIMER_BIT_WIDTH)).
 */

static void example_ledc_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_DUTY_RES,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void app_main(void)
{
    led_rgb_t led_rgb1 = {
        .led_red = {
            .duty = 0,
            .gpio_num = LED_RGB1_RED_GPIO,
            .channel = LEDC_CHANNEL_0,
        },
        .led_green = {
            .duty = 0,
            .gpio_num = LED_RGB1_GREEN_GPIO,
            .channel = LEDC_CHANNEL_1,
        },
        .led_blue = {
            .duty = 0,
            .gpio_num = LED_RGB1_BLUE_GPIO,
            .channel = LEDC_CHANNEL_2,
        },
        .timer = LEDC_TIMER_0,
        .frequency = 20,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .speed_mode = LEDC_LOW_SPEED_MODE
    };

    while(1)
    {
        config_led_rgb(&led_rgb1);
        set_led_rgb_percentage_given_values(&led_rgb1, 50, 100, 25);
        vTask_delay(1000 / portTICK_PERIOD_MS);
        set_led_rgb_percentage_given_values(&led_rgb1, 100, 50, 75);
        vTask_delay(1000 / portTICK_PERIOD_MS);
    }

    // Set the LEDC peripheral configuration
    //example_ledc_init();
    // Set duty to 50%
    //ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY));
    // Update duty to apply the new value
    //ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}
