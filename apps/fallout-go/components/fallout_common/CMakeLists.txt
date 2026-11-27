set(COMPONENT_SRCDIRS
    "sdl"
    "third_party/fpattern/src"
    "third_party/adecode/src"
    "third_party/zlib"
)

# Force cmake re-scan for zlib gz*.c files

set(COMPONENT_ADD_INCLUDEDIRS
    "sdl"
    "third_party/fpattern/include"
    "third_party/adecode/include"
    "third_party/zlib"
)

set(COMPONENT_REQUIRES "retro-go" "esp_timer")

register_component()

rg_setup_compile_options(-Wno-error -O2)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    PLATFORM_ESP32=1
    HAVE_MEMCPY=1
    __unix=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -w
)
