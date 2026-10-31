#include "shared.h"


int core_maxskip = 2;

void app_main(void)
{
    rg_app_t *app = rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60, // This was the old default, let's keep it
        .storageRequired = true,
        .romRequired = true,
    });
    core_maxskip_load();

    RG_LOGI("configNs=%s", app->configNs);

    if (strcmp(app->configNs, "gbc") == 0 || strcmp(app->configNs, "gb") == 0)
        gbc_main();
    else if (strcmp(app->configNs, "nes") == 0 || strcmp(app->configNs, "fds") == 0)
        nes_main();
    else if (strcmp(app->configNs, "pce") == 0)
        pce_main();
    else if (strcmp(app->configNs, "sms") == 0)
        sms_main();
    else if (strcmp(app->configNs, "sg1") == 0 || strcmp(app->configNs, "sg") == 0 || strcmp(app->configNs, "sg1000") == 0)
        sms_main();   // SG-1000: main_sms.c selects the SG-1000 console from configNs / .sg extension
    else if (strcmp(app->configNs, "snes") == 0 || strcmp(app->configNs, "sfc") == 0)
        snes_main();
    else if (strcmp(app->configNs, "gg") == 0)
        sms_main();
    else if (strcmp(app->configNs, "col") == 0)
        sms_main();
    else if (strcmp(app->configNs, "gw") == 0)
        gw_main();
else
        launcher_main();

    RG_PANIC("Never reached");
}
