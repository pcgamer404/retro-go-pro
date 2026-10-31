#include "resident.h"

#include <rg_system.h>
#include <rg_gui.h>

#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_err.h>
#include <hal/mmu_hal.h>
#include <hal/cache_hal.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recipe_index.h"
#include "mio0_index.h"
#include "s3_reloc_data.h"

#define CACHE_DIR RG_BASE_PATH_CACHE "/mk64"
#define TRANSFORM_SCRATCH 65536u
#define MAX_MIO0_OUTPUT 184664u
#define MAX_PACKED_COURSE 131072u
#define MAX_UNPACKED_COURSE (51008u + 64u)

/* The flash writer needs an internal-DRAM staging buffer: PSRAM and flash
 * sources are not accessible while the SPI write has the cache disabled. */
#define WRITE_CHUNK 4096u
#define WRITE_BUF_SIZE (WRITE_CHUNK + 512u)

#define MK64_STORE_MAGIC 0x4D4B5333u

typedef struct {
    uint32_t magic;
    uint32_t tag;
    uint32_t region_size;
    uint32_t data_offset;
    uint32_t map_size;
    uint32_t reserved;
} mk64_store_header_t;

extern uintptr_t gHeapEndPtr;
extern int32_t gIsMirrorMode;
extern int32_t sGfxSeekPosition;
extern void displaylist_unpack(uintptr_t *packed, uintptr_t output_size,
                               uint32_t arg2);

static const esp_partition_t *sPartition;
static uint8_t sWriteBuf[WRITE_BUF_SIZE] __attribute__((aligned(4)));

static bool transform(uint8_t *data, size_t length, unsigned xform,
                      size_t first_word)
{
    if (xform == 0)
        return true;
    if (xform >= 16 && xform - 16 >= mk64_pattern_count)
        return false;

    const mk64_pattern_t *pattern = xform >= 16 ?
                                    &mk64_patterns[xform - 16] : NULL;
    for (size_t pos = 0; pos + 3 < length; pos += 4) {
        uint8_t *p = data + pos;
        uint8_t temp;
        unsigned shape = xform;
        if (pattern)
            shape = 16 + pattern->shapes[(first_word + pos / 4) % pattern->nwords];
        switch (shape) {
            case 1: /* sw16 */
            case 17: /* 22 */
                temp = p[0]; p[0] = p[1]; p[1] = temp;
                temp = p[2]; p[2] = p[3]; p[3] = temp;
                break;
            case 2: /* sw32 */
            case 16: /* 4 */
                temp = p[0]; p[0] = p[3]; p[3] = temp;
                temp = p[1]; p[1] = p[2]; p[2] = temp;
                break;
            case 18: /* 211 */
                temp = p[0]; p[0] = p[1]; p[1] = temp;
                break;
            case 19: /* 112 */
                temp = p[2]; p[2] = p[3]; p[3] = temp;
                break;
            case 20: /* 1111 */
                break;
            default:
                return false;
        }
    }
    return true;
}

static size_t next_chunk(size_t remaining, unsigned xform)
{
    size_t chunk = remaining < TRANSFORM_SCRATCH - 4 ?
                   remaining : TRANSFORM_SCRATCH - 4;
    if (chunk < remaining) {
        size_t period = 4;
        if (xform >= 16 && xform - 16 < mk64_pattern_count)
            period *= mk64_patterns[xform - 16].nwords;
        chunk -= chunk % period;
    }
    return chunk;
}

/* ------------------------------------------------------------------------- */
/* Flash store helpers                                                       */
/* ------------------------------------------------------------------------- */

static bool store_write(uint32_t offset, const void *source, size_t length)
{
    if ((uint64_t)offset + length > MK64_S3_REGION_SIZE)
        return false;
    const uint8_t *p = source;
    while (length) {
        size_t n = length < WRITE_CHUNK ? length : WRITE_CHUNK;
        memcpy(sWriteBuf, p, n);
        if (esp_partition_write(sPartition, MK64_S3_DATA_OFFSET + offset,
                                sWriteBuf, n) != ESP_OK)
            return false;
        offset += n;
        p += n;
        length -= n;
    }
    return true;
}

