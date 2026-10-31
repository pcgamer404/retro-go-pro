set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
register_component()
# RG Ports (built-in port launcher). On by default; build with -DRG_ENABLE_PORTS=0 to remove it.
if(NOT DEFINED RG_ENABLE_PORTS)
    set(RG_ENABLE_PORTS 1)
endif()
rg_setup_compile_options(-O2 -Wno-error=format -Wno-error=char-subscripts -DRG_ENABLE_PORTS=${RG_ENABLE_PORTS})

# add_custom_command(OUTPUT images.c
#                    COMMAND python ${COMPONENT_DIR}/gen_images.py
#                    VERBATIM)
# add_custom_target(images DEPENDS images.c)
# add_dependencies(${COMPONENT_LIB} images)
