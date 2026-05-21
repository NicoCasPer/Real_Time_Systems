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
#define UART_TEMP_INTERVAL_DEFAULT_S 1U
#define UART_TEMP_INTERVAL_MIN_S     1U
#define UART_TEMP_INTERVAL_MAX_S     3600U
#define RGB1_INTENSITY_DEFAULT_PCT   100U
#define RGB1_INTENSITY_MIN_PCT       0U
#define RGB1_INTENSITY_MAX_PCT       100U
#define RGB1_INTENSITY_STEP_PCT      10U

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
    temperature_limit_t limits[UART_RGB_COLOR_COUNT];
    uint8_t intensity_percent[UART_RGB_COLOR_COUNT];
} uart_usage_rgb1_state_t;

typedef struct {
    int ntc_raw;
    float temperature_c;
    bool has_temperature;

    int pot_raw;
    uint8_t rgb2_red;
    uint8_t rgb2_green;
    uint8_t rgb2_blue;
    bool has_rgb2;

    uint8_t temperature_threshold;          // Umbral 0-100°C del potenciómetro
    bool integrated_led_state;              // Estado actual del LED integrado
} uart_usage_snapshot_t;

static const char *TAG = "UART_USAGE";

static uart_usage_rgb1_state_t s_rgb1_state = {
    .limits = {
        [UART_RGB_RED] = {.min_c = 35.0f, .max_c = 100.0f},
        [UART_RGB_GREEN] = {.min_c = 25.0f, .max_c = 35.0f},
        [UART_RGB_BLUE] = {.min_c = 0.0f, .max_c = 25.0f},
    },
    .intensity_percent = {
        [UART_RGB_RED] = RGB1_INTENSITY_DEFAULT_PCT,
        [UART_RGB_GREEN] = RGB1_INTENSITY_DEFAULT_PCT,
        [UART_RGB_BLUE] = RGB1_INTENSITY_DEFAULT_PCT,
    },
};

static uart_usage_callbacks_t s_callbacks;
static uart_usage_snapshot_t s_snapshot;
static uart_usage_temperature_print_config_t s_temperature_print_config = {
    .interval_seconds = UART_TEMP_INTERVAL_DEFAULT_S,
    .unit = UART_USAGE_TEMPERATURE_UNIT_CELSIUS,
};
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

static uint8_t percent_to_pwm(uint8_t percent)
{
    return (uint8_t)(((uint32_t)percent * PWM_8BIT_MAX + 50U) / 100U);
}

float uart_usage_temperature_to_unit(float temperature_c,
                                     uart_usage_temperature_unit_t unit)
{
    switch (unit) {
    case UART_USAGE_TEMPERATURE_UNIT_FAHRENHEIT:
        return (temperature_c * 9.0f / 5.0f) + 32.0f;
    case UART_USAGE_TEMPERATURE_UNIT_KELVIN:
        return temperature_c + 273.15f;
    case UART_USAGE_TEMPERATURE_UNIT_CELSIUS:
    default:
        return temperature_c;
    }
}

const char *uart_usage_temperature_unit_symbol(uart_usage_temperature_unit_t unit)
{
    switch (unit) {
    case UART_USAGE_TEMPERATURE_UNIT_FAHRENHEIT:
        return "F";
    case UART_USAGE_TEMPERATURE_UNIT_KELVIN:
        return "K";
    case UART_USAGE_TEMPERATURE_UNIT_CELSIUS:
    default:
        return "C";
    }
}

static const char *temperature_unit_name(uart_usage_temperature_unit_t unit)
{
    switch (unit) {
    case UART_USAGE_TEMPERATURE_UNIT_FAHRENHEIT:
        return "Fahrenheit";
    case UART_USAGE_TEMPERATURE_UNIT_KELVIN:
        return "Kelvin";
    case UART_USAGE_TEMPERATURE_UNIT_CELSIUS:
    default:
        return "Celsius";
    }
}

