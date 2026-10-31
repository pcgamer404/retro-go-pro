cmake_minimum_required(VERSION 3.5)
# xjit: Xtensa JIT core (emitter, blocks, executable PSRAM); esp_mm: esp_mmu_map / esp_cache_msync
set(COMPONENTS "main retro-go gbsp-libretro app_trace bootloader esptool_py")
if(NOT "$ENV{GBAJIT}" STREQUAL "0")   # dynarec by default, GBAJIT=0: interpreter only
    list(APPEND COMPONENTS xjit esp_mm)
endif()
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(gbsp)
