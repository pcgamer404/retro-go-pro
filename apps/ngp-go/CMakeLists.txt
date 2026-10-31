cmake_minimum_required(VERSION 3.5)
# Cores whose static tables would eat the internal RAM of every retro-core
# emulator (RACE 26 KB) live in their own app partition.
set(COMPONENTS "main retro-go race")
set(RG_ENABLE_NETWORKING 0)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
else()
    message(FATAL_ERROR "Could not find base.cmake next to this core or two levels above it")
endif()
project(ngp-go)
