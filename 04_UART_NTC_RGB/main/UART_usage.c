#include "UART_usage.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define UART_USAGE_PORT_NUM          UART_NUM_0
#define UART_USAGE_BAUD_RATE         115200
#define UART_USAGE_RX_BUFFER_SIZE    256
#define UART_USAGE_TASK_STACK_SIZE   4096
#define UART_USAGE_TASK_PRIORITY     4
#define UART_USAGE_LINE_MAX          96
#define UART_USAGE_READ_TIMEOUT_MS   100
#define PWM_8BIT_MAX                 255U

typedef enum {
    UART_RGB_RED = 0,
    UART_RGB_GREEN,
    UART_RGB_BLUE,
    UART_RGB_COLOR_COUNT,
} uart_rgb_color_t;

typedef struct {
    float min_c;
    float max_c;
} temperature_limit_t;

typedef struct {
    int ntc_raw;
    float temperature_c;
    bool has_temperature;

    int pot_raw;
    uint8_t rgb2_red;
    uint8_t rgb2_green;
    uint8_t rgb2_blue;
    bool has_rgb2;
} uart_usage_snapshot_t;

static const char *TAG = "UART_USAGE";

static temperature_limit_t s_limits[UART_RGB_COLOR_COUNT] = {
    [UART_RGB_RED] = {.min_c = 35.0f, .max_c = 100.0f},
    [UART_RGB_GREEN] = {.min_c = 25.0f, .max_c = 35.0f},
    [UART_RGB_BLUE] = {.min_c = 0.0f, .max_c = 25.0f},
};

static uart_usage_callbacks_t s_callbacks;
static uart_usage_snapshot_t s_snapshot;
static SemaphoreHandle_t s_state_mutex;

static void uart_usage_lock(void)
{
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }
}

static void uart_usage_unlock(void)
{
    if (s_state_mutex != NULL) {
        xSemaphoreGive(s_state_mutex);
    }
}

static char *trim_whitespace(char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    char *end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return text;
}

