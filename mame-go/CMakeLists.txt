cmake_minimum_required(VERSION 3.5)
# Arcade: MAME 0.37b5 (mame2000-libretro subset), non-commercial license, see
# components/mame2000/README_MAMEGO.md
set(COMPONENTS "main retro-go mame2000")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
# `NEOPROF=1 python rg_tool.py ... build mame-go`: ms per frame of the 68000,
# Z80, YM2610, video and mixer, printed every 60 frames (mamego_prof.c)
# Neo Geo sprites are drawn on core 0: on core 1 they measured the same fps
# (43 in Metal Slug 2), the 68000 slowing down from the memory contention
# (11.8 -> 14 ms). `NEODEFER=1` builds the core-1 renderer for experiments.
if(NOT DEFINED ENV{NEODEFER} OR NOT "$ENV{NEODEFER}" STREQUAL "1")
    idf_build_set_property(COMPILE_DEFINITIONS "-DNEO_NO_DEFER=1" APPEND)
endif()
if(DEFINED ENV{NEOPROF} AND NOT "$ENV{NEOPROF}" STREQUAL "0")
    idf_build_set_property(COMPILE_DEFINITIONS "-DNEOPROF=1" APPEND)
endif()
project(mame-go)