static uart_usage_temperature_unit_t next_temperature_unit(uart_usage_temperature_unit_t unit)
{
    switch (unit) {
    case UART_USAGE_TEMPERATURE_UNIT_CELSIUS:
        return UART_USAGE_TEMPERATURE_UNIT_FAHRENHEIT;
    case UART_USAGE_TEMPERATURE_UNIT_FAHRENHEIT:
        return UART_USAGE_TEMPERATURE_UNIT_KELVIN;
    case UART_USAGE_TEMPERATURE_UNIT_KELVIN:
    default:
        return UART_USAGE_TEMPERATURE_UNIT_CELSIUS;
    }
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

static const char *color_to_name(uart_rgb_color_t color)
{
    switch (color) {
    case UART_RGB_RED:
        return "rojo";
    case UART_RGB_GREEN:
        return "verde";
    case UART_RGB_BLUE:
        return "azul";
    default:
        return "desconocido";
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

static bool parse_intensity_percent(const char *text, uint8_t *out_percent)
{
    while (isspace((unsigned char)*text) || *text == '=') {
        text++;
    }

    if (*text == '-') {
        return false;
    }

    char *end = NULL;
    const unsigned long percent = strtoul(text, &end, 10);
    if (end == text) {
        return false;
    }

    while (isspace((unsigned char)*end)) {
        end++;
    }

    if (*end != '\0' ||
        percent > RGB1_INTENSITY_MAX_PCT ||
        (percent % RGB1_INTENSITY_STEP_PCT) != 0U) {
        return false;
    }

    *out_percent = (uint8_t)percent;
    return true;
}

static bool command_matches_prefix(const char *upper_command, const char *keyword)
{
    const size_t keyword_len = strlen(keyword);

    if (strncmp(upper_command, keyword, keyword_len) != 0) {
        return false;
    }

    const char next = upper_command[keyword_len];
    return next == '\0' || isspace((unsigned char)next) || next == '=';
}

static bool parse_interval_seconds(const char *text, uint32_t *out_interval_seconds)
{
    while (isspace((unsigned char)*text) || *text == '=') {
        text++;
    }

    char *end = NULL;
    const unsigned long interval = strtoul(text, &end, 10);
    if (end == text) {
        return false;
    }

    while (isspace((unsigned char)*end)) {
        end++;
    }

    if (*end != '\0' ||
        interval < UART_TEMP_INTERVAL_MIN_S ||
        interval > UART_TEMP_INTERVAL_MAX_S) {
        return false;
    }

    *out_interval_seconds = (uint32_t)interval;
    return true;
}

static bool parse_temperature_unit(const char *text, uart_usage_temperature_unit_t *out_unit)
{
    while (isspace((unsigned char)*text) || *text == '=') {
        text++;
    }

    char unit_text[16];
    size_t i = 0;
    while (!isspace((unsigned char)text[i]) &&
           text[i] != '\0' &&
           i < sizeof(unit_text) - 1) {
        unit_text[i] = (char)toupper((unsigned char)text[i]);
        i++;
    }
    unit_text[i] = '\0';

    text += i;
    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text != '\0') {
        return false;
    }

    if (strcmp(unit_text, "C") == 0 || strcmp(unit_text, "CELSIUS") == 0) {
        *out_unit = UART_USAGE_TEMPERATURE_UNIT_CELSIUS;
        return true;
    }
    if (strcmp(unit_text, "F") == 0 || strcmp(unit_text, "FAHRENHEIT") == 0) {
        *out_unit = UART_USAGE_TEMPERATURE_UNIT_FAHRENHEIT;
        return true;
    }
    if (strcmp(unit_text, "K") == 0 || strcmp(unit_text, "KELVIN") == 0) {
        *out_unit = UART_USAGE_TEMPERATURE_UNIT_KELVIN;
        return true;
    }

    return false;
}

static void print_limits(void)
{
    uart_usage_rgb1_state_t rgb1_state;

    uart_usage_lock();
    rgb1_state = s_rgb1_state;
    uart_usage_unlock();

    printf("\nLimites RGB1 por temperatura:\n");
    printf("  R %.2f-%.2f C | intensidad=%u%% | PWM=%u\n",
           rgb1_state.limits[UART_RGB_RED].min_c,
           rgb1_state.limits[UART_RGB_RED].max_c,
           (unsigned int)rgb1_state.intensity_percent[UART_RGB_RED],
           (unsigned int)percent_to_pwm(rgb1_state.intensity_percent[UART_RGB_RED]));
    printf("  G %.2f-%.2f C | intensidad=%u%% | PWM=%u\n",
           rgb1_state.limits[UART_RGB_GREEN].min_c,
           rgb1_state.limits[UART_RGB_GREEN].max_c,
           (unsigned int)rgb1_state.intensity_percent[UART_RGB_GREEN],
           (unsigned int)percent_to_pwm(rgb1_state.intensity_percent[UART_RGB_GREEN]));
    printf("  B %.2f-%.2f C | intensidad=%u%% | PWM=%u\n",
           rgb1_state.limits[UART_RGB_BLUE].min_c,
           rgb1_state.limits[UART_RGB_BLUE].max_c,
           (unsigned int)rgb1_state.intensity_percent[UART_RGB_BLUE],
           (unsigned int)percent_to_pwm(rgb1_state.intensity_percent[UART_RGB_BLUE]));
}

static void print_temperature_print_config(void)
{
    const uart_usage_temperature_print_config_t config =
        uart_usage_get_temperature_print_config();

    printf("\nTemperatura periodica: intervalo=%lu s | unidad=%s (%s)\n",
           (unsigned long)config.interval_seconds,
           uart_usage_temperature_unit_symbol(config.unit),
           temperature_unit_name(config.unit));
}

static void print_help(void)
{
    printf("\nComandos UART disponibles:\n");
    printf("  R 5-20              -> cambia el rango rojo de RGB1 (0..100 C)\n");
    printf("  G 15-40             -> cambia el rango verde de RGB1 (0..100 C)\n");
    printf("  B 10-30             -> cambia el rango azul de RGB1 (0..100 C)\n");
    printf("  INTENSITY_R 70      -> intensidad roja RGB1 en %% (0,10,..100)\n");
    printf("  INTENSITY_G 50      -> intensidad verde RGB1 en %% (0,10,..100)\n");
    printf("  INTENSITY_B 30      -> intensidad azul RGB1 en %% (0,10,..100)\n");
    printf("  TEMP_INTERVAL 5     -> imprime temperatura cada 5 s (1..3600)\n");
    printf("  TEMP_UNIT C|F|K     -> cambia unidad de impresion periodica\n");
    printf("  TEMP_CONFIG         -> muestra intervalo y unidad actuales\n");
    printf("  READ_TEMP o READ    -> lee temperatura actual del NTC\n");
    printf("  READ_LED_VALUES     -> lee porcentaje actual de RGB2 y potenciometro\n");
    printf("  THRESHOLD           -> muestra umbral y estado del LED integrado\n");
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
        const uart_usage_temperature_print_config_t config =
            uart_usage_get_temperature_print_config();
        printf("\nREAD_TEMP -> NTC raw=%d | T=%.2f %s\n",
               ntc_raw,
               uart_usage_temperature_to_unit(temperature_c, config.unit),
               uart_usage_temperature_unit_symbol(config.unit));
        return;
    }

    bool has_temperature = false;

    uart_usage_lock();
    ntc_raw = s_snapshot.ntc_raw;
    temperature_c = s_snapshot.temperature_c;
    has_temperature = s_snapshot.has_temperature;
    uart_usage_unlock();

    if (has_temperature) {
        const uart_usage_temperature_print_config_t config =
            uart_usage_get_temperature_print_config();
        printf("\nREAD_TEMP -> NTC raw=%d | T=%.2f %s\n",
               ntc_raw,
               uart_usage_temperature_to_unit(temperature_c, config.unit),
               uart_usage_temperature_unit_symbol(config.unit));
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

/*
 * print_threshold()
 * =================
 * Comando THRESHOLD: Imprime el umbral de temperatura actual (0-100°C)
 * junto con el estado del LED integrado y la temperatura en tiempo real.
 *
 * Formato de salida:
 *   THRESHOLD -> umbral=XX°C | LED=ENCENDIDO/APAGADO | T_actual=YY.YY°C
 *
 * El umbral se lee de forma segura mediante uart_usage_get_temperature_threshold(),
 * el estado del LED mediante uart_usage_get_integrated_led_state(), y la
 * temperatura actual se obtiene del snapshot de lectura más reciente.
 */
static void print_threshold(void)
{
    uint8_t threshold;
    bool led_state;
    int ntc_raw = 0;
    float temperature_c = 0.0f;
    bool has_temperature = false;

    /* Leer el umbral y estado del LED de forma segura */
    threshold = uart_usage_get_temperature_threshold();
    led_state = uart_usage_get_integrated_led_state();

    /* Intentar obtener lectura de temperatura actual */
    if (s_callbacks.read_temperature != NULL &&
        s_callbacks.read_temperature(&ntc_raw, &temperature_c) == ESP_OK) {
        has_temperature = true;
    } else {
        /* Si no hay callback, usar el snapshot más reciente */
        uart_usage_lock();
        ntc_raw = s_snapshot.ntc_raw;
        temperature_c = s_snapshot.temperature_c;
        has_temperature = s_snapshot.has_temperature;
        uart_usage_unlock();
    }

    printf("\nTHRESHOLD -> umbral=%u°C | LED=%s",
           (unsigned int)threshold,
           led_state ? "ENCENDIDO" : "APAGADO");

    if (has_temperature) {
        const uart_usage_temperature_print_config_t config =
            uart_usage_get_temperature_print_config();
        printf(" | T_actual=%.2f%s\n",
               uart_usage_temperature_to_unit(temperature_c, config.unit),
               uart_usage_temperature_unit_symbol(config.unit));
    } else {
        printf(" | T_actual=N/A\n");
    }
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
    s_rgb1_state.limits[color].min_c = min_c;
    s_rgb1_state.limits[color].max_c = max_c;
    uart_usage_unlock();

    printf("\nOK: %c %.2f-%.2f C\n", color_to_letter(color), min_c, max_c);
}

static void set_intensity_from_command(uart_rgb_color_t color, const char *intensity_text)
{
    uint8_t percent = 0;

    if (!parse_intensity_percent(intensity_text, &percent)) {
        printf("\nERROR: usa INTENSITY_R 70, INTENSITY_G 50 o INTENSITY_B 30 con valores %u,%u,..%u.\n",
               (unsigned int)RGB1_INTENSITY_MIN_PCT,
               (unsigned int)RGB1_INTENSITY_STEP_PCT,
               (unsigned int)RGB1_INTENSITY_MAX_PCT);
        return;
    }

    uart_usage_lock();
    s_rgb1_state.intensity_percent[color] = percent;
    uart_usage_unlock();

    printf("\nOK: intensidad RGB1 %s (%c) = %u%% | PWM=%u\n",
           color_to_name(color),
           color_to_letter(color),
           (unsigned int)percent,
           (unsigned int)percent_to_pwm(percent));
}

static void set_temperature_interval_from_command(const char *interval_text)
{
    uint32_t interval_seconds = 0;

    if (!parse_interval_seconds(interval_text, &interval_seconds)) {
        printf("\nERROR: usa TEMP_INTERVAL 5 con valores %u..%u segundos.\n",
               (unsigned int)UART_TEMP_INTERVAL_MIN_S,
               (unsigned int)UART_TEMP_INTERVAL_MAX_S);
        return;
    }

    uart_usage_lock();
    s_temperature_print_config.interval_seconds = interval_seconds;
    uart_usage_unlock();

    printf("\nOK: temperatura periodica cada %lu s\n",
           (unsigned long)interval_seconds);
}

static void set_temperature_unit_from_command(const char *unit_text)
{
    uart_usage_temperature_unit_t unit = UART_USAGE_TEMPERATURE_UNIT_CELSIUS;

    if (!parse_temperature_unit(unit_text, &unit)) {
        printf("\nERROR: usa TEMP_UNIT C, TEMP_UNIT F o TEMP_UNIT K.\n");
        return;
    }

    uart_usage_lock();
    s_temperature_print_config.unit = unit;
    uart_usage_unlock();

    printf("\nOK: unidad de temperatura = %s (%s)\n",
           uart_usage_temperature_unit_symbol(unit),
           temperature_unit_name(unit));
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

    if (strcmp(upper_command, "TEMP_CONFIG") == 0 ||
        strcmp(upper_command, "TEMP CONFIG") == 0) {
        print_temperature_print_config();
        return;
    }

    if (command_matches_prefix(upper_command, "INTENSITY_R")) {
        set_intensity_from_command(UART_RGB_RED, &command[strlen("INTENSITY_R")]);
        return;
    }

    if (command_matches_prefix(upper_command, "INTENSITY_G")) {
        set_intensity_from_command(UART_RGB_GREEN, &command[strlen("INTENSITY_G")]);
        return;
    }

    if (command_matches_prefix(upper_command, "INTENSITY_B")) {
        set_intensity_from_command(UART_RGB_BLUE, &command[strlen("INTENSITY_B")]);
        return;
    }

    if (command_matches_prefix(upper_command, "TEMP_INTERVAL")) {
        set_temperature_interval_from_command(&command[strlen("TEMP_INTERVAL")]);
        return;
    }

    if (command_matches_prefix(upper_command, "TEMP INTERVAL")) {
        set_temperature_interval_from_command(&command[strlen("TEMP INTERVAL")]);
        return;
    }

    if (command_matches_prefix(upper_command, "INTERVAL")) {
        set_temperature_interval_from_command(&command[strlen("INTERVAL")]);
        return;
    }

    if (command_matches_prefix(upper_command, "TEMP_UNIT")) {
        set_temperature_unit_from_command(&command[strlen("TEMP_UNIT")]);
        return;
    }

    if (command_matches_prefix(upper_command, "TEMP UNIT")) {
        set_temperature_unit_from_command(&command[strlen("TEMP UNIT")]);
        return;
    }

    if (command_matches_prefix(upper_command, "UNIT")) {
        set_temperature_unit_from_command(&command[strlen("UNIT")]);
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

    if (strcmp(upper_command, "THRESHOLD") == 0) {
        print_threshold();
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

uart_usage_temperature_print_config_t uart_usage_get_temperature_print_config(void)
{
    uart_usage_temperature_print_config_t config;

    uart_usage_lock();
    config = s_temperature_print_config;
    uart_usage_unlock();

    return config;
}

uart_usage_temperature_unit_t uart_usage_cycle_temperature_unit(void)
{
    uart_usage_temperature_unit_t unit;

    uart_usage_lock();
    unit = next_temperature_unit(s_temperature_print_config.unit);
    s_temperature_print_config.unit = unit;
    uart_usage_unlock();

    return unit;
}

void uart_usage_get_rgb1_levels(float temperature_c,
                                uint8_t *out_red,
                                uint8_t *out_green,
                                uint8_t *out_blue)
{
    uart_usage_rgb1_state_t rgb1_state;

    uart_usage_lock();
    rgb1_state = s_rgb1_state;
    uart_usage_unlock();

    if (out_red != NULL) {
        *out_red = is_temperature_inside_limit(temperature_c, &rgb1_state.limits[UART_RGB_RED]) ?
                   percent_to_pwm(rgb1_state.intensity_percent[UART_RGB_RED]) : 0;
    }
    if (out_green != NULL) {
        *out_green = is_temperature_inside_limit(temperature_c, &rgb1_state.limits[UART_RGB_GREEN]) ?
                     percent_to_pwm(rgb1_state.intensity_percent[UART_RGB_GREEN]) : 0;
    }
    if (out_blue != NULL) {
        *out_blue = is_temperature_inside_limit(temperature_c, &rgb1_state.limits[UART_RGB_BLUE]) ?
                    percent_to_pwm(rgb1_state.intensity_percent[UART_RGB_BLUE]) : 0;
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

/*
 * uart_usage_update_integrated_led_state()
 * =========================================
 * Actualiza el umbral de temperatura y el estado del LED integrado.
 * Ambos valores se protegen por el mutex existente s_state_mutex.
 *
 * Parámetros:
 *   - temperature_threshold: valor [0, 100] que representa el umbral en °C
 *   - led_is_on: true si el LED debe estar encendido, false si apagado
 *
 * Esta función es llamada desde app_tasks.c por la tarea que controla el LED.
 */
void uart_usage_update_integrated_led_state(uint8_t temperature_threshold,
                                             bool led_is_on)
{
    uart_usage_lock();
    s_snapshot.temperature_threshold = temperature_threshold;
    s_snapshot.integrated_led_state = led_is_on;
    uart_usage_unlock();
}

/*
 * uart_usage_get_temperature_threshold()
 * ========================================
 * Lee de forma segura el umbral de temperatura actual [0, 100].
 * Retorna el valor almacenado en s_snapshot.temperature_threshold.
 */
uint8_t uart_usage_get_temperature_threshold(void)
{
    uint8_t threshold;
    uart_usage_lock();
    threshold = s_snapshot.temperature_threshold;
    uart_usage_unlock();
    return threshold;
}

/*
 * uart_usage_get_integrated_led_state()
 * =====================================
 * Lee de forma segura el estado actual del LED integrado.
 * Retorna true si está encendido, false si está apagado.
 */
bool uart_usage_get_integrated_led_state(void)
{
    bool state;
    uart_usage_lock();
    state = s_snapshot.integrated_led_state;
    uart_usage_unlock();
    return state;
}
