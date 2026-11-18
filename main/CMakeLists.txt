idf_component_register(SRCS "main.cpp" "source.cpp" "audio.cpp" "display.cpp" "services.cpp"
    INCLUDE_DIRS "." "../include" "../third_party/minimp3"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2s esp_driver_uart
             esp_timer esp_wifi esp_event esp_netif nvs_flash esp_http_client fatfs sdmmc)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
