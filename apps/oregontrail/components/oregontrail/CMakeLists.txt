file(GLOB_RECURSE OREGONTRAIL_SRCS "*.cpp" "*.c")
set(COMPONENT_SRCS ${OREGONTRAIL_SRCS})
set(COMPONENT_ADD_INCLUDEDIRS ". compat game hw ui screens")
set(COMPONENT_REQUIRES retro-go)
register_component()
rg_setup_compile_options(
    -Wno-error
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-format
    -O2
    -include compat/rg_c_api_compat.h
)
