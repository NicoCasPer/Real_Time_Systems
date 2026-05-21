idf_component_register(SRCS "main.c"
                            "app_adc.c"
                            "app_tasks.c"
                            "mode_button.c"
                            "rgb_led.c"
                            "UART_usage.c"
                            "ADC_calibrate_full.c"
                    PRIV_REQUIRES esp_adc esp_driver_gpio esp_driver_ledc esp_driver_uart
                    INCLUDE_DIRS ".")
