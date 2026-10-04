set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
rg_setup_compile_options()
# main.c includes the core's headers: the same switches as gbsp-libretro (they change
# the size of gbsp_memory_t and which symbols exist). GBAJIT=0 builds the interpreter alone.
if(NOT "$ENV{GBAJIT}" STREQUAL "0")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HAVE_DYNAREC XTENSA_ARCH SMALL_TRANSLATION_CACHE)
endif()
