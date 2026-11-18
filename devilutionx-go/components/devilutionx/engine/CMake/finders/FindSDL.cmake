# FindSDL.cmake for Retro-Go embedded SDL 1.2 port

set(SDL_FOUND TRUE)
set(SDL_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/../sdl")
set(SDL_LIBRARY SDL)

if(NOT TARGET SDL)
  file(GLOB SDL_SRCS "${PROJECT_SOURCE_DIR}/../sdl/*.c")
  list(FILTER SDL_SRCS EXCLUDE REGEX "spi_lcd\\.c$")
  add_library(SDL STATIC ${SDL_SRCS})
  target_include_directories(SDL PUBLIC "${PROJECT_SOURCE_DIR}/../sdl")
  target_link_libraries(SDL PUBLIC __idf_retro-go)
  target_compile_definitions(SDL PUBLIC PLATFORM_ESP32=1)
  target_compile_options(SDL PRIVATE -Wno-error -O2)
endif()
