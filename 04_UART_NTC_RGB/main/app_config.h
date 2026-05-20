#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

/* ----------------------------- Pinout usado ----------------------------- */

#define NTC_ADC_CHANNEL         ADC_CHANNEL_6   // GPIO_NUM_34, ADC1_CH6
#define POT_ADC_CHANNEL         ADC_CHANNEL_7   // GPIO_NUM_35, ADC1_CH7
#define BUTTON_MODE_GPIO        GPIO_NUM_16     // Boton con pull-up interno

#define LED_RGB1_RED_GPIO       GPIO_NUM_27
#define LED_RGB1_GREEN_GPIO     GPIO_NUM_4
#define LED_RGB1_BLUE_GPIO      GPIO_NUM_5

#define LED_RGB2_RED_GPIO       GPIO_NUM_12
#define LED_RGB2_GREEN_GPIO     GPIO_NUM_13
#define LED_RGB2_BLUE_GPIO      GPIO_NUM_14

/* ------------------------- Configuracion general ------------------------- */

#define ADC_MAX_RAW                 4095
#define ADC_AVERAGE_SAMPLES         16
#define ADC_INITIAL_CHECK_SAMPLES   10

// En ESP-IDF 5.x, ADC_ATTEN_DB_12 reemplaza a ADC_ATTEN_DB_11 para este rango.
#define ADC_ATTENUATION             ADC_ATTEN_DB_12
#define ADC_BITWIDTH_USED           ADC_BITWIDTH_12

// PWM de 8 bits para mapear directamente el ADC a valores de 0 a 255.
#define LEDC_TIMER                  LEDC_TIMER_0
#define LEDC_MODE                   LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RESOLUTION        LEDC_TIMER_8_BIT
#define LEDC_MAX_DUTY               ((1U << LEDC_DUTY_RESOLUTION) - 1U)
#define LEDC_FREQUENCY_HZ           5000

#define LED_RGB1_RED_CHANNEL        LEDC_CHANNEL_0
#define LED_RGB1_GREEN_CHANNEL      LEDC_CHANNEL_1
#define LED_RGB1_BLUE_CHANNEL       LEDC_CHANNEL_2
#define LED_RGB2_RED_CHANNEL        LEDC_CHANNEL_3
#define LED_RGB2_GREEN_CHANNEL      LEDC_CHANNEL_4
#define LED_RGB2_BLUE_CHANNEL       LEDC_CHANNEL_5

// Cambiar a 1 si el LED RGB es de anodo comun y se enciende con nivel bajo.
#define RGB_LED_COMMON_ANODE        0

#define BUTTON_PRESSED_LEVEL        0
#define BUTTON_DEBOUNCE_TIME_MS     50
#define RGB2_TASK_PERIOD_MS         20
#define TEMP_TASK_PERIOD_MS         1000
#define NTC_ADC_SAMPLES             64

#define STARTUP_ADC_CHECK_TASK_STACK_SIZE  3072
#define STARTUP_ADC_CHECK_TASK_PRIORITY    3
#define TEMP_TASK_STACK_SIZE               4096
#define TEMP_TASK_PRIORITY                 5
#define RGB2_TASK_STACK_SIZE               4096
#define RGB2_TASK_PRIORITY                 5

/* --------------------------- Parametros del NTC -------------------------- */

// Valores tipicos para un termistor NTC 10k/B3950.
#define NTC_VCC_MV                      3300.0f
#define NTC_NOMINAL_RESISTANCE_OHMS     15000.0f
#define NTC_SERIES_RESISTOR_OHMS        10000.0f
#define NTC_BETA_COEFFICIENT            3950.0f
#define NTC_NOMINAL_TEMPERATURE_C       25.0f

/*
 * Conexion asumida para el divisor:
 *
 *   3.3 V --- resistencia fija 10k --- ADC(GPIO34) --- NTC --- GND
 */
