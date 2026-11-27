set(COMPONENT_SRCS
    "main.c"
    "rg_core_assets_generated.c"
)
set(COMPONENT_ADD_INCLUDEDIRS "." "../components/spectemu")
set(COMPONENT_REQUIRES "retro-go")

register_component()
rg_setup_compile_options(
    -O3
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-implicit-function-declaration
    -Wno-char-subscripts
)
