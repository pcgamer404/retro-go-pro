# Atari 5200: the Atari800 emulator's core (ANTIC, GTIA, POKEY, 6502; Petr
# Stehlik and the Atari800 team, GPL v2 or later) as cut down for MCUs by
# Jean-Marc Harvengt in MCUME (github.com/Jean-MarcHarvengt/MCUME,
# MCUME_esp32/esp5200 at 27f6b90). The platform calls of emuapi.h are
# main_a52.c's; the BIOS is read from the card (rom.h).
set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(-O2 -Wno-error=unused-function -Wno-unused-function -Wno-error=unused-variable -Wno-unused-variable -Wno-error=unused-but-set-variable -Wno-error=maybe-uninitialized -Wno-error=implicit-fallthrough -Wno-format -Wno-error=unused-value -Wno-error=pointer-sign -Wno-pointer-sign -Wno-error=int-conversion -Wno-error=return-type -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=parentheses -Wno-error=misleading-indentation -Wno-error=char-subscripts -Wno-error=address -Wno-error=dangling-else -Wno-error=unused-label -Wno-error=sequence-point -Wno-error=stringop-truncation -Wno-error=array-bounds)
