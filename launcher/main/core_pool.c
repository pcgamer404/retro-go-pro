/* Core pool: install, remove and select emulator/port cores that live in the "cores" partition
 * instead of the fixed partition table. The pool is registered as an APP partition (type 0,
 * custom subtype) even though it never boots by itself: cores such as mk64-go ask the IDF which
 * partition contains the running image (esp_ota_get_running_partition) and only look at APP
 * partitions, so a data partition would make them fail. Nothing in the boot path changes -
 * coreboot patches ota[0] and hands the image to the stock loader. The bootloader
 * (bootloader_components/coreboot) reads the same manifest and boots the selected image.
 * Everything here only edits the manifest; the images themselves are written once, at 64 KB
 * aligned offsets, and are never moved. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <rg_system.h>
#include <rg_gui.h>
#include <rg_storage.h>
#include <rg_utils.h>
#include "core_manifest.h"
#include "core_pool.h"

#define CORE_POOL_LABEL "cores"
#define CORE_SD_FOLDER RG_STORAGE_ROOT "/cores"
#define CORE_SD_ASSET_FOLDER CORE_SD_FOLDER "/installed"

/* Some cores build a writable asset store inside their own partition, at fixed offsets from its
 * start. mk64-go (ESP32-S3) erases and fills [0x1DF000, 0xC00000) on its first run and expects
 * its own image below 0x1DF000. Those ranges must stay free while such a core is installed, and
 * they count as used space. */
#define CORE_STORE_MK64_START 0x1DF000u
#define CORE_STORE_MK64_END 0xC00000u

/* First byte of every ESP image. */
#define CORE_IMAGE_MAGIC 0xE9

static const esp_partition_t *pool;
static core_manifest_t manifest;
static int manifest_slot = -1; /* copy currently in use, -1 = none valid */

static int entry_find(const char *name);
static bool store_range_for(const char *name, uint32_t *start, uint32_t *end);

typedef struct
{
    const uint8_t *bundle;
    size_t total_size;
    size_t manifest_offset;
    uint32_t manifest_size;
    uint16_t asset_count;
} core_bundle_view_t;

typedef struct __attribute__((packed))
{
    uint8_t magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t manifest_size;
    uint16_t asset_count;
    uint16_t reserved;
    uint32_t crc;
} core_bundle_header_t;

typedef struct __attribute__((packed))
{
    uint8_t kind;
    uint32_t size;
} core_bundle_asset_t;

_Static_assert(sizeof(core_bundle_header_t) == 28, "core bundle header format changed");
_Static_assert(CORE_ASSET_MANIFEST_V1_SIZE == 216, "core bundle v1 manifest format changed");
_Static_assert(sizeof(core_assets_manifest_t) == 248, "core asset manifest format changed");

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool valid_core_id(const char *id, size_t size)
{
    size_t len = strnlen(id, size);
    if (!len || len >= size || len >= CORE_NAME_LEN)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool valid_rom_folder(const char *folder, size_t size)
{
    size_t len = strnlen(folder, size);
    if (!len || len >= size || folder[0] == '/' || folder[0] == '\\')
        return false;
    const char *part = folder;
    for (size_t i = 0; i <= len; i++)
    {
        if (folder[i] == '/' || folder[i] == '\\' || folder[i] == 0)
        {
            size_t part_len = folder + i - part;
            if (!part_len || (part_len == 1 && part[0] == '.') ||
                (part_len == 2 && part[0] == '.' && part[1] == '.'))
                return false;
            part = folder + i + 1;
        }
    }
    return true;
}

static bool core_bundle_parse_at(const uint8_t *data, size_t available, core_bundle_view_t *out)
{
    if (available < sizeof(core_bundle_header_t) || memcmp(data, CORE_ASSET_MAGIC, 8) != 0)
        return false;
    uint16_t version = read_le16(data + 8);
    uint16_t header_size = read_le16(data + 10);
    uint32_t total_size = read_le32(data + 12);
    uint32_t manifest_size = read_le32(data + 16);
    uint16_t asset_count = read_le16(data + 20);
    uint32_t crc = read_le32(data + 24);
    /* v1 bundles have no version field; everything before it is identical. */ size_t
        expected_manifest =
            version == 1 ? CORE_ASSET_MANIFEST_V1_SIZE : sizeof(core_assets_manifest_t);
    if ((version != 1 && version != CORE_ASSET_VERSION) ||
        header_size != sizeof(core_bundle_header_t) || total_size > CORE_ASSET_MAX_BUNDLE ||
        total_size > available || manifest_size != expected_manifest || asset_count > 3 ||
        header_size + manifest_size > total_size)
        return false;
    const uint8_t *payload = data + header_size;
    if (esp_rom_crc32_le(0, payload, total_size - header_size) != crc)
        return false;
    core_assets_manifest_t manifest = {0};
    memcpy(&manifest, payload, RG_MIN(manifest_size, sizeof(manifest)));
    if (!valid_core_id(manifest.id, sizeof(manifest.id)) || !manifest.title[0] ||
        !memchr(manifest.title, 0, sizeof(manifest.title)) ||
        !memchr(manifest.rom_extensions, 0, sizeof(manifest.rom_extensions)) ||
        !valid_rom_folder(manifest.rom_folder, sizeof(manifest.rom_folder)) ||
        manifest.launch_mode > CORE_LAUNCH_DIRECT)
        return false;
    size_t cursor = header_size + manifest_size;
    for (uint16_t i = 0; i < asset_count; i++)
    {
        if (cursor + sizeof(core_bundle_asset_t) > total_size)
            return false;
        uint8_t kind = data[cursor];
        uint32_t image_size = read_le32(data + cursor + 1);
        cursor += sizeof(core_bundle_asset_t);
        if (kind < CORE_ART_BACKGROUND || kind > CORE_ART_BANNER ||
            image_size > total_size - cursor || image_size < 16 ||
            memcmp(data + cursor, "\x89PNG\r\n\x1a\n", 8) != 0)
            return false;
        cursor += image_size;
    }
    if (cursor != total_size)
        return false;
    if (out)
    {
        out->bundle = data;
        out->total_size = total_size;
        out->manifest_offset = header_size;
        out->manifest_size = manifest_size;
        out->asset_count = asset_count;
    }
    return true;
}

/* Search an image on SD without loading the whole firmware into RAM. */
static bool core_bundle_find_file(FILE *file, size_t file_size, uint8_t **bundle_data,
                                  core_bundle_view_t *out)
{
    const size_t scan_size = 0x8000;
    const size_t overlap = sizeof(CORE_ASSET_MAGIC) - 1;
    uint8_t *scan = malloc(scan_size + overlap);
    if (!scan)
        return false;
    if (fseek(file, 0, SEEK_SET) != 0)
    {
        free(scan);
        return false;
    }
    size_t offset = 0;
    size_t carry = 0;
    while (offset < file_size)
    {
        size_t amount = RG_MIN(scan_size, file_size - offset);
        if (fread(scan + carry, 1, amount, file) != amount)
            break;
        size_t available = carry + amount;
        for (size_t i = 0; i + sizeof(CORE_ASSET_MAGIC) - 1 <= available; i++)
        {
            if (memcmp(scan + i, CORE_ASSET_MAGIC, sizeof(CORE_ASSET_MAGIC) - 1) != 0)
                continue;
            size_t bundle_offset = offset - carry + i;
            uint8_t header[sizeof(core_bundle_header_t)];
            if (bundle_offset + sizeof(header) > file_size ||
                fseek(file, (long)bundle_offset, SEEK_SET) != 0 ||
                fread(header, 1, sizeof(header), file) != sizeof(header))
                continue;
            uint32_t total_size = read_le32(header + 12);
            if (total_size > CORE_ASSET_MAX_BUNDLE || total_size < sizeof(header) ||
                total_size > file_size - bundle_offset)
                continue;
            uint8_t *bundle = malloc(total_size);
            if (!bundle)
                continue;
            if (fseek(file, (long)bundle_offset, SEEK_SET) == 0 &&
                fread(bundle, 1, total_size, file) == total_size &&
                core_bundle_parse_at(bundle, total_size, out) && out->total_size == total_size)
            {
                *bundle_data = bundle;
                free(scan);
                return true;
            }
            free(bundle);
        }
        offset += amount;
        carry = RG_MIN(overlap, available);
        memmove(scan, scan + available - carry, carry);
        if (fseek(file, (long)offset, SEEK_SET) != 0)
            break;
    }
    free(scan);
    return false;
}

static void core_asset_path(char *out, size_t size, const char *id)
{
    snprintf(out, size, CORE_SD_ASSET_FOLDER "/%s.rga", id);
}

static bool core_bundle_load_file(const char *id, void **data, size_t *size,
                                  core_bundle_view_t *view)
{
    char path[RG_PATH_MAX + 1];
    core_asset_path(path, sizeof(path), id);
    if (!rg_storage_read_file(path, data, size, 0))
        return false;
    if (!core_bundle_parse_at(*data, *size, view) || view->total_size != *size)
    {
        free(*data);
        *data = NULL;
        *size = 0;
        return false;
    }
    return true;
}

bool core_pool_get_assets(const char *id, core_assets_manifest_t *out)
{
    if (!id || !out || !valid_core_id(id, strlen(id) + 1))
        return false;
    int index = entry_find(id);
    const char *asset_id = index >= 0 ? manifest.entries[index].name : id;
    void *data = NULL;
    size_t size = 0;
    core_bundle_view_t view;
    if (!core_bundle_load_file(asset_id, &data, &size, &view))
        return false;
    memset(out, 0, sizeof(*out));
    memcpy(out, (uint8_t *)data + view.manifest_offset,
           RG_MIN((size_t)view.manifest_size, sizeof(*out)));
    free(data);
    return true;
}

rg_image_t *core_pool_load_art(const char *id, uint8_t kind)
{
    if (!id || !valid_core_id(id, strlen(id) + 1))
        return NULL;
    int index = entry_find(id);
    const char *asset_id = index >= 0 ? manifest.entries[index].name : id;
    void *data = NULL;
    size_t size = 0;
    core_bundle_view_t view;
    if (!core_bundle_load_file(asset_id, &data, &size, &view))
        return NULL;
    size_t cursor = view.manifest_offset + view.manifest_size;
    for (uint16_t i = 0; i < view.asset_count; i++)
    {
        uint8_t asset_kind = ((uint8_t *)data)[cursor];
        uint32_t asset_size = read_le32((uint8_t *)data + cursor + 1);
        cursor += sizeof(core_bundle_asset_t);
        if (asset_kind == kind)
        {
            rg_image_t *image = rg_surface_load_image((uint8_t *)data + cursor, asset_size, 0);
            free(data);
            return image;
        }
        cursor += asset_size;
    }
    free(data);
    return NULL;
}

static uint32_t crc_of(const core_manifest_t *m)
{
    core_manifest_t tmp = *m;
    tmp.crc = 0;
    return esp_rom_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static bool manifest_read(int slot, core_manifest_t *out)
{
    if (esp_partition_read(pool, slot * CORE_MANIFEST_SECTOR, out, sizeof(*out)) != ESP_OK)
        return false;
    if (out->magic != CORE_MANIFEST_MAGIC || out->version != CORE_MANIFEST_VERSION)
        return false;
    return crc_of(out) == out->crc;
}

/* Version 1 had no per-entry asset area. Kept only so an older pool can be upgraded without
 * losing its installed cores. */
typedef struct
{
    char name[CORE_NAME_LEN];
    char title[CORE_TITLE_LEN];
    char version[CORE_VER_LEN];
    char date[CORE_DATE_LEN];
    char time[CORE_TIME_LEN];
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint32_t flags;
} core_entry_v1_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    int32_t selected;
    uint32_t count;
    uint32_t crc;
    uint32_t reserved[10];
    core_entry_v1_t entries[CORE_MAX_ENTRIES];
} core_manifest_v1_t;

static bool manifest_read_v1(int slot, core_manifest_v1_t *out)
{
    core_manifest_v1_t tmp;
    uint32_t stored_crc;
    if (esp_partition_read(pool, slot * CORE_MANIFEST_SECTOR, &tmp, sizeof(tmp)) != ESP_OK)
        return false;
    if (tmp.magic != CORE_MANIFEST_MAGIC || tmp.version != 1)
        return false;
    stored_crc = tmp.crc;
    tmp.crc = 0;
    if (esp_rom_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp)) != stored_crc)
        return false;
    *out = tmp;
    return true;
}

