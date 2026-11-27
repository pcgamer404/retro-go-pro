/**
 * @file cnc_flashcache.cpp
 * @brief Copies of the "cached" MIX files in a flash partition.
 *
 * The game keeps some MIX files (CONQUER.MIX, LOCAL.MIX, the theater...) in
 * memory. With 8 MB of PSRAM there is not enough room for them, so they are
 * copied from the SD card into the "mixcache" partition the first time they are
 * needed, and read from there through the flash cache (memory mapped): they take
 * no RAM at all.
 *
 * Partition layout: the first sector is the index (one entry per copy, written
 * after the data), the copies follow, each starting on a sector boundary. When a
 * copy does not fit, the store is emptied, provided nothing in it is in use.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "cnc_platform.h"

#define ENTRY_MAGIC     0x4D495843u     // "CXIM"
#define PENDING_MAGIC   0x444E4550u     // "PEND": copy to make (data fields unused)
#define FREE_MAGIC      0xFFFFFFFFu     // end of the index (0: entry cancelled)
#define SECTOR_SIZE     4096
#define INDEX_ENTRIES   (SECTOR_SIZE / sizeof(Entry))
#define COPY_CHUNK      16384
// cnc_gamehooks.cpp: progress on the game screen, if it can be used
extern "C" bool cncStandBy(const char* detail, int percent);

#define TITLE           "PLEASE STAND BY"
#define NOTE            "One-time copy to flash"

static const char* TAG = "mixcache";

struct Entry
{
    uint32_t Magic;
    char Name[20];
    int32_t Start;      // position of the data in the file it comes from
    int32_t Size;
    uint32_t Offset;    // in the partition
};

static const esp_partition_t* partition;
static const uint8_t* mapped;
static bool initialised;
static bool inUse;      // a copy has been handed out: the store can't be emptied any more
static bool wiped;      // emptied by the debug console: no new copies before a restart
static bool standBy;    // the game shows its "please stand by" screen: copies can be made

static bool init(void)
{
    if (!initialised) {
        initialised = true;
        partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "mixcache");
        if (partition == nullptr) {
            ESP_LOGW(TAG, "no mixcache partition");
            return false;
        }
        esp_partition_mmap_handle_t handle;
        const void* ptr;
        esp_err_t err = esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mmap of %u bytes failed: %s", (unsigned)partition->size, esp_err_to_name(err));
            partition = nullptr;
            return false;
        }
        mapped = (const uint8_t*)ptr;
        ESP_LOGI(TAG, "%u kB at %p", (unsigned)(partition->size / 1024), mapped);
    }
    return partition != nullptr;
}

static const Entry* entries(void)
{
    return (const Entry*)mapped;
}

static bool matches(const Entry* e, const char* name, int32_t start, int32_t size)
{
    return e->Magic == ENTRY_MAGIC && e->Start == start && e->Size == size
           && strncasecmp(e->Name, name, sizeof(e->Name)) == 0;
}

// Writes an index entry in the first free slot
static bool addEntry(const Entry* e)
{
    for (unsigned i = 0; i < INDEX_ENTRIES; i++) {
        if (entries()[i].Magic == FREE_MAGIC) {
            return esp_partition_write(partition, i * sizeof(Entry), e, sizeof(*e)) == ESP_OK;
        }
    }
    return false;
}

// Copies can be made now (the game shows its "please stand by" screen)
extern "C" bool cncFlashCacheStandBy(void)
{
    return standBy && !wiped;
}

extern "C" void cncFlashCacheSetStandBy(bool on)
{
    standBy = on;
}

// A mixfile needed when no copy can be made: copied at the next "please stand by"
extern "C" void cncFlashCacheDefer(const char* name)
{
    if (!init() || wiped || strlen(name) >= sizeof(Entry::Name)) {
        return;
    }
    for (unsigned i = 0; i < INDEX_ENTRIES && entries()[i].Magic != FREE_MAGIC; i++) {
        if (entries()[i].Magic == PENDING_MAGIC && strncasecmp(entries()[i].Name, name, sizeof(Entry::Name)) == 0) {
            return;
        }
    }
    Entry e;
    memset(&e, 0, sizeof(e));
    e.Magic = PENDING_MAGIC;
    strncpy(e.Name, name, sizeof(e.Name) - 1);
    if (addEntry(&e)) {
        ESP_LOGI(TAG, "%s: copy deferred", name);
    }
}

// The name of the next deferred copy
extern "C" bool cncFlashCachePending(char* name, int size)
{
    if (!init()) {
        return false;
    }
    for (unsigned i = 0; i < INDEX_ENTRIES && entries()[i].Magic != FREE_MAGIC; i++) {
        if (entries()[i].Magic == PENDING_MAGIC) {
            snprintf(name, size, "%.*s", (int)sizeof(Entry::Name), entries()[i].Name);
            return true;
        }
    }
    return false;
}

// The deferred copy of name is done (or not possible): its entries are cancelled
extern "C" void cncFlashCacheDone(const char* name)
{
    static const uint32_t cancelled = 0;
    for (unsigned i = 0; i < INDEX_ENTRIES && entries()[i].Magic != FREE_MAGIC; i++) {
        if (entries()[i].Magic == PENDING_MAGIC && strncasecmp(entries()[i].Name, name, sizeof(Entry::Name)) == 0) {
            esp_partition_write(partition, i * sizeof(Entry), &cancelled, sizeof(cancelled));
        }
    }
}

// The copy of a MIX file data block, or null if there is none
extern "C" const void* cncFlashCacheFind(const char* name, int32_t start, int32_t size)
{
    if (!init()) {
        return nullptr;
    }
    for (unsigned i = 0; i < INDEX_ENTRIES; i++) {
        const Entry* e = &entries()[i];
        if (e->Magic == FREE_MAGIC) {
            break;
        }
        if (matches(e, name, start, size)) {
            inUse = true;
            return mapped + e->Offset;
        }
    }
    return nullptr;
}

// Copies a MIX file data block (read() is called until size bytes are read) and
// returns the copy, or null if it does not fit.
extern "C" const void* cncFlashCacheStore(const char* name,
                                          int32_t start,
                                          int32_t size,
                                          int (*read)(void* ctx, void* buffer, int length),
                                          void* ctx)
{
    if (!init() || wiped || strlen(name) >= sizeof(Entry::Name)) {
        return nullptr;
    }
    uint32_t needed = (size + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
    if (needed > partition->size - SECTOR_SIZE) {
        return nullptr;
    }
    // first free index slot and first free byte
    unsigned slot = 0;
    uint32_t end = SECTOR_SIZE;
    for (; slot < INDEX_ENTRIES; slot++) {
        const Entry* e = &entries()[slot];
        if (e->Magic == FREE_MAGIC) {
            break;
        }
        if (e->Magic == ENTRY_MAGIC && e->Offset + e->Size > end) {
            end = e->Offset + e->Size;
        }
    }
    end = (end + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
    if (slot == INDEX_ENTRIES || end + needed > partition->size) {
        if (inUse) {
            ESP_LOGW(TAG, "%s (%d bytes) does not fit", name, (int)size);
            return nullptr;
        }
        // nothing used yet (e.g. the other game's files): start over
        ESP_LOGI(TAG, "emptying the store");
        if (esp_partition_erase_range(partition, 0, SECTOR_SIZE) != ESP_OK) {
            return nullptr;
        }
        slot = 0;
        end = SECTOR_SIZE;
    }

    uint8_t* buffer = (uint8_t*)heap_caps_malloc(COPY_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == nullptr) {
        return nullptr;
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "Copying %s", name);
    ESP_LOGI(TAG, "copying %s (%d bytes)", name, (int)size);
    bool onScreen = cncStandBy(detail, 0);
    if (!onScreen) {
        ESP_LOGW(TAG, "the game screen can't be used: own progress screen");
        videoProgress(TITLE, detail, NOTE, 0);
    }
    int64_t t0 = esp_timer_get_time();
    bool ok = esp_partition_erase_range(partition, end, needed) == ESP_OK;
    int shown = 0;
    for (int32_t done = 0; ok && done < size;) {
        int length = size - done < COPY_CHUNK ? size - done : COPY_CHUNK;
        ok = read(ctx, buffer, length) == length && esp_partition_write(partition, end + done, buffer, length) == ESP_OK;
        done += length;
        int percent = (int)((int64_t)done * 100 / size);
        if (percent / 5 != shown / 5) {
            shown = percent;
            if (onScreen) {
                cncStandBy(detail, percent);
            } else {
                videoProgress(TITLE, detail, NOTE, percent);
            }
        }
    }
    free(buffer);
    if (ok) {
        Entry e;
        memset(&e, 0, sizeof(e));
        e.Magic = ENTRY_MAGIC;
        strncpy(e.Name, name, sizeof(e.Name) - 1);
        e.Start = start;
        e.Size = size;
        e.Offset = end;
        ok = esp_partition_write(partition, slot * sizeof(Entry), &e, sizeof(e)) == ESP_OK;
    }
    if (onScreen) {
        cncStandBy(detail, -1);
    }
    videoHideConsole();
    if (!ok) {
        ESP_LOGE(TAG, "copy of %s failed", name);
        return nullptr;
    }
    ESP_LOGI(TAG, "%s: %d bytes copied in %d ms", name, (int)size, (int)((esp_timer_get_time() - t0) / 1000));
    inUse = true;
    return mapped + end;
}

// Empties the store (debug console "mixwipe"): the files are copied again after a restart
extern "C" void cncFlashCacheWipe(void)
{
    // the copies in use stay readable, new ones would overwrite them: none until a restart
    wiped = true;
    if (init() && esp_partition_erase_range(partition, 0, SECTOR_SIZE) == ESP_OK) {
        printf("mixcache emptied, restart to copy the files again\n");
    }
}
