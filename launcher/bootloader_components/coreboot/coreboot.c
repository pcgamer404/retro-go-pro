#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "bootloader_common.h"
#include "bootloader_utility.h"
#include "bootloader_flash_priv.h"
#include "esp_image_format.h"

#include "core_manifest.h"

static const char *TAG = "coreboot";

/* Referenced by the stock bootloader's main component with -u, which is what
 * forces this archive member into the link (the hooks are weak symbols). */
void bootloader_hooks_include(void)
{
}

static uint32_t manifest_crc(const core_manifest_t *m)
{
    core_manifest_t tmp = *m;
    tmp.crc = 0;
    return esp_rom_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

static bool manifest_read(uint32_t base, int slot, core_manifest_t *m)
{
    if (bootloader_flash_read(base + slot * CORE_MANIFEST_SECTOR, m, sizeof(*m), false) != ESP_OK)
        return false;
    if (m->magic != CORE_MANIFEST_MAGIC || m->version != CORE_MANIFEST_VERSION)
        return false;
    return manifest_crc(m) == m->crc;
}

/* Picks the newest valid copy. The launcher always writes to the copy that is
 * not in use, so at least one of them survives a power loss. */
static int manifest_load(uint32_t base, core_manifest_t *m)
{
    core_manifest_t tmp;
    int best = -1;

    for (int slot = 0; slot < CORE_MANIFEST_COPIES; slot++)
    {
        if (!manifest_read(base, slot, &tmp))
            continue;
        if (best < 0 || tmp.seq > m->seq)
        {
            *m = tmp;
            best = slot;
        }
    }
    return best;
}

/* The bootloader always writes to the copy the launcher is not using. It can
 * only be sure about that right after loading, so keep the slot around. */
static bool manifest_save(uint32_t base, int slot, core_manifest_t *m)
{
    slot = (slot == 0) ? 1 : 0;
    m->seq++;
    m->crc = manifest_crc(m);
    if (bootloader_flash_erase_sector((base + slot * CORE_MANIFEST_SECTOR) / CORE_MANIFEST_SECTOR) != ESP_OK)
        return false;
    return bootloader_flash_write(base + slot * CORE_MANIFEST_SECTOR, m, sizeof(*m), false) == ESP_OK;
}

void bootloader_after_init(void)
{
    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs))
        return;                     /* stock bootloader reports the error */
    if (bs.app_count != 1)
        return;                     /* unexpected layout, stay out of the way */

    /* The pool is laid out immediately after the launcher by rg_tool.py. */
    const uint32_t pool = bs.ota[0].offset + bs.ota[0].size;

    if (bootloader_common_check_long_hold_gpio_level(CORE_WIPE_GPIO,
                                                     CORE_WIPE_SECONDS,
                                                     false) == GPIO_LONG_HOLD) {
        ESP_LOGW(TAG, "wipe requested, erasing core manifest");
        bootloader_flash_erase_range(pool, CORE_MANIFEST_RESERVED);
        return;
    }

    core_manifest_t m = {0};
    int slot = manifest_load(pool, &m);
    if (slot < 0)
        return;                     /* no manifest yet, boot the launcher */

    if (m.count > CORE_MAX_ENTRIES)
        return;
    if (m.selected < 0 || m.selected >= (int)m.count)
        return;                     /* launcher selected */

    core_entry_t *e = &m.entries[m.selected];

    /* The selection is single use: consume it before doing anything else. A
     * core can therefore never cause a boot loop, not even if it hangs or
     * crashes immediately - the next boot goes straight to the launcher. */
    m.selected = -1;
    manifest_save(pool, slot, &m);

    if (e->offset < CORE_MANIFEST_RESERVED || (e->offset % CORE_ALIGN) != 0 || e->size == 0)
    {
        ESP_LOGE(TAG, "core entry is not aligned or empty");
        return;
    }

    const uint32_t offset = pool + e->offset;

    esp_partition_pos_t pos = { .offset = offset, .size = e->size };
    esp_image_metadata_t meta = {0};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK)
    {
        ESP_LOGE(TAG, "core image failed verification");
        return;
    }

    /* Hand the core to the stock loader by pointing the launcher's slot at it.
     * This reuses the stock MMU mapping and entry code, so there is no second
     * implementation of the image loader to keep correct. */
    bootloader_state_t patched = bs;
    patched.ota[0].offset = offset;
    patched.ota[0].size = e->size;

    ESP_LOGI(TAG, "booting core '%s' %s at 0x%x", e->name, e->version, offset);
    bootloader_utility_load_boot_image(&patched, 0);
}
