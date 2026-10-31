cmake_minimum_required(VERSION 3.5)
# retro-home: home computers (MSX = fmsx, C64 = frodo). Future: ZX Spectrum etc.
set(COMPONENTS "main retro-go fmsx frodo")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(retro-home)
