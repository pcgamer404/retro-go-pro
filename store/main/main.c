#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "esp_system.h" 
#include "rg_system.h"
#include "rg_network.h"
#include "rg_audio.h" // Added audio header
#include "rg_gui.h"
#include "rom_store.h"

void app_main(void)
{
    rg_system_init(&(const rg_config_t){
        .isLauncher = true, 
        .storageRequired = true,
    });

    // Initialize the audio driver so the Options Menu doesn't panic
    rg_audio_init(44100); 

    rg_network_init();

    if (rg_network_get_info().state != RG_NETWORK_CONNECTED) {
        rg_gui_draw_message("Starting Wi-Fi...");
        rg_display_force_redraw();
        
        int timeout = 100;
        while (rg_network_get_info().state != RG_NETWORK_CONNECTED && timeout > 0) {
            rg_task_delay(100);
            timeout--;
        }
    }

    if (rg_network_get_info().state != RG_NETWORK_CONNECTED) {
        rg_gui_alert("Network Error", "Could not connect to Wi-Fi.");
        esp_restart(); 
    }

    show_rom_store_menu();
    
    esp_restart(); 
}