/* Writes to the copy that is not in use, so a power loss mid-save leaves the previous manifest
 * intact. */
static bool manifest_flush(void)
{
    if (!pool)
        return false;
    int slot = (manifest_slot == 0) ? 1 : 0;
    manifest.seq++;
    manifest.crc = crc_of(&manifest);
    if (esp_partition_erase_range(pool, slot * CORE_MANIFEST_SECTOR, CORE_MANIFEST_SECTOR) !=
        ESP_OK)
        return false;
    if (esp_partition_write(pool, slot * CORE_MANIFEST_SECTOR, &manifest, sizeof(manifest)) !=
        ESP_OK)
        return false;
    manifest_slot = slot;
    return true;
}

static void manifest_reset(void)
{
    memset(&manifest, 0, sizeof(manifest));
    manifest.magic = CORE_MANIFEST_MAGIC;
    manifest.version = CORE_MANIFEST_VERSION;
    manifest.selected = -1;
}

void core_pool_init(void)
{
    /* APP, not DATA: mk64-go locates its asset store through esp_ota_get_running_partition(),
 * which only considers APP partitions. */
    pool = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY,
                                    CORE_POOL_LABEL);
    if (!pool)
    {
        RG_LOGE("Core pool partition '%s' not found", CORE_POOL_LABEL);
        return;
    }
    rg_storage_mkdir(CORE_SD_FOLDER);
    rg_storage_mkdir(CORE_SD_ASSET_FOLDER);
    core_manifest_t tmp;
    core_manifest_t best;
    int best_slot = -1;
    for (int slot = 0; slot < CORE_MANIFEST_COPIES; slot++)
    {
        if (!manifest_read(slot, &tmp))
            continue;
        if (best_slot < 0 || tmp.seq > best.seq)
        {
            best = tmp;
            best_slot = slot;
        }
    }
    if (best_slot < 0)
    {
        core_manifest_v1_t old;
        core_manifest_v1_t best_old;
        int old_slot = -1;
        for (int slot = 0; slot < CORE_MANIFEST_COPIES; slot++)
        {
            if (!manifest_read_v1(slot, &old))
                continue;
            if (old_slot < 0 || old.seq > best_old.seq)
            {
                best_old = old;
                old_slot = slot;
            }
        }
        if (old_slot < 0)
        {
            RG_LOGW("Core pool is unformatted (%u KB)", (unsigned)(pool->size / 1024));
            manifest_reset();
            return;
        }
        /* Upgrade in place: the only new information is each entry's asset area, which
 * the install-time table still knows. */
        manifest_reset();
        manifest.seq = best_old.seq;
        manifest.selected = best_old.selected;
        manifest.count = RG_MIN(best_old.count, (uint32_t)CORE_MAX_ENTRIES);
        manifest_slot = old_slot;
        /* flush to the other copy */ for (uint32_t i = 0; i < manifest.count; i++)
        {
            const core_entry_v1_t *s = &best_old.entries[i];
            core_entry_t *e = &manifest.entries[i];
            uint32_t start = 0, end = 0;
            memcpy(e->name, s->name, sizeof(e->name));
            memcpy(e->title, s->title, sizeof(e->title));
            memcpy(e->version, s->version, sizeof(e->version));
            memcpy(e->date, s->date, sizeof(e->date));
            memcpy(e->time, s->time, sizeof(e->time));
            e->offset = s->offset;
            e->size = s->size;
            e->crc32 = s->crc32;
            e->flags = s->flags;
            if (store_range_for(e->name, &start, &end))
            {
                e->reserve_offset = start;
                e->reserve_size = end - start;
            }
        }
        if (!manifest_flush())
        {
            RG_LOGE("Failed to upgrade the core manifest");
            manifest_reset();
            return;
        }
        RG_LOGI("Core manifest upgraded to version %u", CORE_MANIFEST_VERSION);
    }
    else
    {
        manifest = best;
        manifest_slot = best_slot;
    }
    if (manifest.count > CORE_MAX_ENTRIES)
    {
        RG_LOGE("Core manifest is corrupt (count=%u), resetting", (unsigned)manifest.count);
        uint32_t previous_seq = manifest.seq;
        manifest_reset();
        manifest.seq = previous_seq;
        if (!manifest_flush())
            RG_LOGE("Failed to save reset core manifest");
        return;
    }
    if (manifest.selected >= (int)manifest.count)
        manifest.selected = -1;
    RG_LOGI("Core pool: %u core(s), %u KB used of %u KB", (unsigned)manifest.count,
            (unsigned)(core_pool_used() / 1024), (unsigned)(core_pool_total() / 1024));
    /* Cores installed before the asset area existed may sit inside it; the core that owns the
 * area erases it on its next run, so say so once. */
    for (uint32_t i = 0; i < manifest.count; i++)
    {
        const core_entry_t *owner = &manifest.entries[i];
        if (!owner->reserve_size)
            continue;
        for (uint32_t j = 0; j < manifest.count; j++)
        {
            const core_entry_t *e = &manifest.entries[j];
            if (i == j)
                continue;
            if (e->offset < owner->reserve_offset + owner->reserve_size &&
                e->offset + e->size > owner->reserve_offset)
                RG_LOGW("Core '%s' overlaps the asset area of '%s' and will be erased by it",
                        e->name, owner->name);
        }
    }
}

bool core_pool_available(void)
{
    return pool != NULL;
}

int core_pool_count(void)
{
    return pool ? (int)manifest.count : 0;
}

const core_entry_t *core_pool_entry(int index)
{
    if (!pool || index < 0 || index >= (int)manifest.count)
        return NULL;
    return &manifest.entries[index];
}

int core_pool_selected(void)
{
    return pool ? manifest.selected : -1;
}

uint32_t core_pool_total(void)
{
    return pool ? pool->size : 0;
}

uint32_t core_pool_used(void)
{
    uint32_t used = CORE_MANIFEST_RESERVED;
    for (uint32_t i = 0; i < manifest.count; i++)
    {
        used += align_up(manifest.entries[i].size, CORE_ALIGN);
        used += manifest.entries[i].reserve_size;
    }
    return used;
}

