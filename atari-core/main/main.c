#include "shared.h"

// atari-core: Atari Lynx (handy), Atari 2600 (StellaDS), Atari 5200 (Atari800/MCUME), Atari 7800 (ProSystem).
// The launcher tab that started the app is its configNs.

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

    if (strcmp(app->configNs, "lnx") == 0)
        lynx_main();
    else if (strcmp(app->configNs, "a26") == 0 || strcmp(app->configNs, "stella") == 0)
        a26_main();
    else if (strcmp(app->configNs, "a52") == 0)
        a52_main();
    else if (strcmp(app->configNs, "a78") == 0)
        a78_main();
    else
        RG_PANIC("Unknown system for atari-core (configNs)");

    RG_PANIC("Never reached");
}
