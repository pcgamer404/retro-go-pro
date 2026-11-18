#include "devilutionx_port.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "devilutionx-port";
static const char *const GAME_DIR = "/sd/roms/diablo";

static bool log_data_file(const char *filename, bool required)
{
    char path[128];
    struct stat info;

    const int n = snprintf(path, sizeof(path), "%s/%s", GAME_DIR, filename);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        ESP_LOGE(TAG, "Game data path is too long for %s", filename);
        return false;
    }

    if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
        ESP_LOGI(TAG, "Game data %s: found (%" PRIu32 " bytes)", path,
                 (uint32_t)info.st_size);
        return true;
    }

    if (required) {
        ESP_LOGE(TAG, "Required game data missing: %s", path);
    } else {
        ESP_LOGW(TAG, "Optional expansion data not found: %s", path);
    }
    return false;
}

bool devilutionx_check_game_data(void)
{
    ESP_LOGI(TAG, "Checking game data directory: %s", GAME_DIR);

    // The MPQs are only stat'ed here; the large DIABDAT.MPQ is never loaded
    // into RAM. The real DevilutionX MPQ loader is not connected yet.
    const bool diablo_found = log_data_file("DIABDAT.MPQ", true);
    (void)log_data_file("hellfire.mpq", false);
    (void)log_data_file("hfmonk.mpq", false);
    (void)log_data_file("hfmusic.mpq", false);
    (void)log_data_file("hfvoice.mpq", false);

    return diablo_found;
}

void devilutionx_component_checkpoint(void)
{
    ESP_LOGI(TAG,
             "Component linked successfully. Upstream DevilutionX engine is not "
             "compiled or initialized in this checkpoint.");
}
