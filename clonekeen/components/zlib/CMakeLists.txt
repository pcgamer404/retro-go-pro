idf_component_register(
    SRCS
        "adler32.c"
        "compress.c"
        "crc32.c"
        "deflate.c"
        "gzclose.c"
        "gzlib.c"
        "gzread.c"
        "gzwrite.c"
        "infback.c"
        "inffast.c"
        "inflate.c"
        "inftrees.c"
        "trees.c"
        "uncompr.c"
        "zutil.c"
    INCLUDE_DIRS "."
)

component_compile_options(-Wno-error -Wno-implicit-function-declaration)
