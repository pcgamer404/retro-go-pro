#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rom.h"
#include "course_layout.h"
#include "s3_layout.h"

/*
 * The game asset symbols are absolute aliases into a 64 KB-page flash window
 * mapped at MK64_S3_FLASH_BASE.  Keeping the gMk64AssetRegion[] spelling means
 * the rest of the port does not need to know whether the region is in PSRAM or
 * in memory-mapped flash.
 */
#define gMk64AssetRegion ((uint8_t *)(uintptr_t)MK64_S3_FLASH_BASE)

/*
 * On the first boot after an app update, extract the ROM recipes into the tail
 * of the running OTA partition and map that tail read-only at
 * MK64_S3_FLASH_BASE.  On later boots only the mapping is recreated.
 */
bool mk64_resident_load(mk64_rom_t *rom, char *error, size_t error_size);
