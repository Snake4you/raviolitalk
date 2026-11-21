FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.c
)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES
        esp_wifi
        esp_netif
        esp_http_server
        esp_http_client
        nvs_flash
        driver
        lwip
        mbedtls
        app_update
        mdns
)
