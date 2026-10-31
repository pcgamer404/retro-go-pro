idf_component_register(
    SRCS "rg_core_assets_generated.c" "main.c" "rom_store.c" "romart.c"
    INCLUDE_DIRS "."
    REQUIRES retro-go esp_http_client json mbedtls esp-tls
)