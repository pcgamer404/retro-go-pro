#include "shared.h"

// retro-extra: Neo Geo Pocket / Color (RACE). Kept in its own app because RACE's static tables (~26 KB of
// internal RAM) would otherwise be paid by every retro-core emulator.
// Stella moved to atari-core, Arcade 3D is now the standalone "arcade3d" app.

int core_maxskip = 2;

void app_main(void)
{
    rg_app_t *app = rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = true,
    });
    core_maxskip_load();

    RG_LOGI("configNs=%s", app->configNs);

    if (strcmp(app->configNs, "ngp") == 0 || strcmp(app->configNs, "ngpc") == 0)
        ngp_main();
    else
        RG_PANIC("Unknown system for retro-extra (configNs)");

    RG_PANIC("Never reached");
}