static bool store_zero(uint32_t offset, uint32_t length)
{
    if ((uint64_t)offset + length > MK64_S3_REGION_SIZE)
        return false;
    if (!length)
        return true;
    memset(sWriteBuf, 0, sizeof(sWriteBuf));
    while (length) {
        uint32_t n = length < WRITE_CHUNK ? length : WRITE_CHUNK;
        if (esp_partition_write(sPartition, MK64_S3_DATA_OFFSET + offset,
                                sWriteBuf, n) != ESP_OK)
            return false;
        offset += n;
        length -= n;
    }
    return true;
}

/* First recipe destination strictly above dst, or the end of the region. */
static uint32_t next_dst(uint32_t dst)
{
    size_t lo = 0, hi = mk64_recipe_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t candidate = mk64_recipes[mk64_recipe_dst_order[mid]].dst;
        if (candidate <= dst)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= mk64_recipe_count)
        return MK64_S3_REGION_SIZE;
    return mk64_recipes[mk64_recipe_dst_order[lo]].dst;
}

static bool has_fixups_in(uint32_t begin, uint32_t end)
{
    for (size_t i = 0; i < s3_reloc_fixup_count; ++i) {
        uint32_t off = s3_reloc_fixups[i].offset;
        if (off >= begin && off < end)
            return true;
    }
    return false;
}

static void patch_fixups_in(uint8_t *buffer, uint32_t begin, uint32_t end)
{
    for (size_t i = 0; i < s3_reloc_fixup_count; ++i) {
        uint32_t off = s3_reloc_fixups[i].offset;
        if (off >= begin && off < end) {
            uint32_t value = (uint32_t)s3_reloc_fixups[i].value;
            if (off + sizeof(value) > end)
                continue; /* generator guarantees this cannot happen */
            memcpy(buffer + (off - begin), &value, sizeof(value));
        }
    }
}

/* Debug-only trap: write back dirty data-cache lines that map to flash so a
 * stray write into the read-only part of the asset window panics at this call
 * site instead of later, at an arbitrary eviction point. Silent so it can stay
 * enabled while the menus run. */
void mk64_debug_probe(const char *tag)
{
    (void)tag;
    for (uint32_t off = 0; off < MK64_S3_MAP_SIZE; off += 0x100000u)
    {
        uint32_t len = MK64_S3_MAP_SIZE - off;
        if (len > 0x100000u)
            len = 0x100000u;
        cache_hal_writeback_addr(MK64_S3_FLASH_BASE + off, len);
    }
    for (uint32_t off = 0; off < 0x200000u; off += 0x100000u)
        cache_hal_writeback_addr(0x3C000000u + off, 0x100000u);
}

/* The game mutates part of the extracted data in place: func_80092290
 * animates the menu item vertex colours through D_02007BB8/CD8/DF8, which live
 * in data_segment2 (0x02000000-0x0200B6ED, store offsets 0..0xB6EC). Those are
 * flash-mapped here, so a store leaves a dirty line that the dcache later fails
 * to write back. Back the whole window with PSRAM: copy it out of flash once
 * and point the MMU at the copy, keeping every relocated pointer valid. */
#define WRITABLE_WINDOW 0x10000u

static bool map_writable_window(void)
{
    uint8_t *buffer = heap_caps_aligned_alloc(WRITABLE_WINDOW, WRITABLE_WINDOW,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer)
        return false;
    if (esp_partition_read(sPartition, MK64_S3_DATA_OFFSET, buffer,
                           WRITABLE_WINDOW) != ESP_OK)
        return false;
    cache_hal_writeback_addr((uint32_t)buffer, WRITABLE_WINDOW);

    uint32_t paddr = 0;
    mmu_target_t target = 0;
    if (!mmu_hal_vaddr_to_paddr(0, (uint32_t)buffer, &paddr, &target) ||
        target != MMU_TARGET_PSRAM0)
    {
        RG_LOGW("MK64: write window is not in PSRAM (target %u)", (unsigned)target);
        return false;
    }
    uint32_t mapped = 0;
    mmu_hal_map_region(0, MMU_TARGET_PSRAM0, MK64_S3_FLASH_BASE, paddr,
                       WRITABLE_WINDOW, &mapped);
    if (mapped < WRITABLE_WINDOW ||
        !cache_hal_invalidate_addr(MK64_S3_FLASH_BASE, mapped))
        return false;

    RG_LOGI("MK64: data_segment2 window is writable at %08x-%08x (%u bytes)",
            (unsigned)MK64_S3_FLASH_BASE,
            (unsigned)(MK64_S3_FLASH_BASE + WRITABLE_WINDOW - 1u),
            WRITABLE_WINDOW);
    return true;
}

