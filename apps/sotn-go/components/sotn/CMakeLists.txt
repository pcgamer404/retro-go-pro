# SOTN game + Retro-Go platform glue.
#
# The game is NOT copied into retro-go: it is compiled straight from a checkout
# of the SOTN decomp (the ESP32 port branch) found via SOTN_REPO. Only the
# platform layer is replaced (rg_*.c here); the three .cmake source lists,
# soft_esp32.c and esp_fileapi.c are reused unchanged from the decomp's
# esp32/main directory.
#
#   -DSOTN_REPO=/path/to/sotn-decomp-esp32-port   or env SOTN_REPO
#   default: sibling of the retro-go checkout

if(NOT SOTN_REPO AND DEFINED ENV{SOTN_REPO})
    set(SOTN_REPO $ENV{SOTN_REPO})
endif()
if(NOT SOTN_REPO)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../../../../sotn-decomp-esp32-port/esp32/main/game_sources.cmake")
        get_filename_component(SOTN_REPO
            "${CMAKE_CURRENT_LIST_DIR}/../../../../../../sotn-decomp-esp32-port" ABSOLUTE)
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../../sotn-decomp-esp32-port/esp32/main/game_sources.cmake")
        get_filename_component(SOTN_REPO
            "${CMAKE_CURRENT_LIST_DIR}/../../../../sotn-decomp-esp32-port" ABSOLUTE)
    else()
        get_filename_component(SOTN_REPO
            "D:/Games/E/sotn-decomp-esp32-port" ABSOLUTE)
    endif()
endif()
if(NOT EXISTS "${SOTN_REPO}/esp32/main/game_sources.cmake")
    message(FATAL_ERROR
        "sotn-go: SOTN_REPO='${SOTN_REPO}' is not the SOTN decomp ESP32 port "
        "(esp32/main/game_sources.cmake not found). Set SOTN_REPO.")
endif()
if(NOT EXISTS "${SOTN_REPO}/tools/psyz/psyz/src/psyz")
    message(FATAL_ERROR
        "sotn-go: the psyz submodule is empty. Run: git -C ${SOTN_REPO} "
        "submodule update --init tools/psyz")
endif()

# Where the disc-derived data lives on the SD card (same convention as
# roms/opentyrian). Must start with the target's RG_STORAGE_ROOT.
if(NOT SOTN_DATA_ROOT)
    set(SOTN_DATA_ROOT "/sd/roms/sotn")
endif()

set(REPO ${SOTN_REPO})
set(PSYZ ${REPO}/tools/psyz/psyz)
set(PSYQ ${REPO}/tools/psyz/decomp)
set(SOTN_ESP ${REPO}/esp32/main)

# ---- game: PC layer ----------------------------------------------------------
set(SRC_PC
    ${REPO}/src/pc/main.c
    ${REPO}/src/pc/stubs.c
    ${REPO}/src/pc/sotn.c
    ${REPO}/src/pc/replay.c
    ${REPO}/src/pc/pc.c
    ${REPO}/src/pc/cutscene_pc.c
    ${REPO}/src/pc/io.c
    ${REPO}/src/pc/sim_pc.c
    ${REPO}/src/pc/pl_arc.c
    ${REPO}/src/pc/pl_ric.c
    ${REPO}/src/pc/servant_pc.c
    ${REPO}/src/pc/weapon_pc.c
    ${REPO}/src/pc/stages/stage_loader.c
    ${REPO}/src/pc/stages/overlay.c
    ${REPO}/src/pc/stages/stage_wrp.c
    ${REPO}/src/pc/3rd/cJSON/cJSON.c
)

# ---- game: engine + players + weapons + statically linked stages -------------
include(${SOTN_ESP}/game_sources.cmake)
set(SRC_DRA    ${GAME_SOURCE_FILES_DRA})
set(SRC_RIC    ${GAME_SOURCE_FILES_RIC})
set(SRC_WEAPON ${GAME_SOURCE_FILES_WEAPON})
set(SRC_WRP    ${GAME_SOURCE_FILES_WRP})
set(SRC_NZ0    ${GAME_SOURCE_FILES_NZ0})
set(SRC_SEL    ${GAME_SOURCE_FILES_SEL})
set(SRC_NP3    ${GAME_SOURCE_FILES_NP3})

