cmake_minimum_required(VERSION 3.5)
set(COMPONENTS "main retro-go app_trace bootloader esptool_py")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)

# The build driver passes the board's sdkconfig in the *environment*, and
# tools/cmake/project.cmake gives that precedence over this project's
# sdkconfig.defaults. Promote the environment value to the CMake variable
# project.cmake prefers (see project.cmake: "if(SDKCONFIG_DEFAULTS)") and append
# our own defaults, so they are applied after the board's.
#
# This matters because a generated sdkconfig always wins over any defaults file:
# settings that are only written into ./sdkconfig are silently lost as soon as
# the project is rebuilt from scratch ("release", "clean"). sdkconfig.defaults is
# therefore the authority, and ./sdkconfig is a disposable result.
set(_rg_env_defaults "$ENV{SDKCONFIG_DEFAULTS}")
if(NOT DEFINED SDKCONFIG_DEFAULTS AND _rg_env_defaults)
    set(SDKCONFIG_DEFAULTS "${_rg_env_defaults}")
endif()
if(DEFINED SDKCONFIG_DEFAULTS)
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
project(j2me-go)
