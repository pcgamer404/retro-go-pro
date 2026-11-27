cmake_minimum_required(VERSION 3.5)

# Extra Kconfig defaults for this app (PSRAM .bss, caches). Merged with whatever
# the retro-go base/target already sets; see sdkconfig.defaults.
set(SOTN_SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(SDKCONFIG_DEFAULTS)
    list(APPEND SDKCONFIG_DEFAULTS "${SOTN_SDKCONFIG}")
else()
    set(SDKCONFIG_DEFAULTS "${SOTN_SDKCONFIG}")
endif()

set(COMPONENTS "main retro-go sotn")
set(RG_ENABLE_NETWORKING 0)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../base.cmake")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../../base.cmake")
else()
    message(FATAL_ERROR "Could not find base.cmake next to this core or two levels above it")
endif()
project(sotn-go)
