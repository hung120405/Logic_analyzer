idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_server
        esp_netif
        nvs_flash
        driver
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        esp_timer
        spiffs
        vfs
        spi_flash
)

# Đóng gói thư mục data/ vào SPIFFS image và tự flash khi `idf.py flash`
spiffs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)
