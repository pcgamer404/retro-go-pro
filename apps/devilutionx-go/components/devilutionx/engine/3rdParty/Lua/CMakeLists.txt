file(GLOB LUA_CORE_SRCS "src/*.c")
list(FILTER LUA_CORE_SRCS EXCLUDE REGEX "(lua|luac)\\.c$")

add_library(lua_static STATIC ${LUA_CORE_SRCS})
target_include_directories(lua_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(lua_static PUBLIC LUA_COMPAT_5_3)
target_compile_options(lua_static PRIVATE -Wno-error -O2)
add_library(Lua::Lua ALIAS lua_static)

set(LUA_FOUND TRUE CACHE BOOL "" FORCE)
set(LUA_LIBRARIES lua_static CACHE STRING "" FORCE)
set(LUA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(LUA_VERSION_STRING "5.4.7" CACHE STRING "" FORCE)
