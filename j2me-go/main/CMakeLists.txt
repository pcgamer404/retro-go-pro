# Nano Java Retro-Go application component.
# Use legacy ESP-IDF component variables; add_compile_definitions() is unavailable
# while ESP-IDF 5.3.5 evaluates component requirements.

set(COMPONENT_SRCDIRS
    "nanojava/src/vm"
    "nanojava/src/util"
    "nanojava/src/midp"
    "nanojava/src/audio"
    "nanojava/platform/retrogo"
)
set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "nanojava/src/vm"
    "nanojava/src/util"
    "nanojava/src/midp"
    "nanojava/src/audio"
    "nanojava/src/pal"
)
set(COMPONENT_REQUIRES "retro-go")
set(COMPONENT_PRIV_REQUIRES "freertos esp_timer")
set(COMPONENT_EMBED_FILES "nanojava_classlib.jar")

register_component()
rg_setup_compile_options(
    -O3
    -Wno-error
    -Wno-format-truncation -Wno-stringop-truncation -Wno-format-overflow
)
