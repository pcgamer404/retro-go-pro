# Atari 7800: ProSystem (Greg Stanton), the libretro core
# (github.com/libretro/prosystem-libretro, commit 8a88014), GPL v2.
# Without the BupChip (bupchip_stub.c) and libretro.c; files through
# shim/streams/file_stream.h.
set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ". shim")
set(COMPONENT_REQUIRES "retro-go")
set(COMPONENT_ADD_LDFRAGMENTS "linker.lf")
register_component()
rg_setup_compile_options(-O2 -Wno-error=unused-function -Wno-unused-function -Wno-error=unused-variable -Wno-unused-variable -Wno-error=maybe-uninitialized -Wno-error=implicit-fallthrough -Wno-format -Wno-error=unused-value -Wno-unused-value)
