// RG Ports - port launching for the launcher.
//
// Ports are normal Retro-Go app images (.bin) placed in SD:/roms/rgports/ (optionally with
// their data next to them). Launching a port flashes its .bin into the shared "portslot"
// partition (skipped if that exact image is already there) and reboots into it.
//
// This file only provides the file menu for the RG Ports tab. All other tabs keep using
// application_show_file_menu() unchanged.
#include "rg_ports.h"

#if RG_ENABLE_PORTS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <strings.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <rg_system.h>

#include "bookmarks.h"
#include "gui.h"

#define CHUNK_SIZE    (16 * 1024)
#define ESP_MAGIC     0xE9
#define CHIP_ESP32S3  0x0009

bool rg_ports_owns(const retro_file_t *file)
{
    return file && file->app && strcmp(file->app->short_name, RG_PORTS_NAME) == 0;
}

static const esp_partition_t *slot_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, RG_PORTS_PARTITION);
}

static void file_path(const retro_file_t *file, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", file->folder, file->name);
}

// Ports whose data lives in their own SD folder, exactly like the standalone app uses.
// These get launched with a path inside that folder (instead of the .bin's path) so the
// port finds its data in SD:/roms/<folder>/ and nothing has to be copied into roms/rgports.
// Matched against the .bin file name without extension, case-insensitive. Add a line to add a port.
static const struct { const char *name; const char *rom; } DATA_PATHS[] = {
    {"openlara",  RG_BASE_PATH_ROMS "/openlara/openlara.bin"},   // OpenLara reads <folder>/data/
    {"quake",     RG_BASE_PATH_ROMS "/quake/id1/pak0.pak"},      // Quake reads pak0/pak1 from that folder
    {"quake-go",  RG_BASE_PATH_ROMS "/quake/id1/pak0.pak"},
};

// The path the port is started with, and the one its save states / SRAM are keyed on.
static void port_rom_path(const retro_file_t *file, char *out, size_t size)
{
    char base[64];
    snprintf(base, sizeof(base), "%s", file->name);
    char *dot = strrchr(base, '.');
    if (dot)
        *dot = 0;

    for (size_t i = 0; i < sizeof(DATA_PATHS) / sizeof(DATA_PATHS[0]); i++)
    {
        if (strcasecmp(base, DATA_PATHS[i].name) == 0)
        {
            snprintf(out, size, "%s", DATA_PATHS[i].rom);
            return;
        }
    }
    file_path(file, out, size);
}

// Is this exact image already in the slot? ESP app images end with a SHA-256 of the whole image
// (header byte 23 = hash_appended), so comparing header + that trailing hash with what is really
// in flash is enough. It reads 48 bytes instead of the whole file, and it checks the flash
// itself, so it stays correct if the slot was re-flashed by something else.
static bool slot_has_image(const esp_partition_t *part, FILE *fp, size_t size, const uint8_t *hdr)
{
    uint8_t want[32], have[32], head[16];
    if (hdr[23] != 1 || size < 64)
        return false;
    if (fseek(fp, size - 32, SEEK_SET) != 0 || fread(want, 1, 32, fp) != 32)
        return false;
    return esp_partition_read(part, size - 32, have, 32) == ESP_OK && memcmp(want, have, 32) == 0
        && esp_partition_read(part, 0, head, 16) == ESP_OK && memcmp(head, hdr, 16) == 0;
}

// Flash <path> into the slot unless that exact image is already there.
// Returns true when the slot holds the image and it is safe to boot it.
static bool prepare_slot(const char *path, bool force)
{
    const esp_partition_t *part = slot_partition();
    if (!part)
    {
        rg_gui_alert("RG Ports", "Partition 'portslot' not found.\nAdd it to rg_tool.py and reflash.");
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        rg_gui_alert("RG Ports", "Cannot open file");
        return false;
    }

    bool ok = false;
    uint8_t *buf = malloc(CHUNK_SIZE);
    uint8_t hdr[24];
    size_t n;

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (!buf)
    {
        rg_gui_alert("RG Ports", "Out of memory");
        goto done;
    }
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) || hdr[0] != ESP_MAGIC
        || (hdr[12] | (hdr[13] << 8)) != CHIP_ESP32S3)
    {
        rg_gui_alert("RG Ports", "Not an ESP32-S3 app image");
        goto done;
    }
    if (size > part->size)
    {
        rg_gui_alert("RG Ports", "Too big for the port slot");
        goto done;
    }

    if (!force && slot_has_image(part, fp, size, hdr))
    {
        ok = true;
        goto done;
    }

    rg_gui_draw_hourglass();
    esp_ota_handle_t handle;
    if (esp_ota_begin(part, size, &handle) != ESP_OK)
    {
        rg_gui_alert("RG Ports", "Flash failed");
        goto done;
    }

    bool write_ok = true;
    fseek(fp, 0, SEEK_SET);
    while (write_ok && (n = fread(buf, 1, CHUNK_SIZE, fp)) > 0)
        write_ok = esp_ota_write(handle, buf, n) == ESP_OK;

    if (!write_ok)
        esp_ota_abort(handle);
    if (!write_ok || esp_ota_end(handle) != ESP_OK)
    {
        rg_gui_alert("RG Ports", "Flash failed");
        goto done;
    }

    ok = true;

done:
    free(buf);
    fclose(fp);
    return ok;
}

