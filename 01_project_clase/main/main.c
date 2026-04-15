#include <stdio.h>
// #include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

static QueueHandle_t led_queue = NULL;
#define LED_GPIO    GPIO_NUM_2
#define BUTTON_GPIO GPIO_NUM_0


typedef enum{
    LED_2_2,
    LED_2_1,
    LED_1_1,
    LED_05_05,
    LED_OFF,

}led_enum_state;

void config_LED_and_Button (void){
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = 1ULL<<LED_GPIO;
    //disable pull-down mode
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //disable pull-up mode
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_INPUT;
    io_
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
}

void button_task(void *pvParameter){
    led_enum_state led_state = LED_2_2;

    while(1){
        if(gpio_get_level(BUTTON_GPIO) == 0){
            switch (led_state){
                case LED_2_2:
                    led_state = LED_2_1;
                    break;
                case LED_2_1:
                    led_state = LED_1_1;
                    break;
                case LED_1_1:
                    led_state = LED_05_05;
                    break;
                case LED_05_05:
                    led_state = LED_OFF;
                    break;
                case LED_OFF:
                    led_state = LED_2_2;
                    break;
                }
            xQueueSend( led_queue, &led_state, 0);

            while(gpio_get_level(BUTTON_GPIO) == 0){
                vTaskDelay(1000 / portTICK_PERIOD_MS);

            }

        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}


void led_task(void *pvParameter){
    led_enum_state led_state_2 = LED_2_2;

    while(1){
        xQueueReceive(led_queue, &led_state_2, 0);

        if ( led_state_2 == LED_2_2){
            gpio_set_level(LED_GPIO,1);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            gpio_set_level( LED_GPIO, 0);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
        else if ( led_state_2 == LED_2_1){
            gpio_set_level(LED_GPIO,1);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            gpio_set_level( LED_GPIO, 0);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
        else if ( led_state_2 == LED_1_1){
            gpio_set_level(LED_GPIO,1);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            gpio_set_level( LED_GPIO, 0);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        else if ( led_state_2 == LED_05_05){
            gpio_set_level(LED_GPIO,1);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            gpio_set_level( LED_GPIO, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        else if ( led_state_2 == LED_OFF){
            gpio_set_level(LED_GPIO,0);
        }   
    }
}

void app_main(void)
{
    config_LED_and_Button();

    led_queue = xQueueCreate(10, sizeof(led_enum_state));

    xTaskCreate(&button_task, "button_task", 2048, NULL, 5, NULL);
    xTaskCreate(&led_task, "led_task", 2048, NULL, 5, NULL);
}