# ---- psyz core + decomp Psy-Q libgpu -----------------------------------------
file(GLOB SRC_PSYZ ${PSYZ}/src/psyz/*.c)
list(FILTER SRC_PSYZ EXCLUDE REGEX "libgte_hw_sqrt")
include(${SOTN_ESP}/psyq_sources.cmake)
set(SRC_LIBGPU ${GAME_SOURCE_FILES_PSYQ})
set(SRC_PLATFORM ${PSYZ}/src/platform/psyz.c)

set(COMPONENT_SRCS
    rg_shim.c
    rg_lcd.c
    rg_dispinfo.c
    rg_audio_out.c
    rg_boot.c
    # reused unchanged from the standalone ESP32 port
    ${SOTN_ESP}/esp_fileapi.c
    ${SOTN_ESP}/soft_esp32.c
    ${SRC_PC}
    ${SRC_DRA}
    ${SRC_RIC}
    ${SRC_WEAPON}
    ${SRC_WRP}
    ${SRC_NZ0}
    ${SRC_SEL}
    ${SRC_NP3}
    ${REPO}/src/pc/stages/stage_sel.c
    ${SRC_PSYZ}
    ${SRC_LIBGPU}
    ${SRC_PLATFORM}
)
set(COMPONENT_ADD_INCLUDEDIRS .)
set(COMPONENT_REQUIRES retro-go)
set(COMPONENT_PRIV_REQUIRES esp_timer esp_rom esp_psram fatfs spi_flash driver)
set(COMPONENT_ADD_LDFRAGMENTS "linker.lf")

register_component()

# The game's include dirs are PRIVATE on purpose: psyz/game headers must not
# leak into main or retro-go and shadow anything there.
target_include_directories(${COMPONENT_LIB} PRIVATE
    ${SOTN_ESP}
    ${REPO}/include
    ${REPO}/src/dra
    ${REPO}/src/pc/3rd
    ${REPO}/src/pc
    ${PSYZ}/include
    ${PSYZ}/src
)

rg_setup_compile_options(
    # -Werror=all promotes every -Wall member to a SPECIFIC error, which a
    # blanket -Wno-error does not demote; the warnings themselves must go
    -Wno-error
    -Wno-char-subscripts
    -Wno-switch
    -Wno-pointer-sign
    -Wno-format
    -Wno-dangling-pointer
    -Wno-format-truncation
    -Wno-uninitialized
    -Wno-maybe-uninitialized
    -Wno-parentheses
    -Wno-misleading-indentation
    -Wno-missing-braces
    -Wno-sign-compare
    -Wno-sequence-point
    -Wno-unused-value
    -Wno-bool-operation
    -Wno-empty-body
    -Wno-type-limits
    -Wno-memset-elt-size
    -Wno-sizeof-pointer-div
    -Wno-unused-label
    -Wno-bool-compare
    -Wno-logical-not-parentheses
    -Wno-stringop-truncation
    -Wno-enum-int-mismatch
    -Wno-enum-conversion
    -Wno-array-bounds
    -Wno-stringop-overflow
    -Wno-address
    -Wno-implicit-function-declaration
    -Wno-incompatible-pointer-types
    -Wno-int-conversion
    -Wno-return-mismatch
    -Wno-return-type
    -Wno-builtin-declaration-mismatch
    -Wno-unused-variable
    -Wno-unused-function
    -funsigned-char
    -O3
    -fno-strict-aliasing
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    _USE_MATH_DEFINES
    VERSION_PC
    PERMUTER
    NON_MATCHING
    HARD_LINK
    FIX_UB
    _internal_version_us
    __psyz
    SOTN_STATIC_WRP
    SOTN_STATIC_NZ0
    SOTN_STATIC_SEL
    SOTN_STATIC_NP3
    SOTN_RETROGO
    SOTN_DATA_ROOT="${SOTN_DATA_ROOT}"
    # ESP-IDF's VFS ignores chdir(): every fopen is rebased onto SOTN_DATA_ROOT
    # by esp_fileapi.c (which #undefs this to reach the real one)
    fopen=sotn_fopen
)
target_compile_options(${COMPONENT_LIB} PRIVATE -include stdio.h)

# main.c's `main` becomes callable as sotn_main()
set_source_files_properties(${REPO}/src/pc/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=sotn_main")

# ---- per-stage symbol renames (identical to the standalone port) -------------
# Several static stages in one image: every global that OVL_EXPORT does not
# cover collides between stages. Rename them per stage; references are
# stage-internal by construction.
include(${SOTN_ESP}/stage_dup_syms.cmake)
list(APPEND STAGE_DUP_SYMS cluts gfxBanks rooms_layers spriteBanks)

function(sotn_rename_stage_syms prefix)
    foreach(src ${ARGN})
        set(defs "")
        foreach(sym ${STAGE_DUP_SYMS})
            list(APPEND defs "${sym}=${prefix}_${sym}")
        endforeach()
        set_property(SOURCE ${src} APPEND PROPERTY COMPILE_DEFINITIONS ${defs})
    endforeach()
endfunction()

sotn_rename_stage_syms(WRP ${SRC_WRP} ${REPO}/src/pc/stages/stage_wrp.c)
sotn_rename_stage_syms(NZ0 ${SRC_NZ0})
sotn_rename_stage_syms(NP3 ${SRC_NP3})
sotn_rename_stage_syms(SEL ${SRC_SEL} ${REPO}/src/pc/stages/stage_sel.c)

set_property(SOURCE ${REPO}/src/pc/stages/stage_wrp.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=WRP_StaticInitStage")
set_property(SOURCE ${REPO}/src/pc/stages/stage_nz0.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=NZ0_StaticInitStage")
set_property(SOURCE ${REPO}/src/pc/stages/stage_sel.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=SEL_StaticInitStage")
set_property(SOURCE ${REPO}/src/pc/stages/stage_np3.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=NP3_StaticInitStage")

# WEAPON_ID names each weapon's symbols (w_002_header etc.)
foreach(w 000 002 007 012 034 037 041 045 046 051 052)
    set_source_files_properties(${REPO}/src/weapon/w_${w}.c PROPERTIES
        COMPILE_DEFINITIONS "WEAPON_ID=w_${w}")
endforeach()
