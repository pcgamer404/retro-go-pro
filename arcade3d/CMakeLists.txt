cmake_minimum_required(VERSION 3.5)
# Arcade 3D: standalone game port (OutRun-style racer), no ROM needed.
set(COMPONENTS "main retro-go arcade3d")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(arcade3d)
