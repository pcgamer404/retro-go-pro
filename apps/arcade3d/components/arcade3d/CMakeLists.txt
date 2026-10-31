set(COMPONENT_SRCDIRS "src shim")
set(COMPONENT_ADD_INCLUDEDIRS "src shim")
set(COMPONENT_REQUIRES "retro-go")
register_component()
# "Arcade 3D": davidmonterocrespo24/esp32s3-arcade-3d (an OutRun-style racing
# game written for Arduino + TFT_eSPI), run on retro-go through shim/
rg_setup_compile_options(-O2 -Wno-unused-variable -Wno-unused-function -Wno-sign-compare -Wno-narrowing -Wno-error=maybe-uninitialized -Wno-format)
