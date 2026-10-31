# Quake II model/skin image, built from the retail PAK files:
#   python tools/mkassets.py sd/baseq2 main/assets.bin
# It is linked into flash rodata (16-byte aligned) and used in place by the
# engine, so the models take no PSRAM. Regenerate it when the PAK files change.
set(Q2_ASSETS_FILE "${CMAKE_CURRENT_LIST_DIR}/assets.bin")
if(NOT EXISTS "${Q2_ASSETS_FILE}")
    message(FATAL_ERROR
        "Missing ${Q2_ASSETS_FILE}\n"
        "Build it with: python tools/mkassets.py sd/baseq2 main/assets.bin")
endif()
file(TO_CMAKE_PATH "${Q2_ASSETS_FILE}" Q2_ASSETS_FILE)
configure_file("${CMAKE_CURRENT_LIST_DIR}/assets.S.in"
               "${CMAKE_CURRENT_BINARY_DIR}/q2_assets.S" @ONLY)

idf_component_register(
    SRCS "rg_core_assets_generated.c" "main.c" "${CMAKE_CURRENT_BINARY_DIR}/q2_assets.S"
    INCLUDE_DIRS "."
    REQUIRES retro-go quake2
)

rg_setup_compile_options()

target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=rg_system_exit")
