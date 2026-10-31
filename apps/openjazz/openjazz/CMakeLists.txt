# OpenJazz engine for Retro-Go (ESP32-S3).
# porting/       Retro-Go glue: display, input, SDL compatibility layer (video, events, audio)
# upstream-openjazz/  OpenJazz sources (GPL-2.0); only a few lines changed, all under OJ_RETROGO
cmake_minimum_required(VERSION 3.5)

set(COMPONENT_REQUIRES retro-go)
set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "porting"
    "porting/sdl_compat"
    "upstream-openjazz/src"
    "upstream-openjazz/ext/argparse"
    "upstream-openjazz/ext/miniz"
    "upstream-openjazz/ext/stb"
    "upstream-openjazz/ext/xmp"
)

file(GLOB COMPONENT_SRCS
    "porting/*.c"
    "porting/*.cpp"
    "porting/sdl_compat/*.c"
    "upstream-openjazz/src/*.cpp"
    "upstream-openjazz/src/game/*.cpp"
    "upstream-openjazz/src/io/*.cpp"
    "upstream-openjazz/src/io/gfx/*.cpp"
    "upstream-openjazz/src/level/*.cpp"
    "upstream-openjazz/src/menu/*.cpp"
    "upstream-openjazz/src/player/*.cpp"
    "upstream-openjazz/src/jj1/*.cpp"
    "upstream-openjazz/src/jj1/bonuslevel/*.cpp"
    "upstream-openjazz/src/jj1/level/*.cpp"
    "upstream-openjazz/src/jj1/level/event/*.cpp"
    "upstream-openjazz/src/jj1/planet/*.cpp"
    "upstream-openjazz/src/jj1/save/*.cpp"
    "upstream-openjazz/src/jj1/scene/*.cpp"
    "upstream-openjazz/src/platforms/platform_interface.cpp"
    "upstream-openjazz/ext/argparse/argparse.c"
    "upstream-openjazz/ext/miniz/miniz.c"
    "upstream-openjazz/ext/stb/stb_rect_pack.c"
    "upstream-openjazz/ext/xmp/src/*.c"
    "upstream-openjazz/ext/xmp/src/loaders/*.c"
)

register_component()

# OpenJazz reports file errors with integer exceptions (throw/catch), so the
# component is built with -fexceptions (sdkconfig also enables C++ exceptions).
# Audio: 22.05 kHz stereo (must match config.sampleRate in main/main.cpp), cheapest
# libxmp settings (nearest-neighbour mixing, no DSP effects).
rg_setup_compile_options(
    -DOJ_RETROGO -DNO_RESIZE -fexceptions
    -DSOUND_FREQ=22050 -DSOUND_SAMPLES=1024 -DMUSIC_SETTINGS=0
    -DLIBXMP_CORE_PLAYER -DLIBXMP_CORE_DISABLE_IT -DLIBXMP_STATIC -DHAVE_POWF
)

# Third-party code: don't let a newer compiler's warnings break the build.
file(GLOB XMP_SRCS "upstream-openjazz/ext/xmp/src/*.c" "upstream-openjazz/ext/xmp/src/loaders/*.c")
set_source_files_properties(${XMP_SRCS} "upstream-openjazz/src/io/sound.cpp"
    PROPERTIES COMPILE_FLAGS "-Wno-error")

# The music mixer is the hot loop: build it for speed even if the project default is -Os.
set_source_files_properties(
    "upstream-openjazz/ext/xmp/src/mix_all.c"
    "upstream-openjazz/ext/xmp/src/mixer.c"
    "upstream-openjazz/ext/xmp/src/player.c"
    "upstream-openjazz/ext/xmp/src/virtual.c"
    PROPERTIES COMPILE_FLAGS "-O2 -Wno-error")
