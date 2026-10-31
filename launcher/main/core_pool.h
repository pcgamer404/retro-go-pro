#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <rg_gui.h>

#include "core_manifest.h"
#include "core_assets.h"

/* Loads the manifest from the pool partition. Call once after rg_system_init().
 * Safe to call when the partition is missing: the pool is then reported
 * unavailable and every other function becomes a no-op returning failure. */
void core_pool_init(void);

bool core_pool_available(void);
int core_pool_count(void);
const core_entry_t *core_pool_entry(int index);
int core_pool_selected(void);

/* Installs (or replaces) a core from a .bin on SD. Returns the entry index,
 * or -1 on failure. Shows its own progress/error dialogs. */
int core_pool_install(const char *path);

/* Marks a core as the one the bootloader should start. -1 selects the
 * launcher. Takes effect on the next reboot. */
bool core_pool_select(int index);

/* rg_system hooks: let the launcher see pool cores as regular apps. */
bool core_pool_have_app(const char *partition);
bool core_pool_entry_matches_app(int index, const char *partition);
bool core_pool_switch_app(const char *partition, const char *name, const char *args, int save_slot, uint32_t flags);

/* Removes a core from the manifest. Its flash space is reused by the next
 * install; the image bytes are not erased. */
bool core_pool_remove(int index);

/* Drops every entry. */
bool core_pool_format(void);

uint32_t core_pool_used(void);
uint32_t core_pool_total(void);

/* Launcher settings menu entry. */
rg_gui_event_t core_manager_cb(rg_gui_option_t *option, rg_gui_event_t event);
