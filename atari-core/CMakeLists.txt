cmake_minimum_required(VERSION 3.5)
# atari-core: Lynx (handy), 2600 (StellaDS), 5200 (atari5200), 7800 (prosystem), one app partition.
set(COMPONENTS "main retro-go handy StellaDS atari5200 prosystem")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(atari-core)