// Flash if needed, then reboot into the port. Only returns on failure.
static void launch(retro_file_t *file, bool force, int slot)
{
    char path[RG_PATH_MAX + 1];
    char rompath[RG_PATH_MAX + 1];
    char id[48];

    file_path(file, path, sizeof(path));         // the .bin that gets flashed
    port_rom_path(file, rompath, sizeof(rompath)); // what the port is told to open
    if (!prepare_slot(path, force))
        return;

    // Same app name the port has always been started with: the .bin name without extension
    snprintf(id, sizeof(id), "%s", file->name);
    char *dot = strrchr(id, '.');
    if (dot)
        *dot = 0;

    // Flashing blocks the GUI for seconds, so the system monitor decides the launcher is
    // "unresponsive" and sits in its MENU-key wait loop. If we shut down while it is in there,
    // the stopped input driver reads as "all keys pressed", it draws on the dead display and
    // asserts, and the port then starts in panic recovery and bounces back to the launcher.
    // So: show that we're alive, then give the monitor one loop to notice before switching.
    rg_system_tick(0);
    rg_task_delay(1200);

    int flags = (gui.startup_mode ? RG_BOOT_ONCE : 0) | (slot != -1 ? RG_BOOT_RESUME : 0);
    char *part = strdup(RG_PORTS_PARTITION);
    char *name = strdup(id);
    char *rom = strdup(rompath);
    bookmark_add(BOOK_TYPE_RECENT, file); // This could relocate *file, we no longer need it
    rg_system_switch_app(part, name, rom, slot, flags);
}

// Same layout as the standard "File properties" dialog
static void show_properties(retro_file_t *file)
{
    char path[RG_PATH_MAX + 1];
    char filesize[16];
    char filecrc[16] = "Compute";

    file_path(file, path, sizeof(path));
    rg_stat_t info = rg_storage_stat(path);
    if (!info.exists)
    {
        rg_gui_alert(_("File not found"), file->name);
        return;
    }

    rg_gui_option_t options[] = {
        {0, _("Name"), (char *)file->name, 1, NULL},
        {0, _("Folder"), (char *)file->folder, 1, NULL},
        {0, _("Size"), filesize, 1, NULL},
        {3, _("CRC32"), filecrc, 1, NULL},
        RG_DIALOG_SEPARATOR,
        {5, _("Delete file"), NULL, 1, NULL},
        {1, _("Close"), NULL, 1, NULL},
        RG_DIALOG_END,
    };

    sprintf(filesize, "%d KB", (int)info.size / 1024);

    while (true)
    {
        if (file->checksum)
            sprintf(filecrc, "%08X", (int)file->checksum);

        switch (rg_gui_dialog(_("File properties"), options, -1))
        {
        case 3:
            application_get_file_crc32(file);
            continue;
        case 5:
            if (rg_gui_confirm(_("Delete selected file?"), 0, 0) && remove(path) == 0)
            {
                bookmark_remove(BOOK_TYPE_FAVORITE, file);
                bookmark_remove(BOOK_TYPE_RECENT, file);
                file->type = RETRO_TYPE_INVALID;
                gui_event(TAB_REFRESH, gui_get_current_tab());
                return;
            }
            continue;
        default:
            return;
        }
    }
}

void rg_ports_file_menu(retro_file_t *file)
{
    char *rom_path = malloc(RG_PATH_MAX + 1);
    port_rom_path(file, rom_path, RG_PATH_MAX + 1); // same path the port will use for its saves

    char *sram_path = rg_emu_get_path(RG_PATH_SAVE_SRAM, rom_path);
    rg_emu_states_t *savestates = rg_emu_get_states(rom_path, 4);
    bool has_save = savestates->used > 0;
    bool has_sram = rg_storage_exists(sram_path);
    bool is_fav = bookmark_exists(BOOK_TYPE_FAVORITE, file);
    int slot = -1;

    rg_gui_option_t choices[] = {
        {0, _("Resume game"), NULL, has_save, NULL},
        {1, _("Launch"), NULL, 1, NULL},
        {2, _("Reinstall"), NULL, 1, NULL},
        RG_DIALOG_SEPARATOR,
        {3, is_fav ? _("Del favorite") : _("Add favorite"), NULL, 1, NULL},
        {4, _("Delete save"), NULL, has_save || has_sram, NULL},
        RG_DIALOG_SEPARATOR,
        {5, _("Properties"), NULL, 1, NULL},
        RG_DIALOG_END,
    };

    switch (rg_gui_dialog(NULL, choices, has_save ? 0 : 1))
    {
    case 0:
        if ((slot = rg_gui_savestate_menu(_("Resume"), rom_path)) == -1)
            break;
        launch(file, false, slot);
        break;

    case 1:
        launch(file, false, -1);
        break;

    case 2:
        launch(file, true, -1); // re-flash even if the slot already holds this image
        break;

    case 3:
        if (is_fav)
            bookmark_remove(BOOK_TYPE_FAVORITE, file);
        else
            bookmark_add(BOOK_TYPE_FAVORITE, file); // This could relocate *file
        break;

    case 4:
        while ((slot = rg_gui_savestate_menu(_("Delete save?"), rom_path)) != -1)
        {
            remove(savestates->slots[slot].preview);
            remove(savestates->slots[slot].file);
        }
        if (has_sram && rg_gui_confirm(_("Delete sram file?"), 0, 0))
            remove(sram_path);
        break;

    case 5:
        show_properties(file);
        break;

    default:
        break;
    }

    free(rom_path);
    free(sram_path);
    free(savestates);
}

#endif // RG_ENABLE_PORTS
