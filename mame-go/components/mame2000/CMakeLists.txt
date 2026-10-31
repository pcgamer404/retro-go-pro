# MAME 0.37b5 (mame2000-libretro subset) for the mame-go app, see README_MAMEGO.md
set(COMPONENT_SRCS
    mamego_driver.c
    mamego_pick.c
    mamego_neospr.c
    mamego_prof.c
    src/artwork.c
    src/audit.c
    src/cheat.c
    src/common.c
    src/cpu/i8039/i8039.c
    src/cpu/m6502/m6502.c
    src/cpu/m68000/m68kmame.c
    src/cpu/s2650/s2650.c
    src/cpu/z80/z80.c
    src/cpu/z80/z80snd.c
    src/cpuintrf.c
    src/datafile.c
    src/drawgfx.c
    src/drivers/1942.c
    src/drivers/1943.c
    src/drivers/aerofgt.c
    src/drivers/astrocde.c
    src/drivers/bloodbro.c
    src/drivers/circus.c
    src/drivers/dkong.c
    src/drivers/exidy.c
    src/drivers/frogger.c
    src/drivers/galaga.c
    src/drivers/galaxian.c
    src/drivers/neogeo.c
    src/drivers/cps1.c
    src/drivers/pacman.c
    src/drivers/scramble.c
    src/drivers/starfire.c
    src/drivers/victory.c
    src/gfxobj.c
    src/hiscore.c
    src/info.c
    src/inptport.c
    src/input.c
    src/libretro/blit.c
    src/libretro/config.c
    src/libretro/fileio.c
    src/libretro/input.c
    src/libretro/libretro-common/compat/compat_strcasestr.c
    src/libretro/libretro-common/compat/compat_strl.c
    src/libretro/libretro-common/file/file_path.c
    src/libretro/libretro.c
    src/libretro/shared.c
    src/libretro/sound.c
    src/libretro/video.c
    src/machine/6821pia.c
    src/machine/8255ppi.c
    src/machine/eeprom.c
    src/machine/kabuki.c
    src/machine/mathbox.c
    src/machine/ticket.c
    src/machine/z80fmly.c
    src/mame.c
    src/memory.c
    src/palette.c
    src/png.c
    src/profiler.c
    src/sndhrdw/gorf.c
    src/sndhrdw/seibu.c
    src/sndintrf.c
    src/sound/2203intf.c
    src/sound/2610intf.c
    src/sound/3812intf.c
    src/sound/5220intf.c
    src/sound/adpcm.c
    src/sound/astrocde.c
    src/sound/ay8910.c
    src/sound/dac.c
    src/sound/fm.c
    src/sound/fmopl.c
    src/sound/hc55516.c
    src/sound/mixer.c
    src/sound/namco.c
    src/sound/nes_apu.c
    src/sound/qsound.c
    src/sound/2151intf.c
    src/sound/ym2151.c
    src/sound/samples.c
    src/sound/sn76496.c
    src/sound/streams.c
    src/sound/votrax.c
    src/sound/ymdeltat.c
    src/sprite.c
    src/state.c
    src/tilemap.c
    src/timer.c
    src/ui_text.c
    src/unzip.c
    src/usrintrf.c
    src/version.c
    src/vidhrdw/avgdvg.c
    src/vidhrdw/generic.c
    src/vidhrdw/pengo.c
    src/vidhrdw/vector.c
    src/zlib/adler32.c
    src/zlib/compress.c
    src/zlib/crc32.c
    src/zlib/deflate.c
    src/zlib/gzio.c
    src/zlib/infback.c
    src/zlib/inffast.c
    src/zlib/inflate.c
    src/zlib/inftrees.c
    src/zlib/trees.c
    src/zlib/uncompr.c
    src/zlib/zutil.c
)
set(COMPONENT_ADD_INCLUDEDIRS "src src/libretro src/libretro/libretro-common/include")
set(COMPONENT_REQUIRES "retro-go")
set(COMPONENT_ADD_LDFRAGMENTS "linker.lf")
register_component()
rg_setup_compile_options(
    -O2 -w -fsigned-char -fcommon
    -Wno-error=incompatible-pointer-types -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration -Wno-error=implicit-int
    -DALIGN_INTS -DALIGN_SHORTS -DMAME_UNDERCLOCK -DMAME_FASTSOUND -DBIGCASE
    -D__LIBRETRO__ -DLSB_FIRST -DMAMEGO
    -DHAS_Z80=1 -DHAS_M68000=1 -DHAS_M6502=1 -DHAS_M65C02=1 -DHAS_N2A03=1 -DHAS_I8035=1 -DHAS_S2650=1
    -DHAS_NAMCO=1 -DHAS_CUSTOM=1 -DHAS_SN76496=1 -DHAS_AY8910=1 -DHAS_YM2203=1 -DHAS_YM2610=1 -DHAS_YM3812=1 -DHAS_QSOUND=1
    -DHAS_DAC=1 -DHAS_SAMPLES=1 -DHAS_HC55516=1 -DHAS_ASTROCADE=1 -DHAS_TMS5220=1 -DHAS_NES=1 -DHAS_OKIM6295=1 -DHAS_ADPCM=1 -DHAS_YM2151=1
)

