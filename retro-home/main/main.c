#include "shared.h"

// retro-home: home computers. MSX (fMSX) and Commodore 64 (Frodo Lite). One app partition,
// each core is a component under components/. The launcher tab that started the app is its configNs.
// To add a system: add a component, a <name>_main() in main/, declare it in shared.h and add a branch here.

int core_maxskip = 2;

void app_main(void)
{
    rg_app_t *app = rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = false,  // MSX can boot to BASIC without a ROM; the C64 core reports a missing file itself
    });
    core_maxskip_load();

    RG_LOGI("configNs=%s", app->configNs);

    if (strcmp(app->configNs, "msx") == 0 || strcmp(app->configNs, "msx2") == 0 || strcmp(app->configNs, "fmsx") == 0)
        msx_main();
    else if (strcmp(app->configNs, "c64") == 0 || strcmp(app->configNs, "c64-go") == 0)
        c64_main();
    else
        RG_PANIC("Unknown system for retro-home (configNs)");

    RG_PANIC("Never reached");
}
