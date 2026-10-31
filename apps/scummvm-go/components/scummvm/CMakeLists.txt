# ScummVM (SCUMM v0-v6 + Cine engines) for Retro-Go.
# Source lists mirror the old per-component component.mk files from johannesbehr/scumm,
# merged into one component so everything shares one include root ("common/..", "audio/..").
set(COMPONENT_SRCDIRS
    rg
    audio audio/decoders audio/mods audio/softsynth audio/softsynth/fmtowns_pc98
    backends backends/events/default backends/audiocd/default backends/fs backends/fs/posix
    backends/plugins/posix backends/saves backends/saves/posix backends/saves/default
    backends/timer/default
    base
    cine
    common
    engines
    graphics graphics/fonts graphics/scaler
    gui
    image image/codecs
    scumm scumm/he scumm/players scumm/imuse scumm/imuse/drivers
)
set(COMPONENT_ADD_INCLUDEDIRS "." "rg")
set(COMPONENT_REQUIRES retro-go)

register_component()

rg_setup_compile_options(
    -std=gnu++11
    -Wno-error
    # IDF builds with -Werror=all, which makes each of these a hard error individually;
    # a plain -Wno-error does not undo that, so they must be listed one by one.
    -Wno-error=address
    -Wno-error=maybe-uninitialized
    -Wno-error=uninitialized
    -Wno-error=deprecated-copy
    -Wno-error=class-memaccess
    -Wno-error=parentheses
    -Wno-error=strict-aliasing
    -Wno-error=array-bounds
    -Wno-error=nonnull
    -Wno-error=sequence-point
    -Wno-error=narrowing
    -Wno-error=delete-non-virtual-dtor
    -Wno-error=sizeof-pointer-memaccess
    -Wno-error=overloaded-virtual
    -Wno-error=switch
    -Wno-error=format
    -Wno-error=unused-value
    -Wno-error=comment
    -Wno-error=dangling-pointer
    -Wno-error=stringop-overflow
    -Wno-error=restrict
    -Wno-maybe-uninitialized
    -Wno-deprecated-copy
    -Wno-class-memaccess
    -Wno-format
    -Wno-reorder
    -Wno-multichar
    -Wno-unknown-pragmas
    -Wno-attributes
    -Wno-deprecated-declarations
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-misleading-indentation
    -Wno-implicit-fallthrough
    -Wno-array-bounds
    -Wno-stringop-truncation
    -Wno-format-truncation
    -Wno-format-overflow
    -Wno-missing-field-initializers
    -O2
)