static void format_mb_number(char *out, size_t out_size, uint32_t bytes)
{
    const uint32_t mb = 1024u * 1024u;
    snprintf(out, out_size, "%u.%02u", (unsigned)(bytes / mb),
             (unsigned)(((bytes % mb) * 100u) / mb));
}

static void format_pool_mb(char *out, size_t out_size, uint32_t bytes)
{
    char number[16];
    format_mb_number(number, sizeof(number), bytes);
    snprintf(out, out_size, "%s MB", number);
}

/* "11.71 MB (1.70 + 10.01 assets)" - a core with an asset area is shown as one footprint, so the
 * list does not look like it hides 10 MB somewhere. */
static void format_footprint(char *out, size_t out_size, uint32_t image, uint32_t reserve)
{
    const uint32_t mb = 1024u * 1024u;
    uint32_t total = image + reserve;
    if (!reserve)
    {
        format_pool_mb(out, out_size, image);
        return;
    }
    snprintf(out, out_size, "%u.%02u MB (%u.%02u + %u.%02u %s)", (unsigned)(total / mb),
             (unsigned)(((total % mb) * 100u) / mb), (unsigned)(image / mb),
             (unsigned)(((image % mb) * 100u) / mb), (unsigned)(reserve / mb),
             (unsigned)(((reserve % mb) * 100u) / mb), _("assets"));
}

/* "Atari 2600" or "Atari 2600 v1.2". The version comes from the core's own core.json; the app
 * descriptor's git-describe string is never shown. */
static void core_display_name(char *out, size_t out_size, const char *title, const char *version)
{
    if (version && version[0])
        snprintf(out, out_size, "%s v%s", title, version);
    else
        snprintf(out, out_size, "%s", title);
}

/* First fit from the end of the manifest, walking past overlapping entries and past any
 * runtime-written asset range. `limit` is the exclusive end of the window the image may occupy. */
static void pool_skip(uint32_t *next, uint32_t start, uint32_t size, uint32_t range_start,
                      uint32_t range_size)
{
    uint32_t range_end;
    if (!range_size)
        return;
    range_end = align_up(range_start + range_size, CORE_ALIGN);
    if (start < range_end && range_start < start + size)
        *next = RG_MAX(*next, range_end);
}

static uint32_t pool_alloc(uint32_t size, uint32_t limit)
{
    uint32_t start = align_up(CORE_MANIFEST_RESERVED, CORE_ALIGN);
    if (limit > pool->size)
        limit = pool->size;
    for (;;)
    {
        if (start + size > limit)
            return 0;
        uint32_t next = 0;
        for (uint32_t i = 0; i < manifest.count; i++)
        {
            const core_entry_t *e = &manifest.entries[i];
            pool_skip(&next, start, size, e->offset, e->size);
            pool_skip(&next, start, size, e->reserve_offset, e->reserve_size);
        }
        if (!next)
            return start;
        start = next;
    }
}

/* Core names are matched loosely so a pool entry can satisfy a static app registration even when
 * the spellings differ: case and punctuation are ignored, and a trailing "go" may be dropped
 * ("mk-64", "mk64" and "mk64-go" all resolve to the same core). */
