
#include "driver/ledc.h"
#include "driver/gpio.h"
#include <stdio.h>

typedef struct{    
    uint32_t duty;    
    gpio_num_t gpio_num;
    ledc_channel_t channel;    
    
} led_t;

typedef struct{
    led_t led_red;
    led_t led_green;
    led_t led_blue;
    ledc_timer_t timer;
    ledc_timer_bit_t duty_resolution;
    uint32_t frequency;
    ledc_mode_t speed_mode;
} led_rgb_t;

typedef struct{
    gpio_num_t gpio_num;
} button_t;

typedef struct{
    button_t button_red;
    button_t button_green;
    button_t button_blue;
} button_rgb_t;

void config_led_rgb(led_rgb_t *led_rgb);
void config_buttons_rgb(button_rgb_t *button_rgb);
void set_led_rgb_given_struct(led_rgb_t *led_rgb);
void set_led_rgb_percentage_given_values(led_rgb_t *led_rgb, int percentage_red, int percentage_green, int percentage_blue);     
void set_led_rgb_given_values(led_rgb_t *led_rgb, uint32_t duty_red, uint32_t duty_green, uint32_t duty_blue);