static bool map_store(void)
{
    uint32_t paddr = (uint32_t)sPartition->address + MK64_S3_DATA_OFFSET;
    if (paddr & 0xFFFFu)
        return false;
    uint32_t mapped = 0;
    /* On the S3 the MMU table is shared and mmu_id is ignored. */
    mmu_hal_map_region(0, MMU_TARGET_FLASH0, MK64_S3_FLASH_BASE,
                       paddr, MK64_S3_MAP_SIZE, &mapped);
    if (mapped < MK64_S3_REGION_SIZE)
        return false;
    return cache_hal_invalidate_addr(MK64_S3_FLASH_BASE, mapped);
}

/* ------------------------------------------------------------------------- */
/* Recipe writers                                                            */
/* ------------------------------------------------------------------------- */

static bool raw_write_container(mk64_rom_t *rom, const mk64_recipe_t *recipe,
                                uint32_t next)
{
    uint32_t extended = next - recipe->dst;
    size_t padded = (recipe->size + 3u) & ~3u;
    if (extended > sizeof(sWriteBuf) || padded > sizeof(sWriteBuf))
        return false;
    if (!mk64_rom_read(rom, recipe->src, sWriteBuf, padded))
        return false;
    if (!transform(sWriteBuf, padded, recipe->xform, 0))
        return false;
    if (extended > recipe->size)
        memset(sWriteBuf + recipe->size, 0, extended - recipe->size);
    patch_fixups_in(sWriteBuf, recipe->dst, next);
    return esp_partition_write(sPartition, MK64_S3_DATA_OFFSET + recipe->dst,
                               sWriteBuf, extended) == ESP_OK;
}

static bool emit_raw(mk64_rom_t *rom, const mk64_recipe_t *recipe,
                     uint8_t *scratch)
{
    uint32_t end = recipe->dst + recipe->size;
    uint32_t next = next_dst(recipe->dst);
    if (next < end)
        return false;

    if (has_fixups_in(recipe->dst, next))
        return raw_write_container(rom, recipe, next);

    if (recipe->xform == 0) {
        for (size_t done = 0; done < recipe->size;) {
            size_t n = recipe->size - done;
            if (n > WRITE_CHUNK)
                n = WRITE_CHUNK;
            if (!mk64_rom_read(rom, recipe->src + done, scratch, n) ||
                !store_write(recipe->dst + done, scratch, n))
                return false;
            done += n;
        }
    } else {
        for (size_t done = 0; done < recipe->size;) {
            size_t chunk = next_chunk(recipe->size - done, recipe->xform);
            size_t padded = (chunk + 3u) & ~3u;
            if (!chunk ||
                !mk64_rom_read(rom, recipe->src + done, scratch, padded) ||
                !transform(scratch, padded, recipe->xform, done / 4) ||
                !store_write(recipe->dst + done, scratch, chunk))
                return false;
            done += chunk;
        }
    }
    return store_zero(end, next - end);
}

static bool mio0_write_container(const mk64_recipe_t *recipe,
                                 const uint8_t *block, size_t block_size,
                                 uint32_t next)
{
    uint32_t extended = next - recipe->dst;
    size_t padded = (recipe->size + 3u) & ~3u;
    if (extended > sizeof(sWriteBuf) || padded > sizeof(sWriteBuf) ||
        (uint64_t)recipe->extra + recipe->size > block_size)
        return false;
    memcpy(sWriteBuf, block + recipe->extra, recipe->size);
    if (padded > recipe->size)
        memset(sWriteBuf + recipe->size, 0, padded - recipe->size);
    if (!transform(sWriteBuf, padded, recipe->xform, 0))
        return false;
    if (extended > recipe->size)
        memset(sWriteBuf + recipe->size, 0, extended - recipe->size);
    patch_fixups_in(sWriteBuf, recipe->dst, next);
    return esp_partition_write(sPartition, MK64_S3_DATA_OFFSET + recipe->dst,
                               sWriteBuf, extended) == ESP_OK;
}

