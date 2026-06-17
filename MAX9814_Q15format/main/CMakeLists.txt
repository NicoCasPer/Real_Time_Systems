idf_component_register(SRCS "max9814_driver.c" "main_task.c"
                    REQUIRES esp_adc lvgl driver esp_timer
                    INCLUDE_DIRS ".")
