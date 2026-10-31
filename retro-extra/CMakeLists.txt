cmake_minimum_required(VERSION 3.5)
# retro-extra: Neo Geo Pocket / Color (RACE) only.
set(COMPONENTS "main retro-go race")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(retro-extra)
