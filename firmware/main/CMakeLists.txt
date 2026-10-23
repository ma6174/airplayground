cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "app_main.c" "wifi_net.c" "app_identity.c" "pin_server.c" "pin_fetch.c"
    INCLUDE_DIRS "."
    REQUIRES fairplay hap airplay media nvs_flash esp_wifi esp_netif esp_event spi_flash esp_http_server
)
