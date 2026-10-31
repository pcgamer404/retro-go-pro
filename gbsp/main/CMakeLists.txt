set(COMPONENT_SRCS "main.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES retro-go gbsp-libretro)
if(NOT "$ENV{GBAJIT}" STREQUAL "0")   # dynarec by default, GBAJIT=0: interpreter only
    list(APPEND COMPONENT_REQUIRES xjit)
endif()
register_component()

# HAVE_DYNAREC/XTENSA_ARCH/SMALL_TRANSLATION_CACHE must match gbsp-libretro:
# gbsp_memory_t changes size with them.
rg_setup_compile_options(
    -O3
)
if(NOT "$ENV{GBAJIT}" STREQUAL "0")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        HAVE_DYNAREC XTENSA_ARCH SMALL_TRANSLATION_CACHE)
endif()
