set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS "." "sdl" "engine/Source")
set(COMPONENT_REQUIRES retro-go driver fatfs)
set(COMPONENT_ADD_LDFRAGMENTS "linker.lf")

register_component()

# Configure DevilutionX options for embedded Retro-Go platform
set(TARGET_PLATFORM "retrogo" CACHE STRING "" FORCE)
set(RETROGO_LIB ON CACHE BOOL "" FORCE)
set(USE_SDL1 ON CACHE BOOL "" FORCE)
set(NONET ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_ASSETS_MPQ OFF CACHE BOOL "" FORCE)
set(CPACK OFF CACHE BOOL "" FORCE)
set(DIST ON CACHE BOOL "" FORCE)
set(NOSOUND ON CACHE BOOL "" FORCE)
set(NOEXIT ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

add_definitions(-DPLATFORM_ESP32=1 -DNOEXIT=1)

add_subdirectory(engine)

get_target_property(_linked_objects libdevilutionx LINKED_OBJECTS)
message(STATUS "Retro-Go: libdevilutionx LINKED_OBJECTS: ${_linked_objects}")
target_sources(${COMPONENT_LIB} PRIVATE ${_linked_objects})

target_link_libraries(${COMPONENT_LIB} PUBLIC libdevilutionx SDL)
rg_setup_compile_options(-Wno-error -O2)
