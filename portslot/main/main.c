// Placeholder for the shared port slot. Real ports are flashed over this partition by RG Ports.
#include <rg_system.h>

void app_main(void)
{
    const rg_config_t config = { .sampleRate = 22050, .frameRate = 60 };
    rg_system_init(&config);
    rg_system_switch_app(RG_APP_LAUNCHER, RG_APP_LAUNCHER, NULL, 0, 0);
}
