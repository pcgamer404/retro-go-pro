/* Core pool manifest, shared between the second-stage bootloader and the
 * launcher app. Both sides must agree on this layout byte for byte, so keep
 * the struct packed and versioned.
 *
 * The pool is a single data partition holding installed core images. Its
 * first two 4 KB sectors hold two copies of this manifest; images start at
 * CORE_ALIGN boundaries after them. Images must be 64 KB aligned because the
 * ESP image format stores only virtual addresses, and the bootloader maps
 * flash pages using the physical offset. Keeping paddr and vaddr congruent
 * modulo 64 KB is what makes an arbitrary placement work.
 *
 * Two manifest copies are kept so that a power loss during a save cannot
 * destroy the list of installed cores: the newest copy with a valid CRC wins.
 */
#ifndef CORE_MANIFEST_H
#define CORE_MANIFEST_H

#include <stdint.h>

#define CORE_MANIFEST_MAGIC   0x314D4752u   /* "RGM1" */
#define CORE_MANIFEST_VERSION 2u
#define CORE_MANIFEST_SECTOR  0x1000u       /* one flash sector per copy */
#define CORE_MANIFEST_COPIES  2
#define CORE_ALIGN            0x10000u      /* images must be 64 KB aligned */
#define CORE_MAX_ENTRIES      16
#define CORE_NAME_LEN         16
#define CORE_TITLE_LEN        32
#define CORE_VER_LEN          32
#define CORE_DATE_LEN         16
#define CORE_TIME_LEN         16

/* Hold this pin low at power-on to wipe the manifest and boot the launcher.
 * GPIO 5 is RG_KEY_SELECT on the my-handheld target. */
#define CORE_WIPE_GPIO        5
#define CORE_WIPE_SECONDS     2

/* core_entry_t.flags: bit 0 was CORE_FLAG_PORT ("installed by the RG Ports
 * tab"). The tab is gone, the bit is ignored, and entries that still have it
 * set are shown as normal tabs. */

typedef struct {
    char     name[CORE_NAME_LEN];      /* "gwenesis", unique, from the filename */
    char     title[CORE_TITLE_LEN];    /* "Sega Mega Drive", free text */
    char     version[CORE_VER_LEN];    /* from the app descriptor, may be long */
    char     date[CORE_DATE_LEN];      /* "Oct 26 2026" */
    char     time[CORE_TIME_LEN];      /* "12:34:56" */
    uint32_t offset;                   /* from pool start, CORE_ALIGN aligned */
    uint32_t size;                     /* image length in bytes */
    uint32_t crc32;                    /* CRC32 of the image bytes */
    uint32_t flags;
    /* Asset area the core writes inside its own partition at runtime (e.g.
     * mk64-go). Recorded at install time and kept here so the launcher never
     * has to recognise a core by name again: the space stays reserved, counts
     * as used and is skipped by the allocator. 0 = the core has none. */
    uint32_t reserve_offset;
    uint32_t reserve_size;
} core_entry_t;

typedef struct {
    uint32_t     magic;
    uint32_t     version;
    uint32_t     seq;                  /* bumped on every successful save */
    int32_t      selected;             /* entry index, -1 = boot the launcher */
    uint32_t     count;                /* used entries, all packed at the front */
    uint32_t     crc;                  /* CRC32 over this struct with crc = 0 */
    uint32_t     reserved[10];
    core_entry_t entries[CORE_MAX_ENTRIES];
} core_manifest_t;

_Static_assert(sizeof(core_manifest_t) <= CORE_MANIFEST_SECTOR,
               "core manifest does not fit in one flash sector");

/* Bytes reserved for the manifest copies before the first image. */
#define CORE_MANIFEST_RESERVED (CORE_MANIFEST_SECTOR * CORE_MANIFEST_COPIES)

#endif /* CORE_MANIFEST_H */
