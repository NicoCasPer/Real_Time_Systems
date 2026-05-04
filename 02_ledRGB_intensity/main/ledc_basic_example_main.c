/* LEDC (LED Controller) basic example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "driver/ledc.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "library_led_c.h"

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          GPIO_NUM_2 //(5) // Define the output GPIO
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY               (819) // Set duty to 50%. (2 ** 13) * 50% = 4096
#define LEDC_FREQUENCY          (4000) // Frequency in Hertz. Set frequency at 4 kHz
#define LED_RGB1_RED_GPIO       GPIO_NUM_27
#define LED_RGB1_GREEN_GPIO     GPIO_NUM_4
#define LED_RGB1_BLUE_GPIO      GPIO_NUM_5
#define BUTTON_RGB1_RED_GPIO    GPIO_NUM_18
#define BUTTON_RGB1_GREEN_GPIO  GPIO_NUM_19
#define BUTTON_RGB1_BLUE_GPIO   GPIO_NUM_21
#define BUTTON_PRESSED_LEVEL    0
#define BUTTON_DEBOUNCE_TIME_MS 50
#define BUTTON_RELEASE_TIME_MS  10
#define LED_PERCENTAGE_STEP     10
#define LED_PERCENTAGE_MAX      100

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

static int button_pressed(gpio_num_t button_gpio)
{
    if (gpio_get_level(button_gpio) == BUTTON_PRESSED_LEVEL)
    {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_TIME_MS));
        if (gpio_get_level(button_gpio) == BUTTON_PRESSED_LEVEL)
        {
            while (gpio_get_level(button_gpio) == BUTTON_PRESSED_LEVEL)
            {
                vTaskDelay(pdMS_TO_TICKS(BUTTON_RELEASE_TIME_MS));
            }
            return 1;
        }
    }
    return 0;
}

static int increase_percentage(int percentage)
{
    percentage += LED_PERCENTAGE_STEP;
    if (percentage > LED_PERCENTAGE_MAX)
    {
        percentage = 0;
    }
    return percentage;
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
        .timer = LEDC_TIMER,
        .frequency = LEDC_FREQUENCY,
        .duty_resolution = LEDC_DUTY_RES,
        .speed_mode = LEDC_MODE
    };

    button_rgb_t button_rgb1 = {
        .button_red = {
            .gpio_num = BUTTON_RGB1_RED_GPIO,
        },
        .button_green = {
            .gpio_num = BUTTON_RGB1_GREEN_GPIO,
        },
        .button_blue = {
            .gpio_num = BUTTON_RGB1_BLUE_GPIO,
        },
    };

    int percentage_red = 0;
    int percentage_green = 0;
    int percentage_blue = 0;

    config_led_rgb(&led_rgb1);
    config_buttons_rgb(&button_rgb1);
    set_led_rgb_percentage_given_values(&led_rgb1, percentage_red, percentage_green, percentage_blue);

    while(1)
    {
        if (button_pressed(button_rgb1.button_red.gpio_num))
        {
            percentage_red = increase_percentage(percentage_red);
            set_led_rgb_percentage_given_values(&led_rgb1, percentage_red, percentage_green, percentage_blue);
        }
        if (button_pressed(button_rgb1.button_green.gpio_num))
        {
            percentage_green = increase_percentage(percentage_green);
            set_led_rgb_percentage_given_values(&led_rgb1, percentage_red, percentage_green, percentage_blue);
        }
        if (button_pressed(button_rgb1.button_blue.gpio_num))
        {
            percentage_blue = increase_percentage(percentage_blue);
            set_led_rgb_percentage_given_values(&led_rgb1, percentage_red, percentage_green, percentage_blue);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Set the LEDC peripheral configuration
    //example_ledc_init();
    // Set duty to 50%
    //ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY));
    // Update duty to apply the new value
    //ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}
