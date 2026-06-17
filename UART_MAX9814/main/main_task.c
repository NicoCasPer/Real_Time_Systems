#include "lvgl.h"
#include "esp_log.h"
#include "max9814_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#define SAMPLE_RATE 48000
#define BUFFER_SIZE 1024
#define LVGL_DISPLAY_HOR_RES 240
#define LVGL_DISPLAY_VER_RES 320

lv_obj_t *audio_chart;

static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

// NOTE: lv_tick_inc must be called periodically (e.g. from a timer or a task).

void lvgl_init() {
    // Initialize LVGL
    lv_init();

    // Initialize the display (using your existing driver, e.g., ili9341)
    static lv_color_t buf[LVGL_DISPLAY_HOR_RES * 10];
    lv_display_t *disp = lv_display_create(LVGL_DISPLAY_HOR_RES, LVGL_DISPLAY_VER_RES);
    lv_display_set_default(disp);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_disp_flush);

    // Create a chart to display audio data
    lv_obj_t *scr = lv_scr_act();
    audio_chart = lv_chart_create(scr);

    // Set the size of chart.
    lv_obj_set_size(audio_chart, lv_pct(100), lv_pct(100));
    lv_obj_align(audio_chart, LV_ALIGN_CENTER, 0, -12);

    // Add a data series
    lv_chart_series_t * ser = lv_chart_add_series(audio_chart, (lv_color_t){ .red = 255, .green = 0, .blue = 0 }, LV_CHART_AXIS_PRIMARY_Y);
    (void)ser;

    // Set update mode to manual refresh
    lv_chart_set_update_mode(audio_chart, LV_CHART_UPDATE_MODE_SHIFT);
}

void app_main() {
    esp_err_t err;

    // Initialize MAX9814 ADC device
    if ((err = max9814_init()) != ESP_OK) {
       printf("MAX9814 initialization failed with error %d\n", (int)err);
       return;
    }

    lvgl_init();

    // Create the audio task that reads I2S data
    xTaskCreate(max9814_task, "max9814_task", 2048, NULL, 5, NULL);
}