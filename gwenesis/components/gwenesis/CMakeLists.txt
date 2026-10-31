set(COMPONENT_SRCDIRS "src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_ADD_INCLUDEDIRS ". src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(-O3 -fno-unroll-loops -Wno-incompatible-pointer-types -DGWENESIS_YM_WORKER=1)

# A/B result (Oct 25): -Os shrank .flash.text by 155 KB but cost 4-8% on
# cpu68 (cricket 7.4->7.7 / 12.5->13.5 ms), so -O3 stays. The 68K handlers
# are i-cache bound; the smaller code did not help in practice.
set_source_files_properties("src/cpus/M68K/m68kcpu.c" PROPERTIES COMPILE_OPTIONS "-O3")
