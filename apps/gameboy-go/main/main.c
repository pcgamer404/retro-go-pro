#include "shared.h"

int core_maxskip = 2;

void app_main(void)
{
    rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = true,
    });
    core_maxskip_load();
    gbc_main();
    RG_PANIC("Emulator returned unexpectedly");
}