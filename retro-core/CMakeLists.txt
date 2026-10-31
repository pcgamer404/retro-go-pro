cmake_minimum_required(VERSION 3.5)
# retro-core: NES (fceumm + nofrendo), GB/GBC, PCE, SMS/GG/SG-1000/Coleco, Game & Watch, SNES.
# Lynx moved to atari-core.
set(COMPONENTS "main retro-go nofrendo fceumm gnuboy pce-go gw-emulator smsplus snes9x")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
# SNES renderer counters (optional): `SNES_PROF=1 python rg_tool.py ... build retro-core`
if(DEFINED ENV{SNES_PROF} AND NOT "$ENV{SNES_PROF}" STREQUAL "0")
    idf_build_set_property(COMPILE_DEFINITIONS "-DSNES_PROF=1" APPEND)
    message(STATUS "SNES_PROF=1: snes9x renderer profiling enabled")
endif()
project(retro-core)
