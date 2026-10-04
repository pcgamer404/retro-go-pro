cmake_minimum_required(VERSION 3.5)
# Cores whose static tables would eat the internal RAM of every retro-core
# emulator (RACE 26 KB) live in their own app partition.
set(COMPONENTS "main retro-go race")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(ngp-go)
