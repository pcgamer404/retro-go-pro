# The game sources are compiled from the Vanilla-Conquer tree. The platform layer
# (video, input, sound, paths) is implemented by the cnc_*.cpp files here and
# replaces the SDL2 / OpenAL backends.
set(VC_DIR ${CMAKE_CURRENT_LIST_DIR}/Vanilla-Conquer)
set(VC_COMMON ${VC_DIR}/common)
set(VC_TD ${VC_DIR}/tiberiandawn)
set(VC_RA ${VC_DIR}/redalert)
# Game to build: td (Tiberian Dawn) or ra (Red Alert), e.g. cmake -DCNC_GAME=ra -B build_ra
set(CNC_GAME td CACHE STRING "Game to build: td or ra")

set(VC_COMMON_SRC
    _diptabl.cpp alloc.cpp auduncmp.cpp b64pipe.cpp b64straw.cpp base64.cpp bfiofile.cpp blowfish.cpp
    blowpipe.cpp blwstraw.cpp buff.cpp buffer.cpp buffglbl.cpp ccfile.cpp cdfile.cpp cliprect.cpp combuf.cpp
    control.cpp crc.cpp crew.cpp cstraw.cpp debugstring.cpp delay.cpp dipthong.cpp drawbuff.cpp drawline.cpp
    drawmisc.cpp face.cpp fading.cpp field.cpp file.cpp filepcx.cpp fixed.cpp font.cpp gadget.cpp getshape.cpp
    graphicsviewport.cpp hsv.cpp iff.cpp ini.cpp int.cpp internet.cpp irandom.cpp keybuff.cpp keyframe.cpp
    lcw.cpp lcwpipe.cpp lcwstraw.cpp linear.cpp link.cpp load.cpp memrev.cpp misc.cpp mixfile.cpp mp.cpp
    newdel.cpp packet.cpp palette.cpp palettec.cpp paths.cpp pipe.cpp pk.cpp pkpipe.cpp pkstraw.cpp
    ramfile.cpp random.cpp rawfile.cpp readline.cpp rect.cpp rgb.cpp rndstraw.cpp settings.cpp sha.cpp
    shape.cpp shapipe.cpp shastraw.cpp soscodec.cpp stamp.cpp straw.cpp timer.cpp timerdwn.cpp tobuff.cpp
    toggle.cpp vector.cpp winasm.cpp windows.cpp winhide.cpp winstub.cpp wsa.cpp xordelta.cpp xpipe.cpp
    xstraw.cpp file_posix.cpp
    # "vanilla" (executable) part
    framelimit.cpp gbuffer.cpp interpal.cpp unvqbuff.cpp vqaconfig.cpp vqadrawer.cpp vqaloader.cpp
    vqapalette.cpp vqatask.cpp vqaver.cpp wwkeyboard.cpp wwmouse.cpp soundio_common.cpp
)
list(TRANSFORM VC_COMMON_SRC PREPEND ${VC_COMMON}/)
# movie sound (replaces vqaaudio_openal.cpp), compiled as game code
list(APPEND VC_COMMON_SRC ${CMAKE_CURRENT_LIST_DIR}/cnc_vqaaudio.cpp)

set(VC_TD_SRC
    aadata.cpp abstract.cpp adata.cpp aircraft.cpp anim.cpp audio.cpp base.cpp bbdata.cpp bdata.cpp
    building.cpp bullet.cpp cargo.cpp ccini.cpp cdata.cpp cell.cpp checkbox.cpp cheklist.cpp colrlist.cpp
    combat.cpp confdlg.cpp conquer.cpp const.cpp coord.cpp credits.cpp debug.cpp dial8.cpp dialog.cpp
    display.cpp door.cpp drive.cpp drop.cpp edit.cpp ending.cpp event.cpp expand.cpp facing.cpp factory.cpp
    findpath.cpp flasher.cpp fly.cpp foot.cpp fuse.cpp gamedlg.cpp gauge.cpp globals.cpp goptions.cpp
    gscreen.cpp hdata.cpp heap.cpp help.cpp house.cpp iconset.cpp idata.cpp infantry.cpp init.cpp intro.cpp
    iomap.cpp ioobj.cpp jshell.cpp layer.cpp lib.cpp list.cpp loaddlg.cpp logic.cpp map.cpp mapeddlg.cpp
    mapedit.cpp mapedplc.cpp mapedsel.cpp mapedtm.cpp mapsel.cpp menus.cpp miscasm.cpp mission.cpp monoc.cpp
    mouse.cpp mplayer.cpp msgbox.cpp msglist.cpp nulldlg.cpp object.cpp odata.cpp options.cpp overlay.cpp
    power.cpp profile.cpp queue.cpp radar.cpp radio.cpp reinf.cpp rules.cpp saveload.cpp scenario.cpp
    scenarioini.cpp score.cpp scroll.cpp sdata.cpp shapebtn.cpp sidebar.cpp slider.cpp smudge.cpp
    sounddlg.cpp special.cpp startup.cpp stats.cpp super.cpp tab.cpp tarcom.cpp target.cpp tdata.cpp
    team.cpp teamtype.cpp techno.cpp template.cpp terrain.cpp textbtn.cpp theme.cpp trigger.cpp turret.cpp
    txtlabel.cpp udata.cpp unit.cpp visudlg.cpp winstub.cpp
)
list(TRANSFORM VC_TD_SRC PREPEND ${VC_TD}/)