static void normalize_name(const char *src, char *dst, size_t size, bool strip_go)
{
    size_t len = 0;
    for (size_t i = 0; src[i] && len < size - 1; i++)
    {
        char c = src[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            dst[len++] = c;
    }
    dst[len] = 0;
    /* Only strip a real "go" suffix, never the last two digits of a name such as "mk-64". */
    if (strip_go && len > 2 && dst[len - 2] == 'g' && dst[len - 1] == 'o')
        dst[len - 2] = 0;
}

/* Cores that write an asset store into their own partition at runtime. The range is copied into
 * the manifest entry at install time, so this table is only consulted when a core is installed
 * (and when an old manifest is upgraded); nothing else in the launcher looks a core up by name.
 * Add one line here for a core that needs a fixed area. The name is matched like entry_find(),
 * so "mk-64.bin" and "mk64-go" both hit. */
typedef struct
{
    const char *name;
    uint32_t start;
    /* pool offset of the first written byte */ uint32_t end;
} core_store_t;

static const core_store_t core_stores[] = {
    {"mk64-go", CORE_STORE_MK64_START, CORE_STORE_MK64_END},
};

static bool same_core_name(const char *a, const char *b)
{
    char na[CORE_NAME_LEN], nb[CORE_NAME_LEN];
    normalize_name(a, na, sizeof(na), false);
    normalize_name(b, nb, sizeof(nb), false);
    if (strcmp(na, nb) == 0)
        return true;
    normalize_name(a, na, sizeof(na), true);
    normalize_name(b, nb, sizeof(nb), true);
    return strcmp(na, nb) == 0;
}

static bool store_range_for(const char *name, uint32_t *start, uint32_t *end)
{
    for (size_t i = 0; i < sizeof(core_stores) / sizeof(core_stores[0]); i++)
    {
        if (!same_core_name(name, core_stores[i].name))
            continue;
        if (start)
            *start = core_stores[i].start;
        if (end)
            *end = core_stores[i].end;
        return true;
    }
    return false;
}

static int entry_find_normalized(const char *wanted, bool strip_go)
{
    for (uint32_t i = 0; i < manifest.count; i++)
    {
        char normalized[CORE_NAME_LEN];
        normalize_name(manifest.entries[i].name, normalized, sizeof(normalized), strip_go);
        if (strcmp(normalized, wanted) == 0)
            return (int)i;
    }
    return -1;
}

static int entry_find(const char *name)
{
    char wanted[CORE_NAME_LEN];
    int index;
    /* Exact normalized match wins, so two ports named "quake" and "quake-go" can coexist; the
 * suffix-insensitive match is only a fallback. */
    normalize_name(name, wanted, sizeof(wanted), false);
    if ((index = entry_find_normalized(wanted, false)) >= 0)
        return index;
    normalize_name(name, wanted, sizeof(wanted), true);
    return entry_find_normalized(wanted, true);
}

/* Streams one image into the pool and returns its offset, or 0 on failure. Always writes to a
 * fresh offset: the old copy stays referenced by the manifest until the new one is written and
 * saved, so a power loss during an update leaves a working core behind. The old range becomes
 * free space as soon as the manifest is flushed, because allocation only looks at entries.
 * `limit` is the exclusive end of the allowed window; `reuse` forces an existing offset (used
 * when replacing a core whose partition tail is the only place large enough for it). */
static uint32_t pool_write_image_file(FILE *file, size_t size, uint32_t *crc_out, uint32_t limit,
                                      uint32_t reuse)
{
    if (!pool || size > UINT32_MAX || size > pool->size)
        return 0;
    uint32_t image_size = (uint32_t)size;
    uint32_t offset = 0;
    if (reuse)
    {
        if (reuse >= CORE_MANIFEST_RESERVED && (reuse % CORE_ALIGN) == 0 &&
            (uint64_t)reuse + image_size <= limit)
            offset = reuse;
    }
    if (!offset)
        offset = pool_alloc(image_size, limit);
    if (!offset)
        return 0;
    uint8_t *buffer = malloc(0x10000);
    if (!buffer)
        return 0;
    uint32_t erase_size = align_up(image_size, CORE_MANIFEST_SECTOR);
    /* The last core can end close enough to the partition end that rounding up to a sector
 * would run past it. */
    if (offset + erase_size > pool->size)
        erase_size = pool->size - offset;
    if (esp_partition_erase_range(pool, offset, erase_size) != ESP_OK ||
        fseek(file, 0, SEEK_SET) != 0)
    {
        free(buffer);
        return 0;
    }
    uint32_t crc = 0;
    uint32_t whole = image_size & ~3u;
    for (uint32_t done = 0; done < whole;)
    {
        uint32_t chunk = RG_MIN(whole - done, 0x10000u);
        if (fread(buffer, 1, chunk, file) != chunk ||
            esp_partition_write(pool, offset + done, buffer, chunk) != ESP_OK)
        {
            free(buffer);
            return 0;
        }
        crc = esp_rom_crc32_le(crc, buffer, chunk);
        done += chunk;
        rg_gui_draw_hourglass();
    }
    uint32_t tail_size = image_size - whole;
    if (tail_size)
    {
        uint8_t tail[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        if (fread(tail, 1, tail_size, file) != tail_size)
        {
            free(buffer);
            return 0;
        }
        crc = esp_rom_crc32_le(crc, tail, tail_size);
        if (esp_partition_write(pool, offset + whole, tail, sizeof(tail)) != ESP_OK)
        {
            free(buffer);
            return 0;
        }
    }
    free(buffer);
    esp_partition_pos_t pos = {.offset = pool->address + offset, .size = image_size};
    esp_image_metadata_t meta = {0};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK)
    {
        RG_LOGE("Image at 0x%x does not verify", (unsigned)offset);
        return 0;
    }
    if (crc_out)
        *crc_out = crc;
    return offset;
}

static void sanitize_name(char *dst, const char *path)
{
    const char *base = rg_basename(path);
    size_t len = strlen(base);
    const char *ext = rg_extension(base);
    if (ext)
        len -= strlen(ext) + 1;
    size_t j = 0;
    for (size_t i = 0; i < len && j < CORE_NAME_LEN - 1; i++)
    {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            dst[j++] = c;
        else if (c >= 'A' && c <= 'Z')
            dst[j++] = c - 'A' + 'a';
        else if (c == ' ' || c == '.')
            dst[j++] = '_';
    }
    dst[j] = 0;
    if (!j)
        snprintf(dst, CORE_NAME_LEN, "core");
}

static void copy_field(char *dst, size_t dst_size, const char *src, size_t src_size)
{
    size_t len = strnlen(src, src_size);
    if (len > dst_size - 1)
        len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int core_pool_install_image(const char *path, const char *expected_name, bool ask_confirm)
{
    if (!pool)
    {
        rg_gui_alert(_("Core manager"), _("Core pool is not available."));
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        rg_gui_alert(_("Core manager"), _("Failed to open the core image."));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        rg_gui_alert(_("Core manager"), _("Failed to read the file."));
        return -1;
    }
    long file_length = ftell(file);
    if (file_length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        rg_gui_alert(_("Core manager"), _("Failed to read the file."));
        return -1;
    }
    size_t size = (size_t)file_length;
    if (size > UINT32_MAX || size > pool->size)
    {
        fclose(file);
        rg_gui_alert(_("Core manager"), _("This core image is larger than the available pool."));
        return -1;
    }
    uint8_t image_header[0x20 + sizeof(esp_app_desc_t)];
    if (size < sizeof(image_header) ||
        fread(image_header, 1, sizeof(image_header), file) != sizeof(image_header) ||
        image_header[0] != CORE_IMAGE_MAGIC)
    {
        fclose(file);
        rg_gui_alert(_("Core manager"), _("This file is not a readable core image."));
        return -1;
    }
    int result = -1;
    char name[CORE_NAME_LEN];
    char old_name[CORE_NAME_LEN] = {0};
    uint8_t *bundle_data = NULL;
    bool has_assets = false;
    core_assets_manifest_t bundled_assets = {0};
    core_bundle_view_t bundle_view = {0};
    has_assets = core_bundle_find_file(file, size, &bundle_data, &bundle_view);
    if (has_assets)
    {
        memcpy(&bundled_assets, bundle_view.bundle + bundle_view.manifest_offset,
               RG_MIN((size_t)bundle_view.manifest_size, sizeof(bundled_assets)));
        snprintf(name, sizeof(name), "%s", bundled_assets.id);
        RG_LOGI("Found core bundle '%s' (%s)", bundled_assets.id, bundled_assets.title);
    }
    else
    {
        sanitize_name(name, path);
    }
    if (expected_name && entry_find(name) != entry_find(expected_name))
    {
        rg_gui_alert(_("Core manager"), _("That image is for a different core."));
        goto cleanup;
    }
    int existing = entry_find(name);
    if (existing >= 0)
        snprintf(old_name, sizeof(old_name), "%s", manifest.entries[existing].name);
    if (existing < 0 && manifest.count >= CORE_MAX_ENTRIES)
    {
        rg_gui_alert(_("Core manager"), _("The pool is full. Remove a core first."));
        goto cleanup;
    }
    /* Cores with a runtime asset store need a fixed range of the pool to themselves, and
 * their own image must stay below it. */
    uint32_t store_start = 0, store_end = 0;
    bool needs_store = store_range_for(name, &store_start, &store_end);
    if (needs_store)
    {
        char blockers[64] = {0};
        for (uint32_t i = 0; i < manifest.count; i++)
        {
            const core_entry_t *other = &manifest.entries[i];
            if ((int)i == existing)
                continue;
            if (other->offset < store_end && other->offset + other->size > store_start)
            {
                size_t used = strlen(blockers);
                snprintf(blockers + used, sizeof(blockers) - used, "%s%s", used ? ", " : "",
                         other->name);
            }
        }
        if (blockers[0])
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s\n%s", _("Free the asset area first. Uninstall:"),
                     blockers);
            rg_gui_alert(_("Core manager"), msg);
            goto cleanup;
        }
    }
    char version[CORE_VER_LEN] = "";
    char build_date[CORE_DATE_LEN] = "unknown";
    char build_time[CORE_TIME_LEN] = "";
    char title[CORE_TITLE_LEN] = {0};
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(image_header + 0x20);
    if (desc->magic_word == ESP_APP_DESC_MAGIC_WORD)
    {
        /* The app descriptor version is a git-describe string such as
 * "1.4x-MEGAPACK-32-g2f1f0-dirty"; it is never shown to the user. Only a version
 * declared in the core's own core.json is displayed. */
        copy_field(build_date, sizeof(build_date), desc->date, sizeof(desc->date));
        copy_field(build_time, sizeof(build_time), desc->time, sizeof(desc->time));
        copy_field(title, sizeof(title), desc->project_name, sizeof(desc->project_name));
    }
    if (!title[0])
        snprintf(title, sizeof(title), "%s", name);
    if (has_assets && bundled_assets.title[0])
        copy_field(title, sizeof(title), bundled_assets.title, sizeof(bundled_assets.title));
    if (has_assets)
        copy_field(version, sizeof(version), bundled_assets.version,
                   sizeof(bundled_assets.version));
    char display[CORE_TITLE_LEN + CORE_VER_LEN + 8];
    core_display_name(display, sizeof(display), title, version);
    char image_size[40], free_size[24];
    uint32_t free_bytes =
        core_pool_total() > core_pool_used() ? core_pool_total() - core_pool_used() : 0;
    format_footprint(image_size, sizeof(image_size), (uint32_t)size,
                     needs_store ? store_end - store_start : 0);
    format_pool_mb(free_size, sizeof(free_size), free_bytes);
    char install_info[256];
    snprintf(install_info, sizeof(install_info),
             "%s\nID: %s\nBuild: %s %s\nSize: %s\nPool free: %s", display, name, build_date,
             build_time, image_size, free_size);
    rg_gui_option_t install_options[] = {
        {0, install_info, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {1, existing >= 0 ? _("Replace") : _("Install"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, _("Cancel"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };
    if (ask_confirm && rg_gui_dialog(existing >= 0 ? _("Replace core?") : _("Install core?"),
                                     install_options, 1) != 1)
        goto cleanup;
    /* A store core has no room for a second copy, so replacing it reuses its own offset. The
 * bootloader verifies the image, so a power loss mid-write still falls back to the
 * launcher. */
    uint32_t reuse = 0;
    if (needs_store && existing >= 0)
        reuse = manifest.entries[existing].offset;
    uint32_t image_crc = 0;
    uint32_t offset = pool_write_image_file(file, size, &image_crc,
                                            needs_store ? store_start : pool->size, reuse);
    if (!offset)
    {
        RG_LOGE("Core '%s' not written: size=%u free=%u limit=0x%x", name, (unsigned)size,
                (unsigned)(core_pool_total() - core_pool_used()),
                (unsigned)(needs_store ? store_start : pool->size));
        rg_gui_alert(
            _("Core manager"),
            needs_store
                ? _("Not enough room below the asset area. Uninstall the other cores first.")
                : _("The core could not be written. Check pool space and SD card."));
        goto cleanup;
    }
    int index = existing;
    if (index < 0)
        index = (int)manifest.count++;
    core_entry_t previous = manifest.entries[index];
    core_entry_t *e = &manifest.entries[index];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, CORE_NAME_LEN - 1);
    e->offset = offset;
    e->size = size;
    e->crc32 = image_crc;
    if (needs_store)
    {
        e->reserve_offset = store_start;
        e->reserve_size = store_end - store_start;
    }
    copy_field(e->version, sizeof(e->version), version, sizeof(version));
    copy_field(e->date, sizeof(e->date), build_date, sizeof(build_date));
    copy_field(e->time, sizeof(e->time), build_time, sizeof(build_time));
    copy_field(e->title, sizeof(e->title), title, sizeof(title));
    if (!manifest_flush())
    {
        RG_LOGE("Failed to save the core manifest");
        rg_gui_alert(_("Core manager"), _("Failed to save the core list."));
        /* Put the previous entry back so the in-memory list still matches what is really
 * in flash. */
        manifest.entries[index] = previous;
        if (existing < 0)
            manifest.count--;
        goto cleanup;
    }
    if (old_name[0] && strcmp(old_name, name) != 0)
    {
        char old_path[RG_PATH_MAX + 1];
        core_asset_path(old_path, sizeof(old_path), old_name);
        rg_storage_delete(old_path);
    }
    char asset_path[RG_PATH_MAX + 1];
    core_asset_path(asset_path, sizeof(asset_path), name);
    if (has_assets)
    {
        if (!rg_storage_write_file(asset_path, bundle_data, bundle_view.total_size, 0))
        {
            rg_storage_delete(asset_path);
            RG_LOGE("Failed to save core metadata and art for '%s'", name);
        }
    }
    else
    {
        rg_storage_delete(asset_path);
    }
    RG_LOGI("Installed core '%s' (%s) at 0x%x, %u KB", e->name, e->version, (unsigned)e->offset,
            (unsigned)(e->size / 1024));
    result = index;
cleanup:
    free(bundle_data);
    fclose(file);
    return result;
}

int core_pool_install(const char *path)
{
    return core_pool_install_image(path, NULL, true);
}

bool core_pool_select(int index)
{
    if (!pool)
        return false;
    if (index < -1 || index >= (int)manifest.count)
        return false;
    manifest.selected = index;
    return manifest_flush();
}

/* rg_system hooks: the launcher uses these to treat installed cores exactly like apps that live
 * in the partition table. */
bool core_pool_have_app(const char *partition)
{
    return pool && entry_find(partition) >= 0;
}

bool core_pool_entry_matches_app(int index, const char *partition)
{
    return pool && partition && index >= 0 && index < (int)manifest.count &&
           entry_find(partition) == index;
}

bool core_pool_switch_app(const char *partition, const char *name, const char *args, int save_slot,
                          uint32_t flags)
{
    int index = entry_find(partition);
    if (index < 0)
        return false;
    /* rg_system already saved the boot name/args; we only have to pick which image the
 * bootloader should start. */
    return core_pool_select(index);
}

bool core_pool_remove(int index)
{
    if (!pool || index < 0 || index >= (int)manifest.count)
        return false;
    char removed_name[CORE_NAME_LEN];
    snprintf(removed_name, sizeof(removed_name), "%s", manifest.entries[index].name);
    if (manifest.selected == index)
        manifest.selected = -1;
    else if (manifest.selected > index)
        manifest.selected--;
    memmove(&manifest.entries[index], &manifest.entries[index + 1],
            (manifest.count - index - 1) * sizeof(core_entry_t));
    manifest.count--;
    memset(&manifest.entries[manifest.count], 0, sizeof(core_entry_t));
    if (!manifest_flush())
        return false;
    char path[RG_PATH_MAX + 1];
    core_asset_path(path, sizeof(path), removed_name);
    rg_storage_delete(path);
    return true;
}

bool core_pool_format(void)
{
    if (!pool)
        return false;
    /* Keep the sequence monotonic so the older, non-empty manifest copy cannot win again when
 * the launcher boots after formatting. */
    uint32_t removed_count = manifest.count;
    uint32_t previous_seq = manifest.seq;
    manifest_reset();
    manifest.seq = previous_seq;
    if (!manifest_flush())
        return false;
    /* Names in older manifests may be corrupt, and old failed installs can leave orphaned
 * sidecars, so clear the dedicated asset directory itself. */
    if (rg_storage_exists(CORE_SD_ASSET_FOLDER) && !rg_storage_delete(CORE_SD_ASSET_FOLDER))
        RG_LOGW("Failed to clear core artwork sidecars");
    if (!rg_storage_mkdir(CORE_SD_ASSET_FOLDER))
        RG_LOGW("Failed to recreate core artwork folder");
    RG_LOGI("Core pool formatted; %u core entries removed", (unsigned)removed_count);
    return true;
}

/* ------------------------------------------------------------------ */
/* Core manager UI                                                     */
/* ------------------------------------------------------------------ */

#define CORE_KIND_CORE   0
#define CORE_KIND_INSTALL 1
#define CORE_KIND_FORMAT 2

#define CORE_UI_MAX (CORE_MAX_ENTRIES + 32)

typedef struct
{
    char file[96];                 /* basename under CORE_SD_FOLDER, "" if none */
    char name[CORE_NAME_LEN];
    char title[CORE_TITLE_LEN];
    char version[CORE_VER_LEN];
    char date[CORE_DATE_LEN];
    char time[CORE_TIME_LEN];
    uint32_t size;
    int installed;                 /* manifest index, -1 = not installed */
    bool fits;                     /* enough pool space to install it */
    bool selected;                 /* checked in the install screen */
    uint8_t kind;
} core_item_t;

typedef struct
{
    int x, y, w, h;
    int line, row_h, head_h, foot_h, rows;
} core_layout_t;

typedef struct
{
    const char *title;
    const char *primary;           /* footer button, NULL = hint line only */
    const char *only_id;           /* install screen: restrict to one core */
    core_item_t *items;
    int count;
    int sel;
    int top;
    bool install_list;             /* true = SD cores, false = installed cores */
    bool checkboxes;
    bool footer;
    int footer_sel;
    bool details;
    int detail_index;
    int detail_action;              /* 0 = install/replace, 1 = cancel */
} core_ui_t;

/* One offscreen surface shared by every core-manager screen (refcounted, so
 * the install screen can be opened on top of the manager). Drawing there and
 * submitting once per frame keeps the list from tearing while scrolling. */
static rg_surface_t *core_ui_surface;
static int core_ui_refs;

static void core_install_run(const char *only_id);
static void core_pool_details(int index);

static core_layout_t core_ui_layout(void)
{
    core_layout_t L;
    int sw = rg_display_get_width();
    int sh = rg_display_get_height();

    L.line = TEXT_RECT("Ag", 0).height;
    L.x = 4;
    L.y = 4;
    L.w = sw - 8;
    L.h = sh - 8;
    L.head_h = L.line + 10;
    /* Leave room for a control-hint line above the footer buttons. */
    L.foot_h = 2 * L.line + 14;
    L.row_h = L.line + 4;
    L.rows = (L.h - L.head_h - L.foot_h) / L.row_h;
    if (L.rows < 1)
        L.rows = 1;
    return L;
}

static bool core_ui_begin(void)
{
    if (!core_ui_surface)
        core_ui_surface = rg_surface_create(rg_display_get_width(), rg_display_get_height(),
                                            RG_PIXEL_565_LE, MEM_SLOW);
    if (!core_ui_surface)
        return false;
    core_ui_refs++;
    rg_gui_set_surface(core_ui_surface);
    return true;
}

/* Dialogs and alerts must draw straight to the panel, otherwise they would
 * land in our surface and never be shown. */
static void core_ui_pause(void)
{
    rg_gui_set_surface(NULL);
}

static void core_ui_resume(void)
{
    if (core_ui_surface)
        rg_gui_set_surface(core_ui_surface);
}

static void core_ui_end(void)
{
    rg_gui_set_surface(NULL);
    if (--core_ui_refs <= 0)
    {
        core_ui_refs = 0;
        rg_surface_free(core_ui_surface);
        core_ui_surface = NULL;
        rg_display_force_redraw();
    }
}

/* Reads only the bundle header of a .bin on SD: the id, title and version are
 * enough to build the install list, and the full CRC check happens when the
 * image is actually installed. The bundle lives near the start of the image,
 * so the scan normally stops after the first 64 KB. */
static bool core_bundle_peek_file(const char *path, char *id, size_t id_size, char *title,
                                  size_t title_size, char *version, size_t version_size)
{
    const size_t chunk = 0x8000;
    const size_t overlap = sizeof(CORE_ASSET_MAGIC) - 1;
    uint8_t *buf = malloc(chunk + overlap);
    FILE *file = fopen(path, "rb");
    bool found = false;

    if (!file || !buf)
        goto done;
    if (fseek(file, 0, SEEK_END) != 0)
        goto done;

    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
        goto done;

    size_t offset = 0, carry = 0;
    while (offset < (size_t)length && !found)
    {
        size_t amount = RG_MIN(chunk, (size_t)length - offset);
        if (fread(buf + carry, 1, amount, file) != amount)
            break;

        size_t available = carry + amount;
        for (size_t i = 0; i + overlap <= available && !found; i++)
        {
            if (memcmp(buf + i, CORE_ASSET_MAGIC, overlap) != 0)
                continue;

            size_t at = offset - carry + i;
            uint8_t header[sizeof(core_bundle_header_t)];
            if (at + sizeof(header) > (size_t)length || fseek(file, (long)at, SEEK_SET) != 0 ||
                fread(header, 1, sizeof(header), file) != sizeof(header))
                continue;

            uint16_t ver = read_le16(header + 8);
            uint16_t hsize = read_le16(header + 10);
            uint32_t total = read_le32(header + 12);
            uint32_t msize = read_le32(header + 16);

            if (hsize != sizeof(core_bundle_header_t))
                continue;
            if (ver == 1)
            {
                if (msize != CORE_ASSET_MANIFEST_V1_SIZE)
                    continue;
            }
            else if (ver == CORE_ASSET_VERSION)
            {
                if (msize != sizeof(core_assets_manifest_t))
                    continue;
            }
            else
                continue;
            if (total < hsize + msize || total > CORE_ASSET_MAX_BUNDLE ||
                at + total > (size_t)length || at + hsize + msize > (size_t)length)
                continue;

            core_assets_manifest_t m = {0};
            if (fread((uint8_t *)&m, 1, msize, file) != msize)
                continue;
            if (!valid_core_id(m.id, sizeof(m.id)) || !m.title[0])
                continue;

            copy_field(id, id_size, m.id, sizeof(m.id));
            copy_field(title, title_size, m.title, sizeof(m.title));
            copy_field(version, version_size, m.version, sizeof(m.version));
            found = true;
        }

        offset += amount;
        carry = RG_MIN(overlap, available);
        memmove(buf, buf + available - carry, carry);
        if (fseek(file, (long)offset, SEEK_SET) != 0)
            break;
    }

done:
    free(buf);
    if (file)
        fclose(file);
    return found;
}

static void core_peek_build_info(const char *path, char *date, size_t date_size, char *time,
                                 size_t time_size)
{
    snprintf(date, date_size, "%s", "unknown");
    time[0] = 0;

    FILE *file = fopen(path, "rb");
    if (!file)
        return;

    esp_app_desc_t desc = {0};
    if (fseek(file, 0x20, SEEK_SET) == 0 && fread(&desc, 1, sizeof(desc), file) == sizeof(desc) &&
        desc.magic_word == ESP_APP_DESC_MAGIC_WORD)
    {
        copy_field(date, date_size, desc.date, sizeof(desc.date));
        copy_field(time, time_size, desc.time, sizeof(desc.time));
    }
    fclose(file);
}

/* Same test the installer does before writing: is there a hole big enough for
 * this image (below the asset area for cores that need one)? */
static bool core_fits(const char *name, uint32_t size)
{
    uint32_t store_start = 0, store_end = 0;

    if (manifest.count >= CORE_MAX_ENTRIES || size > pool->size)
        return false;

    if (store_range_for(name, &store_start, &store_end))
    {
        for (uint32_t i = 0; i < manifest.count; i++)
        {
            const core_entry_t *e = &manifest.entries[i];
            if (e->offset < store_end && e->offset + e->size > store_start)
                return false;
            if (e->reserve_size && e->reserve_offset < store_end &&
                e->reserve_offset + e->reserve_size > store_start)
                return false;
        }
        return pool_alloc(size, store_start) != 0;
    }

    return pool_alloc(size, pool->size) != 0;
}

typedef struct
{
    core_item_t *items;
    int count;
    int max;
    const char *only_id;
} core_scan_ctx_t;

static int core_scan_cb(const rg_scandir_t *file, void *arg)
{
    core_scan_ctx_t *ctx = arg;
    char id[CORE_NAME_LEN], title[CORE_TITLE_LEN], version[CORE_VER_LEN];

    if (ctx->count >= ctx->max)
        return RG_SCANDIR_STOP;
    if (!rg_extension_match(file->path, "bin"))
        return RG_SCANDIR_CONTINUE;
    if (strlen(file->basename) >= sizeof(ctx->items[0].file))
        return RG_SCANDIR_CONTINUE;

    /* dirent does not always provide the file size, so ask for it explicitly. */
    rg_stat_t st = rg_storage_stat(file->path);
    if (!st.is_file || st.size < 0x1000)
        return RG_SCANDIR_CONTINUE;

    if (!core_bundle_peek_file(file->path, id, sizeof(id), title, sizeof(title), version,
                               sizeof(version)))
        return RG_SCANDIR_CONTINUE;
    if (ctx->only_id && !same_core_name(id, ctx->only_id))
        return RG_SCANDIR_CONTINUE;

    for (int i = 0; i < ctx->count; i++)
        if (same_core_name(ctx->items[i].name, id))
            return RG_SCANDIR_CONTINUE;

    core_item_t *it = &ctx->items[ctx->count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->file, sizeof(it->file), "%s", file->basename);
    snprintf(it->name, sizeof(it->name), "%s", id);
    snprintf(it->title, sizeof(it->title), "%s", title);
    snprintf(it->version, sizeof(it->version), "%s", version);
    core_peek_build_info(file->path, it->date, sizeof(it->date), it->time, sizeof(it->time));
    it->size = (uint32_t)st.size;
    it->installed = entry_find(id);
    it->fits = it->installed >= 0 || core_fits(id, it->size);
    it->kind = CORE_KIND_CORE;
    return RG_SCANDIR_CONTINUE;
}

/* Cores sitting in /sd/cores that could be installed. */
static int core_scan_sd(core_item_t *items, int max, const char *only_id)
{
    core_scan_ctx_t ctx = {items, 0, max, only_id};

    if (rg_storage_ready())
        rg_storage_scandir(CORE_SD_FOLDER, core_scan_cb, &ctx,
                           RG_SCANDIR_FILES | RG_SCANDIR_SORT | RG_SCANDIR_STAT);

    return ctx.count;
}

/* Cores already installed in the pool, followed by the action rows. */
static int core_scan_installed(core_item_t *items, int max)
{
    int count = 0;

    for (uint32_t i = 0; i < manifest.count && count < max - 2; i++)
    {
        const core_entry_t *e = &manifest.entries[i];
        core_item_t *it = &items[count++];
        memset(it, 0, sizeof(*it));
        copy_field(it->name, sizeof(it->name), e->name, sizeof(e->name));
        copy_field(it->title, sizeof(it->title), e->title, sizeof(e->title));
        copy_field(it->version, sizeof(it->version), e->version, sizeof(e->version));
        it->size = e->size;
        it->installed = (int)i;
        it->fits = true;
        it->kind = CORE_KIND_CORE;
    }

    core_item_t *it = &items[count++];
    memset(it, 0, sizeof(*it));
    it->kind = CORE_KIND_INSTALL;
    it->fits = true;
    snprintf(it->title, sizeof(it->title), "%s", _("Install from SD..."));

    it = &items[count++];
    memset(it, 0, sizeof(*it));
    it->kind = CORE_KIND_FORMAT;
    it->fits = true;
    snprintf(it->title, sizeof(it->title), "%s", _("Format pool..."));

    return count;
}

static void core_ui_reload(core_ui_t *ui)
{
    if (ui->install_list)
        ui->count = core_scan_sd(ui->items, CORE_UI_MAX, ui->only_id);
    else
        ui->count = core_scan_installed(ui->items, CORE_UI_MAX);

    if (ui->sel >= ui->count)
        ui->sel = ui->count - 1;
    if (ui->sel < 0)
        ui->sel = 0;
    if (ui->top > ui->sel)
        ui->top = ui->sel;
}

static void core_ui_draw(const core_ui_t *ui)
{
    core_layout_t L = core_ui_layout();
    rg_color_t bg = rg_gui_get_theme_color("dialog", "background", C_NAVY);
    rg_color_t border = rg_gui_get_theme_color("dialog", "border", C_DIM_GRAY);
    rg_color_t header = rg_gui_get_theme_color("dialog", "header", C_WHITE);
    rg_color_t fg = rg_gui_get_theme_color("dialog", "item_standard", C_WHITE);

    /* Clear the surface (rg_display_clear would hit the panel instead). */
    rg_gui_draw_rect(0, 0, rg_display_get_width(), rg_display_get_height(), 0, C_NONE, C_BLACK);
    rg_gui_draw_rect(L.x, L.y, L.w, L.h, 2, border, bg);

    rg_gui_draw_text(L.x + 10, L.y + 4, 0, ui->title, header, bg, 0);

    char free_mb[16], total_mb[16], space[48];
    uint32_t total = core_pool_total(), used = core_pool_used();
    format_mb_number(free_mb, sizeof(free_mb), total > used ? total - used : 0);
    format_mb_number(total_mb, sizeof(total_mb), total);
    snprintf(space, sizeof(space), "%s / %s MB", free_mb, total_mb);
    int space_w = TEXT_RECT(space, 0).width;
    rg_gui_draw_text(L.x + L.w - 10 - space_w, L.y + 4, space_w, space, C_GRAY, bg, 0);

    rg_gui_draw_rect(L.x + 4, L.y + L.head_h - 4, L.w - 8, 1, 0, C_NONE, border);

    if (ui->details && ui->detail_index >= 0 && ui->detail_index < ui->count)
    {
        const core_item_t *it = &ui->items[ui->detail_index];
        char label[CORE_TITLE_LEN + CORE_VER_LEN + 8];
        char size_text[40], free_text[24], line[96];
        int x = L.x + 12;
        int y = L.y + L.head_h + 6;
        int step = L.line + 4;
        uint32_t used = core_pool_used();

        core_display_name(label, sizeof(label), it->title, it->version);
        format_footprint(size_text, sizeof(size_text), it->size, 0);
        format_pool_mb(free_text, sizeof(free_text), total > used ? total - used : 0);
        rg_gui_draw_text(x, y, L.w - 24, label, C_WHITE, C_TRANSPARENT, 0);
        y += step;
        snprintf(line, sizeof(line), "ID: %s", it->name);
        rg_gui_draw_text(x, y, L.w - 24, line, C_GRAY, C_TRANSPARENT, 0);
        y += step;
        snprintf(line, sizeof(line), "Build: %s %s", it->date, it->time);
        rg_gui_draw_text(x, y, L.w - 24, line, C_GRAY, C_TRANSPARENT, 0);
        y += step;
        snprintf(line, sizeof(line), "Size: %s", size_text);
        rg_gui_draw_text(x, y, L.w - 24, line, C_GRAY, C_TRANSPARENT, 0);
        y += step;
        snprintf(line, sizeof(line), "Pool free: %s", free_text);
        rg_gui_draw_text(x, y, L.w - 24, line, C_GRAY, C_TRANSPARENT, 0);
        if (it->installed >= 0)
        {
            y += step;
            rg_gui_draw_text(x, y, L.w - 24, _("Installed core: this will replace it"), C_GRAY,
                             C_TRANSPARENT, 0);
        }

        int fy = L.y + L.h - L.foot_h;
        const char *hint = _("[LEFT/RIGHT] Choose    [A] Confirm    [B] Back");
        int hw = TEXT_RECT(hint, 0).width;
        int hy = fy + 2;
        rg_gui_draw_text(L.x + (L.w - hw) / 2, hy, hw, hint, C_DIM_GRAY, C_TRANSPARENT, 0);

        const char *install = (it->installed >= 0) ? _("[ Replace ]") : _("[ Install ]");
        const char *cancel = _("[ Cancel ]");
        int iw = TEXT_RECT(install, 0).width;
        int cw = TEXT_RECT(cancel, 0).width;
        int gap = 18;
        int bx = L.x + (L.w - (iw + cw + gap)) / 2;
        int by = fy + L.line + 7;
        bool install_focus = ui->detail_action == 0;
        rg_gui_draw_text(bx, by, iw, install, install_focus ? bg : C_WHITE,
                         install_focus ? C_WHITE : C_TRANSPARENT, 0);
        rg_gui_draw_text(bx + iw + gap, by, cw, cancel, install_focus ? C_WHITE : bg,
                         install_focus ? C_TRANSPARENT : C_WHITE, 0);
        return;
    }

    int box_w = ui->checkboxes ? TEXT_RECT("[x]", 0).width + 8 : 0;
    int size_w = TEXT_RECT("000.00 MB", 0).width + 4;
    int text_x = L.x + 10 + box_w;
    int text_w = L.w - 20 - box_w - size_w - 4;

    if (text_w < 20)
        text_w = 20;

    for (int i = 0; i < L.rows; i++)
    {
        int idx = ui->top + i;

        if (idx >= ui->count)
            break;

        const core_item_t *it = &ui->items[idx];
        int y = L.y + L.head_h + i * L.row_h;
        bool focus = !ui->footer && (idx == ui->sel);
        rg_color_t color, tfg;

        if (it->kind == CORE_KIND_CORE)
            color = (it->installed >= 0) ? C_GREEN : (it->fits ? fg : C_RED);
        else if (it->kind == CORE_KIND_FORMAT)
            color = C_ORANGE;
        else
            color = fg;

        if (idx > 0 && ui->items[idx - 1].kind == CORE_KIND_CORE && it->kind != CORE_KIND_CORE)
            rg_gui_draw_rect(L.x + 10, y + L.row_h / 2, L.w - 20, 1, 0, C_NONE, border);

        rg_gui_draw_rect(L.x + 6, y, L.w - 12, L.row_h - 1, 0, C_NONE, focus ? color : bg);

        /* Text backgrounds stay transparent: rg_gui_draw_text paints its whole
         * box otherwise, which used to wipe the label drawn before it. */
        tfg = focus ? bg : color;

        if (it->kind == CORE_KIND_CORE)
        {
            char label[CORE_TITLE_LEN + CORE_VER_LEN + 8];
            char size_str[20];

            core_display_name(label, sizeof(label), it->title, it->version);
            format_pool_mb(size_str, sizeof(size_str), it->size);

            if (ui->checkboxes)
            {
                const char *box = (it->installed >= 0) ? "[*]" : (it->selected ? "[x]" : "[ ]");
                rg_gui_draw_text(L.x + 10, y + 2, box_w, box, tfg, C_TRANSPARENT, 0);
            }

            rg_gui_draw_text(text_x, y + 2, text_w, label, tfg, C_TRANSPARENT, 0);

            int size_w2 = TEXT_RECT(size_str, 0).width;
            rg_gui_draw_text(L.x + L.w - 10 - size_w2, y + 2, size_w2, size_str, tfg,
                             C_TRANSPARENT, 0);
        }
        else
        {
            rg_gui_draw_text(L.x + 10, y + 2, L.w - 20, it->title, tfg, C_TRANSPARENT, 0);
        }
    }

    if (!ui->count)
    {
        const char *empty = ui->install_list ? _("No cores found on the SD card")
                                             : _("No cores installed");
        rg_gui_draw_text(L.x + 10, L.y + L.head_h + 2, L.w - 20, empty, C_DIM_GRAY, C_TRANSPARENT,
                         0);
    }

    /* Footer */
    int fy = L.y + L.h - L.foot_h;
    int by = fy + (L.foot_h - L.line) / 2;

    rg_gui_draw_rect(L.x + 4, fy, L.w - 8, 1, 0, C_NONE, border);

    if (ui->checkboxes)
    {
        const char *hint = _("[SELECT] Mark  [START] Install  [A] Info");
        int hw = TEXT_RECT(hint, 0).width;
        rg_gui_draw_text(L.x + (L.w - hw) / 2, fy + 2, hw, hint, C_DIM_GRAY, C_TRANSPARENT, 0);
        by = fy + L.line + 7;
    }

    if (!ui->primary)
    {
        const char *hint = ui->install_list ? _("[SELECT/START] Mark  [A] Install  [B] Back")
                                            : _("[A] Select    [B] Back");
        int hw = TEXT_RECT(hint, 0).width;
        rg_gui_draw_text(L.x + (L.w - hw) / 2, by, hw, hint, C_DIM_GRAY, C_TRANSPARENT, 0);
        return;
    }

    int selected = 0;

    for (int i = 0; i < ui->count; i++)
        if (ui->items[i].selected)
            selected++;

    char b0[48], b1[32];

    if (ui->checkboxes)
    {
        if (selected)
            snprintf(b0, sizeof(b0), _("[ Install selected (%d) ]"), selected);
        else
            snprintf(b0, sizeof(b0), "%s", _("[ Install selected ]"));
    }
    else
    {
        snprintf(b0, sizeof(b0), "[ %s ]", ui->primary);
    }

    snprintf(b1, sizeof(b1), "%s", _("[ Cancel ]"));

    int bw0 = TEXT_RECT(b0, 0).width, bw1 = TEXT_RECT(b1, 0).width;
    int gap = 14;
    int bx = L.x + (L.w - (bw0 + bw1 + gap)) / 2;
    bool f0 = ui->footer && ui->footer_sel == 0;
    bool f1 = ui->footer && ui->footer_sel == 1;
    rg_color_t c0 = (!ui->checkboxes || selected || f0) ? fg : C_DIM_GRAY;

    rg_gui_draw_text(bx, by, bw0, b0, f0 ? bg : c0, f0 ? c0 : C_TRANSPARENT, 0);
    rg_gui_draw_text(bx + bw0 + gap, by, bw1, b1, f1 ? bg : fg, f1 ? fg : C_TRANSPARENT, 0);
}

static void core_ui_loop(core_ui_t *ui, bool (*activate)(core_ui_t *ui, int index))
{
    core_layout_t L = core_ui_layout();
    bool redraw = true, exit = false;
    rg_key_t prev = 0;
    int64_t repeat_at = 0;

    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);

    while (!exit)
    {
        if (redraw)
        {
            core_ui_draw(ui);
            rg_display_submit(core_ui_surface, 0);
            redraw = false;
        }

        rg_task_delay(20);
        rg_system_tick(0);

        rg_key_t joy = rg_input_read_gamepad();
        rg_key_t edge = joy & ~prev;
        prev = joy;

        if (edge)
            repeat_at = rg_system_timer() + 400000;
        else if ((joy & (RG_KEY_UP | RG_KEY_DOWN)) && (rg_system_timer() - repeat_at) >= 0)
        {
            edge = joy & (RG_KEY_UP | RG_KEY_DOWN);
            repeat_at = rg_system_timer() + 130000;
        }

        if (!edge)
            continue;

        if (ui->details)
        {
            if (edge & (RG_KEY_B | RG_KEY_MENU | RG_KEY_OPTION))
            {
                ui->details = false;
                redraw = true;
            }
            else if (edge & (RG_KEY_LEFT | RG_KEY_RIGHT | RG_KEY_UP | RG_KEY_DOWN))
            {
                ui->detail_action ^= 1;
                redraw = true;
            }
            else if (edge & RG_KEY_A)
            {
                if (ui->detail_action == 1)
                {
                    ui->details = false;
                }
                else
                {
                    int install_index = ui->detail_index;
                    core_ui_pause();
                    exit = activate(ui, install_index);
                    core_ui_resume();
                    prev = rg_input_read_gamepad();
                    ui->details = false;
                    core_ui_reload(ui);
                }
                redraw = true;
            }
            continue;
        }

        if (edge & (RG_KEY_B | RG_KEY_MENU | RG_KEY_OPTION))
            break;

        if (ui->footer)
        {
            if (edge & (RG_KEY_LEFT | RG_KEY_RIGHT))
            {
                ui->footer_sel ^= 1;
                redraw = true;
            }
            else if (edge & RG_KEY_UP)
            {
                ui->footer = false;
                ui->sel = ui->count - 1;
                redraw = true;
            }
            else if (edge & RG_KEY_DOWN)
            {
                ui->footer = false;
                ui->sel = 0;
                ui->top = 0;
                redraw = true;
            }
            else if (ui->checkboxes && (edge & RG_KEY_START))
            {
                core_ui_pause();
                exit = activate(ui, -1);
                core_ui_resume();
                prev = rg_input_read_gamepad();
                redraw = true;
            }
            else if (edge & RG_KEY_A)
            {
                if (ui->footer_sel == 0)
                {
                    core_ui_pause();
                    exit = activate(ui, -1);
                    core_ui_resume();
                }
                else
                {
                    exit = true;
                }
                /* A nested dialog can time out while its closing key is still held.
                 * Treat that held state as the new baseline so B cannot also close
                 * this list screen. */
                prev = rg_input_read_gamepad();
                redraw = true;
            }

            if (ui->sel < ui->top)
                ui->top = ui->sel;
            if (ui->sel >= ui->top + L.rows)
                ui->top = ui->sel - L.rows + 1;
            continue;
        }

        if (edge & RG_KEY_UP)
        {
            if (ui->sel > 0)
                ui->sel--;
            else
            {
                ui->footer = true;
                ui->footer_sel = 1;
            }
            redraw = true;
        }
        else if (edge & RG_KEY_DOWN)
        {
            if (ui->sel < ui->count - 1)
                ui->sel++;
            else
            {
                ui->footer = true;
                ui->footer_sel = 0;
            }
            redraw = true;
        }
        else if (ui->checkboxes && (edge & RG_KEY_SELECT))
        {
            if (ui->sel >= 0 && ui->sel < ui->count &&
                ui->items[ui->sel].kind == CORE_KIND_CORE)
                ui->items[ui->sel].selected = !ui->items[ui->sel].selected;
            redraw = true;
        }
        else if (ui->checkboxes && (edge & RG_KEY_START))
        {
            /* Quick shortcut; the footer button remains available as well. */
            core_ui_pause();
            exit = activate(ui, -1);
            core_ui_resume();
            prev = rg_input_read_gamepad();
            redraw = true;
        }
        else if (edge & RG_KEY_A)
        {
            if (ui->install_list && ui->sel >= 0 && ui->sel < ui->count &&
                ui->items[ui->sel].kind == CORE_KIND_CORE)
            {
                ui->details = true;
                ui->detail_index = ui->sel;
                ui->detail_action = 0;
            }
            else
            {
                core_ui_pause();
                exit = activate(ui, ui->sel);
                core_ui_resume();
                prev = rg_input_read_gamepad();
            }
            redraw = true;
        }

        if (ui->sel < ui->top)
            ui->top = ui->sel;
        if (ui->sel >= ui->top + L.rows)
            ui->top = ui->sel - L.rows + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Install from SD                                                     */
/* ------------------------------------------------------------------ */

static bool core_install_one(core_item_t *it)
{
    char path[RG_PATH_MAX + 1];

    snprintf(path, sizeof(path), CORE_SD_FOLDER "/%s", it->file);
    rg_gui_draw_message(_("Installing %s..."), it->title);
    rg_task_delay(30);

    return core_pool_install_image(path, it->name, false) >= 0;
}

static bool core_install_activate(core_ui_t *ui, int index)
{
    int picked = 0, done = 0;

    /* The filtered reinstall screen contains exactly one core and has no checkboxes. */
    if (!ui->checkboxes && index < 0 && ui->count == 1)
        index = 0;

    /* Called after the in-list details page confirms Install or Replace. */
    if (index >= 0 && index < ui->count && ui->items[index].kind == CORE_KIND_CORE)
    {
        core_item_t *it = &ui->items[index];
        char path[RG_PATH_MAX + 1];
        snprintf(path, sizeof(path), CORE_SD_FOLDER "/%s", it->file);
        if (core_pool_install_image(path, it->name, false) >= 0 &&
            rg_gui_confirm(_("Core installed"), _("Restart now to refresh the launcher?"), false))
            rg_system_restart();
        core_ui_reload(ui);
        return false;
    }

    /* The footer's install action is the only path that consumes the checked set. */
    if (ui->checkboxes)
    {
        uint32_t total_size = 0;
        for (int i = 0; i < ui->count; i++)
        {
            if (!ui->items[i].selected || ui->items[i].kind != CORE_KIND_CORE)
                continue;
            picked++;
            total_size += ui->items[i].size;
        }

        if (!picked)
        {
            rg_gui_alert(_("Install from SD"), _("Select at least one core first."));
            return false;
        }

        char size_text[32], free_text[32], summary[128];
        format_pool_mb(size_text, sizeof(size_text), total_size);
        uint32_t used = core_pool_used();
        format_pool_mb(free_text, sizeof(free_text), core_pool_total() > used
                                                        ? core_pool_total() - used
                                                        : 0);
        snprintf(summary, sizeof(summary), _("Cores: %d\nImage total: %s\nPool free: %s"),
                 picked, size_text, free_text);
        if (!rg_gui_confirm(_("Install selected cores?"), summary, false))
            return false;

        for (int i = 0; i < ui->count; i++)
        {
            if (ui->items[i].selected && ui->items[i].kind == CORE_KIND_CORE &&
                core_install_one(&ui->items[i]))
                done++;
        }
    }

    ui->footer = false;
    ui->sel = 0;
    ui->top = 0;
    core_ui_reload(ui);

    if (done && rg_gui_confirm(_("Core installed"), _("Restart now to refresh the launcher?"), false))
        rg_system_restart();

    return false;
}

static void core_install_run(const char *only_id)
{
    core_item_t *items = calloc(CORE_UI_MAX, sizeof(core_item_t));

    if (!items)
    {
        rg_gui_alert(_("Core manager"), _("Not enough memory."));
        return;
    }

    core_ui_t ui = {
        .title = only_id ? _("Replace core") : _("Install from SD"),
        .primary = _("Install"),
        .only_id = only_id,
        .items = items,
        .install_list = true,
        .checkboxes = only_id == NULL,
    };

    core_ui_reload(&ui);

    if (!ui.count)
    {
        rg_gui_alert(ui.title, only_id ? _("That core is not on the SD card.")
                                       : _("No cores found on the SD card."));
        free(items);
        return;
    }

    if (!core_ui_begin())
    {
        rg_gui_alert(_("Core manager"), _("Not enough memory."));
        free(items);
        return;
    }

    core_ui_loop(&ui, core_install_activate);
    core_ui_end();
    free(items);
}

/* ------------------------------------------------------------------ */
/* Installed cores                                                     */
/* ------------------------------------------------------------------ */

static void core_pool_details(int index)
{
    const core_entry_t *e = core_pool_entry(index);

    if (!e)
        return;

    char label[CORE_TITLE_LEN + CORE_VER_LEN + 8];
    char info[192], image_size[40];

    core_display_name(label, sizeof(label), e->title, e->version);
    format_footprint(image_size, sizeof(image_size), e->size, e->reserve_size);
    snprintf(info, sizeof(info), "%s\nID: %s\nBuild: %s %s\nSize: %s", label, e->name, e->date,
             e->time, image_size);

    rg_gui_option_t options[] = {
        {0, info, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {1, _("Launch"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, _("Reinstall / replace"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3, _("Uninstall"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };

    intptr_t sel = rg_gui_dialog(label, options, 1);

    if (sel == 1)
    {
        if (core_pool_select(index))
            rg_system_restart();
        rg_gui_alert(_("Core manager"), _("Failed to select the core."));
    }
    else if (sel == 2)
    {
        core_install_run(e->name);
    }
    else if (sel == 3)
    {
        if (rg_gui_confirm(_("Uninstall core"), _("Remove this core from the pool?"), false))
        {
            if (core_pool_remove(index))
            {
                if (rg_gui_confirm(_("Core uninstalled"), _("Restart now to refresh the launcher?"),
                                   false))
                    rg_system_restart();
            }
            else
            {
                rg_gui_alert(_("Core manager"), _("Failed to remove the core."));
            }
        }
    }
}

static bool core_manager_activate(core_ui_t *ui, int index)
{
    if (index < 0 || index >= ui->count)
        return false;

    core_item_t *it = &ui->items[index];

    if (it->kind == CORE_KIND_INSTALL)
    {
        core_install_run(NULL);
    }
    else if (it->kind == CORE_KIND_FORMAT)
    {
        if (rg_gui_confirm(_("Format pool"), _("Remove all cores from the pool?"), false))
        {
            if (core_pool_format())
            {
                if (rg_gui_confirm(_("Pool formatted"),
                                   _("Restart now to refresh the launcher?"), false))
                    rg_system_restart();
            }
            else
            {
                rg_gui_alert(_("Core manager"), _("Failed to format the pool."));
            }
        }
    }
    else if (it->installed >= 0)
    {
        core_pool_details(it->installed);
    }

    core_ui_reload(ui);
    return false;
}

static void core_manager_run(void)
{
    core_item_t *items = calloc(CORE_UI_MAX, sizeof(core_item_t));

    if (!items)
    {
        rg_gui_alert(_("Core manager"), _("Not enough memory."));
        return;
    }

    core_ui_t ui = {
        .title = _("Core manager"),
        .primary = NULL,
        .only_id = NULL,
        .items = items,
        .install_list = false,
        .checkboxes = false,
    };

    core_ui_reload(&ui);

    if (!core_ui_begin())
    {
        rg_gui_alert(_("Core manager"), _("Not enough memory."));
        free(items);
        return;
    }

    core_ui_loop(&ui, core_manager_activate);
    core_ui_end();
    free(items);
}

rg_gui_event_t core_manager_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    if (!core_pool_available())
    {
        rg_gui_alert(_("Core manager"), _("Core pool is not available."));
        return RG_DIALOG_VOID;
    }

    core_manager_run();
    return RG_DIALOG_VOID;
}