static bool emit_mio0(const mk64_recipe_t *recipe, const uint8_t *block,
                      size_t block_size, uint8_t *scratch)
{
    if ((uint64_t)recipe->extra + recipe->size > block_size)
        return false;
    uint32_t end = recipe->dst + recipe->size;
    uint32_t next = next_dst(recipe->dst);
    if (next < end)
        return false;

    const uint8_t *source = block + recipe->extra;
    if (has_fixups_in(recipe->dst, next))
        return mio0_write_container(recipe, block, block_size, next);

    if (recipe->xform == 0) {
        if (!store_write(recipe->dst, source, recipe->size))
            return false;
    } else {
        for (size_t done = 0; done < recipe->size;) {
            size_t chunk = next_chunk(recipe->size - done, recipe->xform);
            size_t padded = (chunk + 3u) & ~3u;
            size_t available = block_size - recipe->extra - done;
            size_t copy = padded < available ? padded : available;
            if (!chunk)
                return false;
            memcpy(scratch, source + done, copy);
            if (copy < padded)
                memset(scratch + copy, 0, padded - copy);
            if (!transform(scratch, padded, recipe->xform, done / 4) ||
                !store_write(recipe->dst + done, scratch, chunk))
                return false;
            done += chunk;
        }
    }
    return store_zero(end, next - end);
}

