#include "rgb_led.h"

#include <stddef.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"

typedef struct {
    ledc_channel_t red_channel;
    ledc_channel_t green_channel;
    ledc_channel_t blue_channel;
} rgb_channels_t;

static const rgb_channels_t s_leds[RGB_LED_COUNT] = {
    [RGB_LED_TEMPERATURE] = {
        .red_channel = LED_RGB1_RED_CHANNEL,
        .green_channel = LED_RGB1_GREEN_CHANNEL,
        .blue_channel = LED_RGB1_BLUE_CHANNEL,
    },
    [RGB_LED_CONFIGURABLE] = {
        .red_channel = LED_RGB2_RED_CHANNEL,
        .green_channel = LED_RGB2_GREEN_CHANNEL,
        .blue_channel = LED_RGB2_BLUE_CHANNEL,
    },
};

static uint32_t led_intensity_to_duty(uint8_t intensity)
{
#if RGB_LED_COMMON_ANODE
    return LEDC_MAX_DUTY - intensity;
#else
    return intensity;
#endif
}

static esp_err_t configure_ledc_channel(gpio_num_t gpio_num, ledc_channel_t channel)
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

    return ledc_channel_config(&ledc_channel);
}

esp_err_t rgb_led_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        return err;
    }

    const struct {
        gpio_num_t gpio;
        ledc_channel_t channel;
    } channels[] = {
        {LED_RGB1_RED_GPIO, LED_RGB1_RED_CHANNEL},
        {LED_RGB1_GREEN_GPIO, LED_RGB1_GREEN_CHANNEL},
        {LED_RGB1_BLUE_GPIO, LED_RGB1_BLUE_CHANNEL},
        {LED_RGB2_RED_GPIO, LED_RGB2_RED_CHANNEL},
        {LED_RGB2_GREEN_GPIO, LED_RGB2_GREEN_CHANNEL},
        {LED_RGB2_BLUE_GPIO, LED_RGB2_BLUE_CHANNEL},
    };

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        err = configure_ledc_channel(channels[i].gpio, channels[i].channel);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t rgb_led_set(rgb_led_id_t led_id, uint8_t red, uint8_t green, uint8_t blue)
{
    if (led_id < 0 || led_id >= RGB_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const rgb_channels_t *rgb = &s_leds[led_id];

    esp_err_t err = ledc_set_duty(LEDC_MODE, rgb->red_channel, led_intensity_to_duty(red));
    if (err != ESP_OK) {
        return err;
    }
    err = ledc_set_duty(LEDC_MODE, rgb->green_channel, led_intensity_to_duty(green));
    if (err != ESP_OK) {
        return err;
    }
    err = ledc_set_duty(LEDC_MODE, rgb->blue_channel, led_intensity_to_duty(blue));
    if (err != ESP_OK) {
        return err;
    }

    err = ledc_update_duty(LEDC_MODE, rgb->red_channel);
    if (err != ESP_OK) {
        return err;
    }
    err = ledc_update_duty(LEDC_MODE, rgb->green_channel);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(LEDC_MODE, rgb->blue_channel);
}