static void uppercase_copy(char *dest, size_t dest_size, const char *src)
{
    if (dest_size == 0) {
        return;
    }

    size_t i = 0;
    for (; i < dest_size - 1 && src[i] != '\0'; i++) {
        dest[i] = (char)toupper((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

static bool is_temperature_inside_limit(float temperature_c,
                                        const temperature_limit_t *limit)
{
    return temperature_c >= limit->min_c && temperature_c <= limit->max_c;
}

static int pwm_to_percent(uint8_t pwm)
{
    return ((int)pwm * 100 + (int)(PWM_8BIT_MAX / 2U)) / (int)PWM_8BIT_MAX;
}

static bool color_from_command(char command, uart_rgb_color_t *out_color)
{
    switch (toupper((unsigned char)command)) {
    case 'R':
        *out_color = UART_RGB_RED;
        return true;
    case 'G':
        *out_color = UART_RGB_GREEN;
        return true;
    case 'B':
        *out_color = UART_RGB_BLUE;
        return true;
    default:
        return false;
    }
}

static char color_to_letter(uart_rgb_color_t color)
{
    switch (color) {
    case UART_RGB_RED:
        return 'R';
    case UART_RGB_GREEN:
        return 'G';
    case UART_RGB_BLUE:
        return 'B';
    default:
        return '?';
    }
}

static bool parse_temperature_range(const char *text, float *out_min, float *out_max)
{
    while (isspace((unsigned char)*text) || *text == '=') {
        text++;
    }

    char *end = NULL;
    const float min_c = strtof(text, &end);
    if (end == text) {
        return false;
    }

    text = end;
    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text != '-') {
        return false;
    }
    text++;

    while (isspace((unsigned char)*text)) {
        text++;
    }

    const float max_c = strtof(text, &end);
    if (end == text) {
        return false;
    }

    text = end;
    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text != '\0') {
        return false;
    }

    if (min_c < 0.0f || max_c > 100.0f || min_c > max_c) {
        return false;
    }

    *out_min = min_c;
    *out_max = max_c;
    return true;
}

static void print_limits(void)
{
    temperature_limit_t limits[UART_RGB_COLOR_COUNT];

    uart_usage_lock();
    memcpy(limits, s_limits, sizeof(limits));
    uart_usage_unlock();

    printf("\nLimites RGB1 por temperatura:\n");
    printf("  R %.2f-%.2f C\n", limits[UART_RGB_RED].min_c, limits[UART_RGB_RED].max_c);
    printf("  G %.2f-%.2f C\n", limits[UART_RGB_GREEN].min_c, limits[UART_RGB_GREEN].max_c);
    printf("  B %.2f-%.2f C\n", limits[UART_RGB_BLUE].min_c, limits[UART_RGB_BLUE].max_c);
}

static void print_help(void)
{
    printf("\nComandos UART disponibles:\n");
    printf("  R 5-20              -> cambia el rango rojo de RGB1 (0..100 C)\n");
    printf("  G 15-40             -> cambia el rango verde de RGB1 (0..100 C)\n");
    printf("  B 10-30             -> cambia el rango azul de RGB1 (0..100 C)\n");
    printf("  READ_TEMP o READ    -> lee temperatura actual del NTC\n");
    printf("  READ_LED_VALUES     -> lee porcentaje actual de RGB2 y potenciometro\n");
    printf("  LIMITS              -> muestra los rangos actuales\n");
    printf("  HELP                -> muestra esta ayuda\n");
}

static void print_temperature_reading(void)
{
    int ntc_raw = 0;
    float temperature_c = 0.0f;

    if (s_callbacks.read_temperature != NULL &&
        s_callbacks.read_temperature(&ntc_raw, &temperature_c) == ESP_OK) {
        uart_usage_update_temperature_snapshot(ntc_raw, temperature_c);
        printf("\nREAD_TEMP -> NTC raw=%d | T=%.2f C\n", ntc_raw, temperature_c);
        return;
    }

    bool has_temperature = false;

    uart_usage_lock();
    ntc_raw = s_snapshot.ntc_raw;
    temperature_c = s_snapshot.temperature_c;
    has_temperature = s_snapshot.has_temperature;
    uart_usage_unlock();

    if (has_temperature) {
        printf("\nREAD_TEMP -> NTC raw=%d | T=%.2f C\n", ntc_raw, temperature_c);
    } else {
        printf("\nERROR: todavia no hay lectura de temperatura disponible.\n");
    }
}

static void print_led_values(void)
{
    int pot_raw = 0;
    uint8_t pot_pwm = 0;
    bool has_pot_reading = false;

    if (s_callbacks.read_potentiometer != NULL &&
        s_callbacks.read_potentiometer(&pot_raw, &pot_pwm) == ESP_OK) {
        has_pot_reading = true;
    }

    uart_usage_snapshot_t snapshot;

    uart_usage_lock();
    snapshot = s_snapshot;
    uart_usage_unlock();

    if (!has_pot_reading && snapshot.has_rgb2) {
        pot_raw = snapshot.pot_raw;
        has_pot_reading = true;
    }

    if (!snapshot.has_rgb2) {
        printf("\nERROR: todavia no hay valores de RGB2 disponibles.\n");
        return;
    }

    printf("\nREAD_LED_VALUES -> RGB2 R=%d%% G=%d%% B=%d%%",
           pwm_to_percent(snapshot.rgb2_red),
           pwm_to_percent(snapshot.rgb2_green),
           pwm_to_percent(snapshot.rgb2_blue));

    if (has_pot_reading) {
        printf(" | POT raw=%d | POT=%d%%",
               pot_raw,
               pwm_to_percent(pot_pwm));
    }

    printf("\n");
}

static void set_limit_from_command(char color_command, const char *range_text)
{
    uart_rgb_color_t color = UART_RGB_RED;
    float min_c = 0.0f;
    float max_c = 0.0f;

    if (!color_from_command(color_command, &color) ||
        !parse_temperature_range(range_text, &min_c, &max_c)) {
        printf("\nERROR: usa formato R 5-20, G 15-40 o B 10-30 con valores 0..100.\n");
        return;
    }

    uart_usage_lock();
    s_limits[color].min_c = min_c;
    s_limits[color].max_c = max_c;
    uart_usage_unlock();

    printf("\nOK: %c %.2f-%.2f C\n", color_to_letter(color), min_c, max_c);
}

static void process_command(char *line)
{
    char *command = trim_whitespace(line);
    if (*command == '\0') {
        return;
    }

    char upper_command[UART_USAGE_LINE_MAX];
    uppercase_copy(upper_command, sizeof(upper_command), command);

    if (strcmp(upper_command, "HELP") == 0 || strcmp(upper_command, "?") == 0) {
        print_help();
        return;
    }

    if (strcmp(upper_command, "LIMITS") == 0) {
        print_limits();
        return;
    }

    if (strcmp(upper_command, "READ") == 0 ||
        strcmp(upper_command, "READ_TEMP") == 0 ||
        strcmp(upper_command, "READ TEMP") == 0 ||
        strcmp(upper_command, "TEMP") == 0) {
        print_temperature_reading();
        return;
    }

    if (strcmp(upper_command, "READ_LED_VALUES") == 0 ||
        strcmp(upper_command, "READ LED VALUES") == 0) {
        print_led_values();
        return;
    }

    uart_rgb_color_t color = UART_RGB_RED;
    if (color_from_command(command[0], &color) &&
        (command[1] == '\0' || isspace((unsigned char)command[1]) || command[1] == '=')) {
        set_limit_from_command(command[0], &command[1]);
        return;
    }

    printf("\nERROR: comando desconocido. Escribe HELP para ver opciones.\n");
}

static void uart_usage_task(void *arg)
{
    (void)arg;

    uint8_t rx_buffer[64];
    char line[UART_USAGE_LINE_MAX];
    size_t line_len = 0;

    print_help();

    while (true) {
        const int length = uart_read_bytes(UART_USAGE_PORT_NUM,
                                           rx_buffer,
                                           sizeof(rx_buffer),
                                           pdMS_TO_TICKS(UART_USAGE_READ_TIMEOUT_MS));

        for (int i = 0; i < length; i++) {
            const char ch = (char)rx_buffer[i];

            if (ch == '\r' || ch == '\n') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    process_command(line);
                    line_len = 0;
                }
                continue;
            }

            if (ch == '\b' || ch == 127) {
                if (line_len > 0) {
                    line_len--;
                }
                continue;
            }

            if (isprint((unsigned char)ch) && line_len < sizeof(line) - 1) {
                line[line_len] = ch;
                line_len++;
            }
        }
    }
}

