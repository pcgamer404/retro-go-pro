cmake_minimum_required(VERSION 3.5)
set(COMPONENTS "main retro-go gwenesis")
set(RG_ENABLE_NETWORKING 0)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
else()
    message(FATAL_ERROR "Could not find base.cmake next to this core or two levels above it")
endif()
# A Genesis frame changes on every line, every frame, so Retro-Go's per-line
# checksum can never skip a write here.  Turning partial updates off drops the
# hashing from the display path (rg_display.c) without changing what reaches
# the panel.  Build-property scope, so it only affects this app.
idf_build_set_property(COMPILE_DEFINITIONS "-DRG_SCREEN_PARTIAL_UPDATES=0" APPEND)
project(gwenesis)
