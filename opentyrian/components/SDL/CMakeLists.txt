set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ". ../OpenTyrian")
set(COMPONENT_REQUIRES "retro-go")
register_component()

# SDL_input.c is an unused (stubbed) SDL1 joystick-GUID compat shim; a few
# of its dead-code paths return an uninitialized struct, which -Wall turns
# into a hard error under this project's default flags.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=uninitialized
)

rg_setup_compile_options()
