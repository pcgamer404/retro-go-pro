set(COMPONENT_SRCS "spectemu_core.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES "retro-go")

register_component()
rg_setup_compile_options(
    -O3
    -DHAVE_CONFIG_H
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-implicit-function-declaration
    -Wno-char-subscripts
    -Wno-parentheses
)
