cmake_minimum_required(VERSION 3.5)

set(COMPONENTS "main retro-go openjazz")
set(RG_ENABLE_NETWORKING 0)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
else()
    message(FATAL_ERROR "Could not find base.cmake next to this core or two levels above it")
endif()

# OpenJazz needs C++ exceptions. `rg_tool.py clean` deletes sdkconfig, so also ask for them
# here: this keeps the target's defaults (rg_tool sets SDKCONFIG_DEFAULTS) and adds ours.
set(SDKCONFIG_DEFAULTS "$ENV{SDKCONFIG_DEFAULTS};${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

# An existing sdkconfig wins over the defaults, so drop one that has exceptions off.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/sdkconfig" _oj_sdkconfig)
    if(NOT _oj_sdkconfig MATCHES "CONFIG_COMPILER_CXX_EXCEPTIONS=y")
        message(WARNING "openjazz/sdkconfig has C++ exceptions off: regenerating it")
        file(REMOVE "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    endif()
endif()

# Register the engine component directory (not this app root).
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/openjazz")
project(openjazz)
