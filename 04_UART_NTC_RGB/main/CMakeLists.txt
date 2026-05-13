idf_component_register(SRCS "UART_usage.c" "ledc_basic_example_main.c" "ADC_calibrate.c"
                    PRIV_REQUIRES esp_adc esp_driver_gpio esp_driver_ledc esp_driver_uart
                    INCLUDE_DIRS ".")