set(VC_RA_SRC
    aadata.cpp abstract.cpp adata.cpp aircraft.cpp anim.cpp audio.cpp bar.cpp base.cpp bbdata.cpp bdata.cpp
    bench.cpp bigcheck.cpp building.cpp bullet.cpp cargo.cpp carry.cpp ccini.cpp ccptr.cpp cdata.cpp cell.cpp
    checkbox.cpp cheklist.cpp colrlist.cpp combat.cpp confdlg.cpp conquer.cpp const.cpp coord.cpp crate.cpp
    credits.cpp debug.cpp dial8.cpp dialog.cpp dibfile.cpp dibutil.cpp display.cpp door.cpp drive.cpp drop.cpp
    edit.cpp egos.cpp ending.cpp event.cpp expand.cpp face.cpp facing.cpp factory.cpp findpath.cpp flasher.cpp
    fly.cpp foot.cpp fuse.cpp gamedlg.cpp gauge.cpp globals.cpp goptions.cpp gscreen.cpp hdata.cpp heap.cpp
    help.cpp house.cpp iconlist.cpp iconset.cpp idata.cpp infantry.cpp init.cpp intro.cpp iomap.cpp ioobj.cpp
    jshell.cpp layer.cpp list.cpp loaddlg.cpp logic.cpp map.cpp mapeddlg.cpp mapedit.cpp mapedplc.cpp
    mapedsel.cpp mapedtm.cpp mapsel.cpp menus.cpp miscasm.cpp mission.cpp monoc.cpp mouse.cpp mplayer.cpp
    msgbox.cpp msglist.cpp nulldlg.cpp object.cpp odata.cpp options.cpp overlay.cpp power.cpp profile.cpp
    queue.cpp radar.cpp radio.cpp rawolapi.cpp reinf.cpp rules.cpp saveload.cpp scenario.cpp score.cpp
    scroll.cpp sdata.cpp seditdlg.cpp session.cpp shapebtn.cpp sidebar.cpp slider.cpp smudge.cpp sounddlg.cpp
    special.cpp startup.cpp statbtn.cpp stats.cpp super.cpp tab.cpp taction.cpp target.cpp tdata.cpp team.cpp
    teamtype.cpp techno.cpp template.cpp terrain.cpp tevent.cpp textbtn.cpp theme.cpp tooltip.cpp tracker.cpp
    trigger.cpp trigtype.cpp txtlabel.cpp udata.cpp unit.cpp vdata.cpp version.cpp vessel.cpp visudlg.cpp
    vortex.cpp warhead.cpp weapon.cpp winstub.cpp wolstrng.cpp
)
list(TRANSFORM VC_RA_SRC PREPEND ${VC_RA}/)

if(CNC_GAME STREQUAL "ra")
    set(VC_GAME ${VC_RA})
    set(VC_GAME_SRC ${VC_RA_SRC})
    set(VC_GAME_DEFS CNC_GAME_RA)
else()
    set(VC_GAME ${VC_TD})
    set(VC_GAME_SRC ${VC_TD_SRC})
    set(VC_GAME_DEFS CNC_GAME_TD)
endif()
# port code compiled with the game headers and flags
list(APPEND VC_GAME_SRC ${CMAKE_CURRENT_LIST_DIR}/cnc_gamehooks.cpp)

idf_component_register(
    SRCS
        ${VC_COMMON_SRC}
        ${VC_GAME_SRC}
                cnc_video.cpp
        cnc_input.cpp
        cnc_sound.cpp
        cnc_paths.cpp
        cnc_gitinfo.cpp
        cnc_debug.cpp
        cnc_flashcache.cpp
                                        display.c
        font8x8.c
    LDFRAGMENTS
        linker.lf
    INCLUDE_DIRS
        .
        sdl
        ${VC_GAME}
        ${VC_DIR}
        ${VC_COMMON}
    REQUIRES
        retro-go
        esp_partition
        esp_psram
        esp_timer)

# Game build flavour: an "SDL2" build without SDL (key codes are USB HID usages,
# see sdl/), new video path (the frame is presented by Video_Render_Frame()).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TRUE_FALSE_DEFINED ENGLISH SDL_BUILD SDL2_BUILD NEW_VIDEO_BUILD FIXIT_FAST_LOAD ESP_PLATFORM_CNC ${VC_GAME_DEFS})
# The game code predates most of the modern warnings
set_source_files_properties(${VC_COMMON_SRC} ${VC_GAME_SRC} PROPERTIES COMPILE_OPTIONS
    "-O2;-std=gnu++14;-fsigned-char;-w;-Wno-error;-fno-strict-aliasing;-fpermissive;-Wno-narrowing")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=unused-variable -Wno-error=unused-function)
# main() of the game, called by the game task (cnc_main.cpp)
set_property(SOURCE ${VC_GAME}/startup.cpp APPEND PROPERTY COMPILE_DEFINITIONS main=cnc_game_main)
