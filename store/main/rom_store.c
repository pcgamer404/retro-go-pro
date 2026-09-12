#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "rg_system.h"
#include "rg_gui.h"
#include "rg_storage.h"

#define TAG "ROM_STORE"
#define DOWNLOAD_BUFFER_SIZE 4096

// ---------------------------------------------------------
// EMBEDDED CONSOLE IMAGES
// ---------------------------------------------------------
#include "store_images.h"

static const struct {
    const char *name;
    const char *short_name; // Clean name for the ROM list header
    const char *id;
    const uint8_t *img_data; 
    size_t img_len;          
} store_consoles[] = {
    {"Nintendo Entertainment System", "NES", "nes", nes_img_data, nes_img_len},
    {"Game Boy", "Gameboy", "gb", gb_img_data, gb_img_len},
    {"Game Boy Color", "Gameboy Color", "gbc", gbc_img_data, gbc_img_len},
    {"Game Boy Advance", "Gameboy Advance", "gba", gba_img_data, gba_img_len},
    {"Game & Watch", "Game & Watch", "gw", gw_img_data, gw_img_len},
    {"Super Nintendo", "Super Nintendo", "snes", snes_img_data, snes_img_len},
    {"Sega Game Gear", "Game Gear", "gg", gg_img_data, gg_img_len},
    {"Sega Master System", "Master System", "sms", sms_img_data, sms_img_len},
    {"Sega Mega Drive", "Mega Drive", "md", md_img_data, md_img_len},
    {"Sega SG-1000", "SG-1000", "sg1", sg1_img_data, sg1_img_len},
    {"Neo Geo Pocket", "Neo Geo Pocket", "ngp", ngp_img_data, ngp_img_len},
    {"PICO-8", "PICO-8", "pico8", pico8_img_data, pico8_img_len},
    {"Atari Lynx", "Atari Lynx", "lnx", lnx_img_data, lnx_img_len},
    {"Atari 2600", "Atari 2600", "a26", a26_img_data, a26_img_len},
    {"WonderSwan", "WonderSwan", "ws", ws_img_data, ws_img_len},
    {"ColecoVision", "ColecoVision", "col", col_img_data, col_img_len},
    {"MSX", "MSX", "msx", msx_img_data, msx_img_len},
    {"NEC PC Engine", "PC Engine", "pce", pce_img_data, pce_img_len},
    {"Commodore 64", "Commodore 64", "c64", c64_img_data, c64_img_len}
};
#define CONSOLE_COUNT (sizeof(store_consoles)/sizeof(store_consoles[0]))

// --- CUSTOM MENU HANDLER ---
static rg_gui_event_t store_menu_cb(rg_gui_option_t *option, rg_gui_event_t event) {
    if (event == RG_DIALOG_ENTER) {
        if (option->arg == 1) {
            rg_gui_options_menu();
            return RG_DIALOG_REDRAW; 
        }
        if (option->arg == 2) {
            esp_restart(); // Exit to Launcher
        }
        return RG_DIALOG_CANCEL; // Resume Browsing
    }
    return RG_DIALOG_VOID;
}


// --- REUSABLE DOWNLOAD ENGINE ---

