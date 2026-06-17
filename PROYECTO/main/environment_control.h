#ifndef MAIN_ENVIRONMENT_CONTROL_H_
#define MAIN_ENVIRONMENT_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define ENV_SCHEDULE_COUNT 8

#define ENV_SERVO_GPIO 18
#define ENV_FAN_PWM_GPIO 25
#define ENV_ALARM_LED_GPIO 2
#define ENV_TEMP_SENSOR_GPIO 34
#define ENV_RGB_RED_GPIO 21
#define ENV_RGB_GREEN_GPIO 22
#define ENV_RGB_BLUE_GPIO 23

typedef enum
{
    ENV_CURTAIN_MODE_MANUAL = 0,
    ENV_CURTAIN_MODE_SCHEDULE = 1,
} env_curtain_mode_t;

typedef enum
{
    ENV_FAN_MODE_MANUAL = 0,
    ENV_FAN_MODE_AUTO = 1,
} env_fan_mode_t;

typedef struct
{
    bool enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t opening_percent;
} env_schedule_record_t;

typedef struct
{
    float temperature_c;
    float desired_temp_c;
    float max_temp_c;
    uint8_t fan_speed_percent;
    uint8_t manual_fan_speed_percent;
    uint8_t curtain_opening_percent;
    env_curtain_mode_t curtain_mode;
    env_fan_mode_t fan_mode;
    uint8_t rgb_red;
    uint8_t rgb_green;
    uint8_t rgb_blue;
    uint8_t rgb_brightness_percent;
    bool alarm_active;
    env_schedule_record_t schedule[ENV_SCHEDULE_COUNT];
} env_control_state_t;

void env_control_start(void);
void env_control_get_state(env_control_state_t *state);

esp_err_t env_control_set_curtain_manual(uint8_t opening_percent);
esp_err_t env_control_set_curtain_mode(env_curtain_mode_t mode);
esp_err_t env_control_set_schedule_record(uint8_t index, bool enabled, uint8_t hour, uint8_t minute, uint8_t opening_percent);

esp_err_t env_control_set_fan_manual(uint8_t speed_percent);
esp_err_t env_control_set_fan_auto(float desired_temp_c, float max_temp_c);
esp_err_t env_control_set_fan_mode(env_fan_mode_t mode);
esp_err_t env_control_set_temperatures(float desired_temp_c, float max_temp_c);

esp_err_t env_control_set_rgb(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness_percent);

#endif
