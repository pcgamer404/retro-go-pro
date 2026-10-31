#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rg_surface.h>

#define CORE_ASSET_MAGIC "RGCOREA1"
#define CORE_ASSET_VERSION 2u
#define CORE_ASSET_MAX_BUNDLE (256u * 1024u)

/* Version 1 bundles end right after launch_mode/reserved. They are still
 * accepted so bundles built before the version field existed keep working. */
#define CORE_ASSET_MANIFEST_V1_SIZE 216u

enum {
    CORE_ART_BACKGROUND = 1,
    CORE_ART_LOGO = 2,
    CORE_ART_BANNER = 3,
};

enum {
    CORE_LAUNCH_DIRECT = 1,
    CORE_LAUNCH_ROM = 0,
};

/* Binary manifest embedded in each new-style core image. Keep field order and
 * widths synchronized with tools/gen_core_assets.py. */
typedef struct __attribute__((packed)) {
    char id[16];
    char title[64];
    char rom_extensions[64];
    char rom_folder[64];
    uint8_t launch_mode;
    uint8_t reserved[7];
    char version[32];               /* optional, from core.json */
} core_assets_manifest_t;

_Static_assert(offsetof(core_assets_manifest_t, version) == CORE_ASSET_MANIFEST_V1_SIZE,
               "core bundle v2 must keep the v1 fields at the same offsets");

bool core_pool_get_assets(const char *id, core_assets_manifest_t *out);
rg_image_t *core_pool_load_art(const char *id, uint8_t kind);