static bool write_reloc_tables(void)
{
    for (size_t i = 0; i < s3_reloc_table_count; ++i) {
        const s3_reloc_table_t *table = &s3_reloc_tables[i];
        if (table->length > sizeof(sWriteBuf))
            return false;
        memcpy(sWriteBuf, table->data, table->length);
        if (esp_partition_write(sPartition,
                                MK64_S3_DATA_OFFSET + table->offset,
                                sWriteBuf, table->length) != ESP_OK)
            return false;
        uint32_t end = table->offset + table->length;
        uint32_t next = next_dst(table->offset);
        if (next > end && !store_zero(end, next - end))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* MIO0 cache                                                                */
/* ------------------------------------------------------------------------- */

static const mk64_mio0_block_t *find_block(uint32_t offset)
{
    for (size_t i = 0; i < sizeof(mk64_mio0_blocks) /
                            sizeof(mk64_mio0_blocks[0]); ++i)
        if (mk64_mio0_blocks[i].offset == offset)
            return &mk64_mio0_blocks[i];
    return NULL;
}

static bool read_cached_block(uint32_t offset, uint8_t *buffer,
                              size_t *output_size)
{
    const mk64_mio0_block_t *block = find_block(offset);
    if (!block || block->output_size > MAX_MIO0_OUTPUT)
        return false;
    char path[RG_PATH_MAX + 1];
    int length = snprintf(path, sizeof(path), CACHE_DIR "/mio0_%08lx.bin",
                          (unsigned long)offset);
    if (length < 0 || length >= (int)sizeof(path))
        return false;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    bool okay = fread(buffer, 1, block->output_size, file) == block->output_size &&
                fgetc(file) == EOF;
    fclose(file);
    *output_size = block->output_size;
    return okay;
}

/* ------------------------------------------------------------------------- */
/* Course unpack                                                             */
/* ------------------------------------------------------------------------- */

static bool unpack_course(mk64_rom_t *rom, size_t course_id,
                          uint8_t *packed, uint8_t *unpacked,
                          unsigned *count, uint32_t *bytes)
{
    const mk64_course_dl_t *course = &mk64_course_displaylists[course_id];
    if (course->rom_len > MAX_PACKED_COURSE ||
        (uint64_t)course->unpacked_len + 64 > MAX_UNPACKED_COURSE ||
        course->packed_off >= course->rom_len ||
        !mk64_rom_read(rom, course->rom_off, packed, course->rom_len))
        return false;

    memset(unpacked, 0, course->unpacked_len + 64);
    uintptr_t saved_heap_end = gHeapEndPtr;
    int32_t saved_mirror = gIsMirrorMode;
    gIsMirrorMode = 0;
    gHeapEndPtr = (uintptr_t)unpacked +
                  ((course->unpacked_len + 15u) & ~15u) + 8u;
    displaylist_unpack((uintptr_t *)(packed + course->packed_off),
                       course->unpacked_len, 0);
    gHeapEndPtr = saved_heap_end;
    gIsMirrorMode = saved_mirror;
    if (sGfxSeekPosition < 0 || (uint64_t)sGfxSeekPosition * 8 >
                                  course->unpacked_len + 64)
        return false;

    for (size_t i = 0; i < mk64_recipe_count; ++i) {
        const mk64_recipe_t *recipe = &mk64_recipes[i];
        if (recipe->kind != 5 || recipe->src != course_id)
            continue;
        if ((uint64_t)recipe->extra + recipe->size > course->unpacked_len)
            return false;
        if (!store_write(recipe->dst, unpacked + recipe->extra,
                         recipe->size))
            return false;
        uint32_t end = recipe->dst + recipe->size;
        uint32_t next = next_dst(recipe->dst);
        if (next < end || (next > end && !store_zero(end, next - end)))
            return false;
        (*count)++;
        *bytes += recipe->size;
    }
    RG_LOGI("MK64: unpacked course %u/%u (%u bytes)",
            (unsigned)(course_id + 1),
            (unsigned)mk64_course_displaylist_count,
            (unsigned)course->unpacked_len);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                               */
/* ------------------------------------------------------------------------- */

bool mk64_resident_load(mk64_rom_t *rom, char *error, size_t error_size)
{
    if (!rom || !error || !error_size)
        return false;
    error[0] = '\0';

    sPartition = esp_ota_get_running_partition();
    if (!sPartition) {
        snprintf(error, error_size, "No running app partition");
        return false;
    }
    if (sPartition->size < MK64_S3_REQUIRED_PARTITION) {
        snprintf(error, error_size,
                 "App partition too small: %u < %u bytes",
                 (unsigned)sPartition->size,
                 (unsigned)MK64_S3_REQUIRED_PARTITION);
        return false;
    }
    if (((uint32_t)sPartition->address + MK64_S3_DATA_OFFSET) & 0xFFFFu) {
        snprintf(error, error_size, "App partition is not 64 KiB aligned");
        return false;
    }

    mk64_store_header_t header;
    if (esp_partition_read(sPartition, MK64_S3_HEADER_OFFSET,
                           &header, sizeof(header)) == ESP_OK &&
        header.magic == MK64_STORE_MAGIC &&
        header.tag == MK64_S3_LAYOUT_TAG &&
        header.region_size == MK64_S3_REGION_SIZE &&
        header.data_offset == MK64_S3_DATA_OFFSET &&
        header.map_size == MK64_S3_MAP_SIZE) {
        if (!map_store()) {
            snprintf(error, error_size, "Cannot map flash asset store");
            return false;
        }
        if (!map_writable_window()) {
            snprintf(error, error_size, "Cannot map writable asset window");
            return false;
        }
        RG_LOGI("MK64: flash asset store mapped (%u bytes at %08x)",
                (unsigned)MK64_S3_REGION_SIZE,
                (unsigned)MK64_S3_FLASH_BASE);
        return true;
    }

    RG_LOGI("MK64: extracting flash asset store (first boot)");
    rg_gui_draw_message("MK64: extracting assets\n(first boot, please wait)");
    vTaskDelay(1);

    uint8_t *scratch = NULL;
    uint8_t *block = NULL;
    uint8_t *packed = NULL;
    uint8_t *unpacked = NULL;
    bool okay = false;
    int64_t began = rg_system_timer();

    /* Keep the watchdog and the UI alive while erasing ~10.4 MiB. */
    for (uint32_t offset = MK64_S3_HEADER_OFFSET;
         offset < MK64_S3_REQUIRED_PARTITION;) {
        uint32_t length = MK64_S3_REQUIRED_PARTITION - offset;
        if (length > 0x100000u)
            length = 0x100000u;
        if (esp_partition_erase_range(sPartition, offset, length) != ESP_OK) {
            snprintf(error, error_size, "Asset store erase failed at %08x",
                     (unsigned)offset);
            goto done;
        }
        offset += length;
        vTaskDelay(1);
    }

    scratch = rg_alloc(TRANSFORM_SCRATCH, MEM_SLOW);
    block = rg_alloc(MAX_MIO0_OUTPUT, MEM_SLOW);
    if (!scratch || !block) {
        snprintf(error, error_size, "Insufficient RAM for asset recipes");
        goto done;
    }

    uint32_t loaded_block = UINT32_MAX;
    size_t block_size = 0;
    unsigned raw = 0, mio0 = 0, unpack_count = 0, deferred = 0;
    uint32_t raw_bytes = 0, mio0_bytes = 0, unpack_bytes = 0, deferred_bytes = 0;

    for (size_t i = 0; i < mk64_recipe_count; ++i) {
        const mk64_recipe_t *recipe = &mk64_recipes[i];
        if ((uint64_t)recipe->dst + recipe->size > MK64_S3_REGION_SIZE) {
            snprintf(error, error_size, "Asset recipe %u exceeds region",
                     (unsigned)i);
            goto done;
        }
        if (recipe->kind == 1) {
            if (!emit_raw(rom, recipe, scratch)) {
                snprintf(error, error_size, "RAW recipe %u failed", (unsigned)i);
                goto done;
            }
            raw++;
            raw_bytes += recipe->size;
        } else if (recipe->kind == 2) {
            if (recipe->src != loaded_block) {
                if (!read_cached_block(recipe->src, block, &block_size)) {
                    snprintf(error, error_size,
                             "MIO0 cache read failed at %08lx",
                             (unsigned long)recipe->src);
                    goto done;
                }
                loaded_block = recipe->src;
            }
            if (!emit_mio0(recipe, block, block_size, scratch)) {
                snprintf(error, error_size, "MIO0 recipe %u failed",
                         (unsigned)i);
                goto done;
            }
            mio0++;
            mio0_bytes += recipe->size;
        } else if (recipe->kind == 4) {
            deferred++;
            deferred_bytes += recipe->size;
        } else if (recipe->kind != 5) {
            snprintf(error, error_size, "Unknown asset recipe %u", (unsigned)i);
            goto done;
        }
        if ((i & 1023u) == 0) {
            RG_LOGI("MK64: recipe %u/%u", (unsigned)i,
                    (unsigned)mk64_recipe_count);
            vTaskDelay(1);
        }
    }

    free(block);
    free(scratch);
    block = scratch = NULL;

    packed = heap_caps_aligned_alloc(16, MAX_PACKED_COURSE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unpacked = heap_caps_aligned_alloc(16, MAX_UNPACKED_COURSE,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packed || !unpacked) {
        snprintf(error, error_size, "Insufficient RAM for course unpacking");
        goto done;
    }
    for (size_t course = 0; course < mk64_course_displaylist_count; ++course) {
        if (!unpack_course(rom, course, packed, unpacked,
                           &unpack_count, &unpack_bytes)) {
            snprintf(error, error_size, "Course unpack failed at %u",
                     (unsigned)course);
            goto done;
        }
        vTaskDelay(1);
    }
    free(unpacked);
    free(packed);
    unpacked = packed = NULL;

    if (!write_reloc_tables()) {
        snprintf(error, error_size, "Asset relocation tables failed");
        goto done;
    }

    mk64_store_header_t new_header = {
        .magic = MK64_STORE_MAGIC,
        .tag = MK64_S3_LAYOUT_TAG,
        .region_size = MK64_S3_REGION_SIZE,
        .data_offset = MK64_S3_DATA_OFFSET,
        .map_size = MK64_S3_MAP_SIZE,
        .reserved = 0,
    };
    if (esp_partition_write(sPartition, MK64_S3_HEADER_OFFSET,
                            &new_header, sizeof(new_header)) != ESP_OK) {
        snprintf(error, error_size, "Asset store header write failed");
        goto done;
    }
    if (!map_store()) {
        snprintf(error, error_size, "Cannot map flash asset store");
        goto done;
    }
    if (!map_writable_window()) {
        snprintf(error, error_size, "Cannot map writable asset window");
        goto done;
    }

    RG_LOGI("MK64: flash store ready: RAW %u/%u, MIO0 %u/%u, "
            "UNPACK %u/%u, RELOCS %u/%u deferred, %u tables, %u fixups, "
            "%u ms", raw, (unsigned)raw_bytes, mio0, (unsigned)mio0_bytes,
            unpack_count, (unsigned)unpack_bytes, deferred,
            (unsigned)deferred_bytes, (unsigned)s3_reloc_table_count,
            (unsigned)s3_reloc_fixup_count,
            (unsigned)((rg_system_timer() - began) / 1000));
    RG_LOGI("MK64: PSRAM after store: free %u, largest %u bytes",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    okay = true;

done:
    free(unpacked);
    free(packed);
    free(block);
    free(scratch);
    return okay;
}
