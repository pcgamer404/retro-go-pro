set(COMPONENT_SRCDIRS "." "cpu")
set(COMPONENT_ADD_INCLUDEDIRS "." "cpu")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(
    -Wno-error=stringop-truncation
    -Wno-error=format-overflow
    -Wno-implicit-fallthrough
    -Wno-format-overflow
    -Wno-stringop-truncation
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -O3
)
file(GLOB_RECURSE c_sources "cpu/*.c" "cpu/*.cpp" "*.c" "*.cpp")
set_source_files_properties(${c_sources} PROPERTIES COMPILE_FLAGS "")
