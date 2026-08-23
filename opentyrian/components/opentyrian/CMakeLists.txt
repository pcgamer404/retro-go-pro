set(COMPONENT_SRCS
    animlib.c
    arg_parse.c
    backgrnd.c
    config.c
    config_file.c
    destruct.c
    editship.c
    episodes.c
    file.c
    font.c
    fonthand.c
    game_menu.c
    helptext.c
    joystick.c
    jukebox.c
    keyboard.c
    lds_play.c
    loudness.c
    lvllib.c
    lvlmast.c
    mainint.c
    menus.c
    mouse.c
    mtrand.c
    musmast.c
    network.c
    nortsong.c
    nortvars.c
    opentyr.c
    opl.c
    palette.c
    params.c
    pcxload.c
    pcxmast.c
    picload.c
    player.c
    scroller.c
    setup.c
    shots.c
    sizebuf.c
    sndmast.c
    sprite.c
    starlib.c
    std_support.c
    tyrian2.c
    varz.c
    vga256d.c
    vga_palette.c
    video.c
    video_scale.c
    video_scale_hqNx.c
    xmas.c
)

set(COMPONENT_ADD_INCLUDEDIRS ". ../SDL")
set(COMPONENT_REQUIRES "retro-go SDL")

register_component()

# This is old, warning-heavy C89/C99 DOS-port code; match rott's approach
# of silencing the noise rather than touching 50 files.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-address
    -Wno-misleading-indentation
    -Wno-format-overflow
    -Wno-char-subscripts
    -Wno-missing-field-initializers
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-pointer-sign
    -Wno-maybe-uninitialized
    -Wno-implicit-fallthrough
    -Wno-parentheses
    -Wno-sign-compare
    -Wno-int-conversion
    -Wno-incompatible-pointer-types
    -Wno-return-type
    -Wno-unused-value
    -Wno-stringop-overflow
    -Wno-format-truncation
    -Wno-error=dangling-pointer
    -Wno-error=array-bounds
    -Wno-error=format
    -Wno-error=restrict
)

rg_setup_compile_options(
    -O2
    -DUSE_SDL=1
)
