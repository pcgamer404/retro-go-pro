idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES retro-go fatfs newlib SDL zlib keen)

target_compile_options(${COMPONENT_LIB} PRIVATE -D${RG_BUILD_TARGET}=1)