static esp_err_t download_file(const char *url, const char *sd_path, const char *display_name) {
    FILE *f = fopen(sd_path, "wb");
    if (!f) return ESP_FAIL;

    esp_http_client_config_t *config = calloc(1, sizeof(esp_http_client_config_t));
    if (!config) { fclose(f); remove(sd_path); return ESP_FAIL; }
    
    config->url = url;
    config->crt_bundle_attach = esp_crt_bundle_attach;
    config->timeout_ms = 15000;

    esp_http_client_handle_t client = esp_http_client_init(config);
    if (!client || esp_http_client_open(client, 0) != ESP_OK) {
        fclose(f); remove(sd_path); free(config);
        if (client) esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        fclose(f); remove(sd_path); free(config);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buffer = malloc(DOWNLOAD_BUFFER_SIZE);
    if (!buffer) {
        fclose(f); remove(sd_path); free(config);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    long content_length = esp_http_client_get_content_length(client);
    long total_read = 0;
    long next_update = (long)DOWNLOAD_BUFFER_SIZE * 8; 
    int read_bytes = 0;
    bool write_error = false;

    while ((read_bytes = esp_http_client_read(client, buffer, DOWNLOAD_BUFFER_SIZE)) > 0) {
        if (fwrite(buffer, 1, read_bytes, f) != (size_t)read_bytes) {
            write_error = true;
            break;
        }

        total_read += read_bytes;
        if (total_read >= next_update) {
            next_update = total_read + (long)DOWNLOAD_BUFFER_SIZE * 8;
            if (content_length > 0) {
                int pct = (int)((total_read * 100) / content_length);
                rg_gui_draw_message("Downloading...\n%s\n%d%%", display_name, pct);
            } else {
                rg_gui_draw_message("Downloading...\n%s\n%ld KB", display_name, total_read / 1024);
            }
            rg_display_force_redraw();
        }
    }

    free(buffer); fclose(f); free(config);
    esp_http_client_close(client); esp_http_client_cleanup(client);

    if (write_error || read_bytes < 0) {
        remove(sd_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// --- THE DOUBLE FETCH ---

static esp_err_t download_rom_and_art(const char* console_id, const char* filename) {
    char url[512];
    char sd_path[256];
    char enc_filename[256] = {0};

    int j = 0;
    for(int i = 0; filename[i] && j < 250; i++) {
        if(filename[i] == ' ') { enc_filename[j++]='%'; enc_filename[j++]='2'; enc_filename[j++]='0'; }
        else { enc_filename[j++] = filename[i]; }
    }
    
    snprintf(url, sizeof(url), "%s/%s/%s", RG_STORE_BASE_URL, console_id, enc_filename);
    
    char rom_folder[128];
    snprintf(rom_folder, sizeof(rom_folder), "/sd/roms/%s", console_id);
    rg_storage_mkdir("/sd/roms"); 
    rg_storage_mkdir(rom_folder); 
    
    snprintf(sd_path, sizeof(sd_path), "%s/%s", rom_folder, filename);

    if (download_file(url, sd_path, filename) != ESP_OK) {
        return ESP_FAIL;
    }

    char art_filename[128];
    char enc_art_filename[256];
    strncpy(art_filename, filename, sizeof(art_filename) - 1);
    
    char *dot = strrchr(art_filename, '.');
    if (dot) strcpy(dot, ".png");
    else strcat(art_filename, ".png");

    j = 0;
    for(int i = 0; art_filename[i] && j < 250; i++) {
        if(art_filename[i] == ' ') { enc_art_filename[j++]='%'; enc_art_filename[j++]='2'; enc_art_filename[j++]='0'; }
        else { enc_art_filename[j++] = art_filename[i]; }
    }

    snprintf(url, sizeof(url), "%s/%s/%s", RG_STORE_BASE_URL, console_id, enc_art_filename);
    
    char art_folder[128];
    snprintf(art_folder, sizeof(art_folder), "/sd/romart/%s", console_id);
    rg_storage_mkdir("/sd/romart");
    rg_storage_mkdir(art_folder);
    
    snprintf(sd_path, sizeof(sd_path), "%s/%s", art_folder, art_filename);

    rg_gui_draw_message("Fetching Boxart...");
    rg_display_force_redraw();
    
    download_file(url, sd_path, art_filename);

    return ESP_OK;
}

static cJSON *get_entry_newest_first(cJSON *root, int count, int logical_index) { 
    return cJSON_GetArrayItem(root, count - 1 - logical_index); 
}

static const char *entry_filename(cJSON *item) {
    cJSON *f_item = item ? cJSON_GetObjectItem(item, "f") : NULL;
    if (!cJSON_IsString(f_item) || !f_item->valuestring || !f_item->valuestring[0]) return NULL;
    return f_item->valuestring;
}

// --- CUSTOM GUI: ROM LIST LOOP ---

static void open_rom_list(const char* console_id, const char* store_name) {
    char url[256];
    snprintf(url, 256, "%s/%s/%s.json", RG_STORE_BASE_URL, console_id, console_id);

    rg_gui_draw_message("Connecting to server...");
    rg_display_force_redraw();

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { rg_gui_alert("Error", "Could not create HTTP client."); return; }

    char *json_buf = calloc(1, 65536);
    if (!json_buf) { rg_gui_alert("Error", "Out of memory."); esp_http_client_cleanup(client); return; }

    bool fetch_ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) == 200) {
            int total = 0, read_bytes;
            while (total < 65535 && (read_bytes = esp_http_client_read(client, json_buf + total, 65535 - total)) > 0) {
                total += read_bytes;
            }
            fetch_ok = (total > 0);
        }
    }

    esp_http_client_close(client); esp_http_client_cleanup(client);
    if (!fetch_ok) { rg_gui_alert("Network Error", "Could not reach store."); free(json_buf); return; }

    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    if (!root) { rg_gui_alert("Error", "Store is empty or offline."); return; }

    int count = cJSON_GetArraySize(root);
    if (count <= 0) { rg_gui_alert("Store", "No games listed."); cJSON_Delete(root); return; }

    cJSON **valid_items = malloc(count * sizeof(cJSON*));
    if (!valid_items) { rg_gui_alert("Error", "Out of memory."); cJSON_Delete(root); return; }
    
    int valid_count = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        const char *file = entry_filename(item);
        if (file) {
            if (strstr(file, ".png") != NULL || strstr(file, ".PNG") != NULL) continue; 
            valid_items[valid_count++] = item;
        }
    }

    if (valid_count <= 0) {
        rg_gui_alert("Store", "No ROMs found.");
        free(valid_items);
        cJSON_Delete(root);
        return;
    }

    int selected = 0;
    int prev_selected = 0;
    int scroll = 0;
    int prev_scroll = 0;
    int max_visible = 9; 
    
    int prev_pad = 0;
    int hold_timer = 0;
    bool needs_full_redraw = true;

    // Centered Store Header Setup
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s Store", store_name);
    int hdr_w = strlen(hdr) * 8;
    int hdr_x = (320 - hdr_w) / 2;

    rg_display_clear(C_BLACK); 
    rg_gui_draw_text(hdr_x, 10, 0, hdr, C_LIGHT_GRAY, C_BLACK, 0);

    while (1) {
        int pad = rg_input_read_gamepad();
        int pressed = 0;

        if (pad != prev_pad) {
            pressed = pad & ~prev_pad; 
            hold_timer = 0;            
        } else if (pad != 0) {
            hold_timer++;
            if (hold_timer > 15) {           
                if (hold_timer % 2 == 0) {   
                    pressed = pad;
                }
            }
        } else {
            hold_timer = 0;
        }
        prev_pad = pad;

        if (pressed) {
            prev_selected = selected;
            if (pressed & RG_KEY_DOWN) { if (selected < valid_count - 1) selected++; else selected = 0; }
            if (pressed & RG_KEY_UP) { if (selected > 0) selected--; else selected = valid_count - 1; }
            if (pressed & RG_KEY_RIGHT) { selected += max_visible; if (selected >= valid_count) selected = valid_count - 1; }
            if (pressed & RG_KEY_LEFT) { selected -= max_visible; if (selected < 0) selected = 0; }
            
            if (pressed & RG_KEY_B) {
                break;
            }

            if (pressed & RG_KEY_MENU) {
                rg_gui_option_t menu_opts[4];
                memset(menu_opts, 0, sizeof(menu_opts));
                
                menu_opts[0].label = "Resume Browsing";
                menu_opts[0].flags = RG_DIALOG_FLAG_NORMAL;
                menu_opts[0].update_cb = store_menu_cb;
                menu_opts[0].arg = 0;
                
                menu_opts[1].label = "Options";
                menu_opts[1].flags = RG_DIALOG_FLAG_NORMAL;
                menu_opts[1].update_cb = store_menu_cb;
                menu_opts[1].arg = 1;
                
                menu_opts[2].label = "Exit to Launcher";
                menu_opts[2].flags = RG_DIALOG_FLAG_NORMAL;
                menu_opts[2].update_cb = store_menu_cb;
                menu_opts[2].arg = 2;
                
                menu_opts[3] = (rg_gui_option_t)RG_DIALOG_END;
                
                rg_gui_dialog("Store Menu", menu_opts, 0);
                
                prev_pad = rg_input_read_gamepad(); 
                rg_display_clear(C_BLACK);
                rg_gui_draw_text(hdr_x, 10, 0, hdr, C_LIGHT_GRAY, C_BLACK, 0);
                needs_full_redraw = true;
            }
            
            if (pressed & RG_KEY_A) {
                cJSON *item = valid_items[valid_count - 1 - selected];
                const char *filename = entry_filename(item);
                if (filename) {
                    rg_gui_draw_message("Initializing...\n%s", filename);
                    rg_display_force_redraw();
                    if (download_rom_and_art(console_id, filename) == ESP_OK) {
                        rg_gui_alert("Success!", "Game saved to SD Card.");
                    } else {
                        rg_gui_alert("Error", "Download Failed.\nCheck SD path & Wi-Fi.");
                    }
                    
                    prev_pad = rg_input_read_gamepad(); 
                    rg_display_clear(C_BLACK); 
                    rg_gui_draw_text(hdr_x, 10, 0, hdr, C_LIGHT_GRAY, C_BLACK, 0);
                    needs_full_redraw = true; 
                }
            }

            prev_scroll = scroll;
            if (selected < scroll) scroll = selected;
            if (selected >= scroll + max_visible) scroll = selected - max_visible + 1;
            if (scroll != prev_scroll) needs_full_redraw = true;
        }

        if (needs_full_redraw) {
            for (int i = 0; i < max_visible; i++) {
                int idx = scroll + i;
                if (idx >= valid_count) {
                    rg_gui_draw_text(10, 35 + (i * 20), 300, "", C_WHITE, C_BLACK, 0);
                    continue;
                }
                
                cJSON *item = valid_items[valid_count - 1 - idx];
                const char *file = entry_filename(item);
                if (!file) continue;

                rg_color_t color = (idx == selected) ? C_GOLD : C_WHITE;
                char local_path[128], tmp_lbl[128], label[128];
                
                snprintf(local_path, sizeof(local_path), "/sd/roms/%s/%s", console_id, file);
                bool have_it = rg_storage_exists(local_path);
                
                snprintf(tmp_lbl, sizeof(tmp_lbl), "%s%s", have_it ? "[X] " : "", file);
                snprintf(label, sizeof(label), "%-38.38s", tmp_lbl); 
                
                rg_gui_draw_text(10, 35 + (i * 20), 300, label, color, C_BLACK, 0);
            }
            rg_display_force_redraw();
            needs_full_redraw = false;
        } 
        else if (selected != prev_selected) {
            if (prev_selected >= scroll && prev_selected < scroll + max_visible) {
                int prev_i = prev_selected - scroll;
                const char *file = entry_filename(valid_items[valid_count - 1 - prev_selected]);
                if (file) {
                    char local_path[128], tmp_lbl[128], label[128];
                    snprintf(local_path, sizeof(local_path), "/sd/roms/%s/%s", console_id, file);
                    snprintf(tmp_lbl, sizeof(tmp_lbl), "%s%s", rg_storage_exists(local_path) ? "[X] " : "", file);
                    snprintf(label, sizeof(label), "%-38.38s", tmp_lbl);
                    
                    rg_gui_draw_text(10, 35 + (prev_i * 20), 300, label, C_WHITE, C_BLACK, 0);
                }
            }

            if (selected >= scroll && selected < scroll + max_visible) {
                int cur_i = selected - scroll;
                const char *file = entry_filename(valid_items[valid_count - 1 - selected]);
                if (file) {
                    char local_path[128], tmp_lbl[128], label[128];
                    snprintf(local_path, sizeof(local_path), "/sd/roms/%s/%s", console_id, file);
                    snprintf(tmp_lbl, sizeof(tmp_lbl), "%s%s", rg_storage_exists(local_path) ? "[X] " : "", file);
                    snprintf(label, sizeof(label), "%-38.38s", tmp_lbl);
                    
                    rg_gui_draw_text(10, 35 + (cur_i * 20), 300, label, C_GOLD, C_BLACK, 0);
                }
            }
            rg_display_force_redraw();
            prev_selected = selected;
        }
        
        rg_task_delay(20);
    }
    free(valid_items);
    cJSON_Delete(root);
}


// --- CUSTOM GUI: CAROUSEL LOOP ---

void show_rom_store_menu() {
    int selected = 0;
    int prev_pad = 0;
    int hold_timer = 0;
    bool needs_full_redraw = true;

    // The Main Header is now static and centered!
    char *main_hdr = "Store";
    int main_hdr_w = strlen(main_hdr) * 8;
    int main_hdr_x = (320 - main_hdr_w) / 2;

    while (1) {
        int pad = rg_input_read_gamepad();
        int pressed = 0;

        if (pad != prev_pad) {
            pressed = pad & ~prev_pad; 
            hold_timer = 0;            
        } else if (pad != 0) {
            hold_timer++;
            if (hold_timer > 20) {          
                if (hold_timer % 4 == 0) {  
                    pressed = pad;
                }
            }
        } else {
            hold_timer = 0;
        }
        prev_pad = pad;

        if (pressed) {
            if (pressed & RG_KEY_RIGHT) { selected++; if (selected >= CONSOLE_COUNT) selected = 0; needs_full_redraw = true; }
            if (pressed & RG_KEY_LEFT) { selected--; if (selected < 0) selected = CONSOLE_COUNT - 1; needs_full_redraw = true; }
            
            if (pressed & RG_KEY_B) {
                break; 
            }

            if (pressed & RG_KEY_MENU) {
                rg_gui_option_t menu_opts[4];
                memset(menu_opts, 0, sizeof(menu_opts));
                
                menu_opts[0].label = "Resume Browsing";
                menu_opts[0].flags = RG_DIALOG_FLAG_NORMAL;
                menu_opts[0].update_cb = store_menu_cb;
                menu_opts[0].arg = 0;
                
                menu_opts[1].label = "Options";
                menu_opts[1].flags = RG_DIALOG_FLAG_NORMAL;
                menu_opts[1].update_cb = store_menu_cb;
                menu_opts[1].arg = 1;
                
                menu_opts[2].label = "Exit to Launcher";
                menu_opts[2].flags = RG_DIALOG_FLAG_NORMAL;
                menu_opts[2].update_cb = store_menu_cb;
                menu_opts[2].arg = 2;
                
                menu_opts[3] = (rg_gui_option_t)RG_DIALOG_END;
                
                rg_gui_dialog("Store Menu", menu_opts, 0);
                
                prev_pad = rg_input_read_gamepad(); 
                rg_display_clear(C_BLACK);
                needs_full_redraw = true;
            }
            
            if (pressed & RG_KEY_A) {
                // Pass the ID and the new short_name to the list!
                open_rom_list(store_consoles[selected].id, store_consoles[selected].short_name);
                
                prev_pad = rg_input_read_gamepad(); 
                rg_display_clear(C_BLACK);
                needs_full_redraw = true; 
            }
        }

        if (needs_full_redraw) {
            rg_display_clear(C_BLACK);
            
            rg_gui_draw_text(main_hdr_x, 10, 0, main_hdr, C_LIGHT_GRAY, C_BLACK, 0);
            
            // Reduced to < %s > to fix the rightward shifting issue
            char carousel_text[64];
            snprintf(carousel_text, sizeof(carousel_text), "< %s >", store_consoles[selected].name);
            int text_width = strlen(carousel_text) * 8; 
            int x_pos = (320 - text_width) / 2;
            rg_gui_draw_text(x_pos, 200, 0, carousel_text, C_GOLD, C_BLACK, 0);

            if (store_consoles[selected].img_data != NULL) {
                rg_surface_t *img = rg_surface_load_image(
                    store_consoles[selected].img_data, 
                    store_consoles[selected].img_len, 
                    0
                );
                
                if (img) {
                    int img_x = (320 - img->width) / 2;
                    int img_y = 35; 
                    
                    rg_gui_draw_image(img_x, img_y, img->width, img->height, false, img);
                    rg_surface_free(img);
                }
            } else {
                rg_gui_draw_text(110, 100, 0, "[ MISSING ART ]", C_RED, C_BLACK, 0);
            }

            rg_display_force_redraw();
            needs_full_redraw = false; 
        } 
        
        rg_task_delay(20); 
    }
}