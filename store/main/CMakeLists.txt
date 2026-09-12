idf_component_register(
    SRCS "main.c" "rom_store.c"
    INCLUDE_DIRS "."
    REQUIRES retro-go esp_http_client json mbedtls esp-tls
)