esp_err_t uart_usage_init(const uart_usage_callbacks_t *callbacks)
{
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    } else {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
    }

    const uart_config_t uart_config = {
        .baud_rate = UART_USAGE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (!uart_is_driver_installed(UART_USAGE_PORT_NUM)) {
        esp_err_t err = uart_driver_install(UART_USAGE_PORT_NUM,
                                            UART_USAGE_RX_BUFFER_SIZE,
                                            0,
                                            0,
                                            NULL,
                                            0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo instalar driver UART: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_ERROR_CHECK(uart_param_config(UART_USAGE_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_USAGE_PORT_NUM,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    if (xTaskCreate(uart_usage_task,
                    "uart_usage",
                    UART_USAGE_TASK_STACK_SIZE,
                    NULL,
                    UART_USAGE_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void uart_usage_temperature_to_rgb(float temperature_c,
                                   uint8_t on_intensity,
                                   uint8_t *out_red,
                                   uint8_t *out_green,
                                   uint8_t *out_blue)
{
    temperature_limit_t limits[UART_RGB_COLOR_COUNT];

    uart_usage_lock();
    memcpy(limits, s_limits, sizeof(limits));
    uart_usage_unlock();

    if (out_red != NULL) {
        *out_red = is_temperature_inside_limit(temperature_c, &limits[UART_RGB_RED]) ?
                   on_intensity : 0;
    }
    if (out_green != NULL) {
        *out_green = is_temperature_inside_limit(temperature_c, &limits[UART_RGB_GREEN]) ?
                     on_intensity : 0;
    }
    if (out_blue != NULL) {
        *out_blue = is_temperature_inside_limit(temperature_c, &limits[UART_RGB_BLUE]) ?
                    on_intensity : 0;
    }
}

void uart_usage_update_temperature_snapshot(int ntc_raw, float temperature_c)
{
    uart_usage_lock();
    s_snapshot.ntc_raw = ntc_raw;
    s_snapshot.temperature_c = temperature_c;
    s_snapshot.has_temperature = true;
    uart_usage_unlock();
}

void uart_usage_update_rgb2_snapshot(int pot_raw,
                                     uint8_t red,
                                     uint8_t green,
                                     uint8_t blue)
{
    uart_usage_lock();
    s_snapshot.pot_raw = pot_raw;
    s_snapshot.rgb2_red = red;
    s_snapshot.rgb2_green = green;
    s_snapshot.rgb2_blue = blue;
    s_snapshot.has_rgb2 = true;
    uart_usage_unlock();
}
