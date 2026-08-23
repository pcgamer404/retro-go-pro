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

#define TAG "ROM_STORE"

#define DOWNLOAD_BUFFER_SIZE 4096

// --- COVER ART NOTE ---
// We intentionally do NOT list/download cover art through the store. Many
// uploaders won't bother adding one, and people who do want full romart
// coverage tend to reach for a CRC-matched bulk art pack instead (matching
// romart to the actual ROM CRC rather than a filename). Trying to bundle
// art here would just mean a lot of missing/mismatched images for no real
// benefit. If that changes, the natural place to add it later is a
// "cover": "name.png" field per manifest entry, downloaded to
// /sd/roms/<console>/ alongside the rom (same folder our romart patch
// already scans).

// --- PAGINATION ---
// The dialog list has room for about 11 lines of game names (not counting
// the title bar). Long names wrap to 2+ lines, so a page holds fewer entries
// when names are long.
#define GAMES_SCREEN_LINES 11

typedef struct {
    const char *console;
    char filename[128];
    int index; // this entry's position within the current page (0-based)
} download_ctx_t;

// Set by download_game_cb: which direction to move pages (-1/+1/0), and
// whether the new page should open with its LAST item selected (continuous
// scroll feel when wrapping via Up/Down) vs its first item (Left/Right).
static int g_page_delta = 0;
static bool g_land_on_last = false;
static int g_last_focus_arg = -1;
static int g_page_item_count = 0;

// --- DOWNLOAD ENGINE ---

static esp_err_t download_rom(const char* console, const char* filename) {
    // 1. Move large variables from the Stack to the Heap
    char *url = malloc(512);
    char *sd_folder = malloc(128);
    char *sd_path = malloc(256);
    char *enc_filename = calloc(1, 256);

    if (!url || !sd_folder || !sd_path || !enc_filename) {
        free(url); free(sd_folder); free(sd_path); free(enc_filename);
        return ESP_FAIL;
    }

    int j = 0;
    for(int i = 0; filename[i] && j < 250; i++) {
        if(filename[i] == ' ') { enc_filename[j++]='%'; enc_filename[j++]='2'; enc_filename[j++]='0'; }
        else { enc_filename[j++] = filename[i]; }
    }
    
    snprintf(url, 512, "%s/%s/%s", RG_STORE_BASE_URL, console, enc_filename);
    
    rg_storage_mkdir("/sd/roms"); 
    snprintf(sd_folder, 128, "/sd/roms/%s", console);
    rg_storage_mkdir(sd_folder); 
    
    snprintf(sd_path, 256, "%s/%s", sd_folder, filename);

    FILE *f = fopen(sd_path, "wb");
    if (!f) {
        free(url); free(sd_folder); free(sd_path); free(enc_filename);
        return ESP_FAIL;
    }

    esp_http_client_config_t *config = calloc(1, sizeof(esp_http_client_config_t));
    if (!config) {
        fclose(f);
        remove(sd_path);
        free(url); free(sd_folder); free(sd_path); free(enc_filename);
        return ESP_FAIL;
    }
    config->url = url;
    config->crt_bundle_attach = esp_crt_bundle_attach;
    config->timeout_ms = 15000;

    esp_http_client_handle_t client = esp_http_client_init(config);
    if (!client || esp_http_client_open(client, 0) != ESP_OK) {
        fclose(f);
        remove(sd_path);
        if (client) esp_http_client_cleanup(client);
        free(url); free(sd_folder); free(sd_path); free(enc_filename); free(config);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);

    // A 404/500 page would otherwise be saved as if it were the ROM.
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        fclose(f);
        remove(sd_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(url); free(sd_folder); free(sd_path); free(enc_filename); free(config);
        return ESP_FAIL;
    }

    char *buffer = malloc(DOWNLOAD_BUFFER_SIZE);
    if (!buffer) {
        fclose(f);
        remove(sd_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(url); free(sd_folder); free(sd_path); free(enc_filename); free(config);
        return ESP_FAIL;
    }

    // -1 if the server didn't send Content-Length (e.g. chunked encoding) --
    // in that case we just show KB downloaded instead of a percentage.
    long content_length = esp_http_client_get_content_length(client);
    long total_read = 0;
    long next_update = (long)DOWNLOAD_BUFFER_SIZE * 8; // throttle: ~32KB between redraws

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
                rg_gui_draw_message("Downloading...\n%s\n%d%%", filename, pct);
            } else {
                rg_gui_draw_message("Downloading...\n%s\n%ld KB", filename, total_read / 1024);
            }
            rg_display_force_redraw();
        }
    }

    free(buffer);
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    bool failed = write_error || read_bytes < 0;
    if (failed) {
        // On any failure, remove the partial/0-byte file instead of leaving a
        // broken ROM behind that would show up (and fail to launch) in the library.
        remove(sd_path);
    }

    // 2. Always free Heap memory when done to prevent leaks
    free(url); free(sd_folder); free(sd_path); free(enc_filename); free(config);
    return failed ? ESP_FAIL : ESP_OK;
}

// --- RETRO-GO UI INTEGRATION ---

static rg_gui_event_t download_game_cb(rg_gui_option_t *dialog, rg_gui_event_t event) {
    if (event == RG_DIALOG_ENTER) {
        download_ctx_t *ctx = (download_ctx_t*)dialog->arg;
        
        // This draws to the screen instantly WITHOUT waiting for you to press 'A'
        rg_gui_draw_message("Downloading...\n%s\n0%%", ctx->filename);
        rg_display_force_redraw();
        
        if (download_rom(ctx->console, ctx->filename) == ESP_OK) {
            rg_gui_alert("Success!", "Game saved to SD Card.");
        } else {
            rg_gui_alert("Error", "Download Failed.\nCheck SD path & Wi-Fi.");
        }
        return RG_DIALOG_REDRAW; 
    }
    // Left/Right: hand the direction back to open_console_store_cb and force
    // this page's dialog to close (returning CANCEL exits rg_gui_dialog's
    // loop), so the caller can rebuild and show the next/previous page.
    if (event == RG_DIALOG_PREV) {
        g_page_delta = -1;
        g_land_on_last = false; // land on first item of the previous page
        return RG_DIALOG_CANCEL;
    }
    if (event == RG_DIALOG_NEXT) {
        g_page_delta = 1;
        g_land_on_last = false; // land on first item of the next page
        return RG_DIALOG_CANCEL;
    }
    // Up/Down: rg_gui_dialog already wraps selection within a page (top <->
    // bottom). We detect that wrap here (via the FOCUS_GAINED transition)
    // and turn it into a page change too, so Up/Down feels continuous
    // across page boundaries instead of stopping dead at the edges.
    if (event == RG_DIALOG_FOCUS_GAINED) {
        download_ctx_t *ctx = (download_ctx_t*)dialog->arg;
        int cur = ctx->index;
        if (g_page_item_count > 1) {
            if (g_last_focus_arg == 0 && cur == g_page_item_count - 1) {
                g_page_delta = -1;
                g_land_on_last = true; // continue scrolling up into the previous page's last item
                return RG_DIALOG_CANCEL;
            }
            if (g_last_focus_arg == g_page_item_count - 1 && cur == 0) {
                g_page_delta = 1;
                g_land_on_last = false; // continue scrolling down into the next page's first item
                return RG_DIALOG_CANCEL;
            }
        }
        g_last_focus_arg = cur;
        return RG_DIALOG_VOID;
    }
    return RG_DIALOG_VOID;
}

// Exact wrapped-line count for this label, matching the real dialog
// renderer's own math (see rg_gui_estimate_label_lines) -- so pack_page()
// can never disagree with what's actually shown on screen.
static int estimate_lines(const char *label) {
    return rg_gui_estimate_label_lines(label);
}

// Games are shown newest-first by simply walking the manifest array
// backwards -- i.e. we assume your upload process appends new entries to
// the end of the JSON array. No extra manifest field needed. (If your
// upload script doesn't append to the end, this will need an explicit
// "d"/order field instead.)
static cJSON *get_entry_newest_first(cJSON *root, int count, int logical_index) {
    return cJSON_GetArrayItem(root, count - 1 - logical_index);
}

static const char *entry_filename(cJSON *item) {
    cJSON *f_item = item ? cJSON_GetObjectItem(item, "f") : NULL;
    if (!cJSON_IsString(f_item) || !f_item->valuestring || !f_item->valuestring[0])
        return NULL;
    return f_item->valuestring;
}

// Greedily packs entries starting at page_start until adding one more would
// exceed GAMES_SCREEN_LINES total lines. Always includes at least one entry
// (even if it alone exceeds the budget) so we can't get stuck on a page that
// never advances.
static int pack_page(cJSON *root, int count, int page_start) {
    int used = 0;
    int i = page_start;
    while (i < count) {
        cJSON *item = get_entry_newest_first(root, count, i);
        const char *file = entry_filename(item);
        // Measure with the "[X] " prefix already applied (worst case) so
        // packing never under-estimates an already-downloaded game's real
        // rendered height -- the actual label may or may not carry the
        // prefix, but this guarantees we never pack too tight.
        char probe[164];
        snprintf(probe, sizeof(probe), "[X] %s", file ? file : "?");
        int lines = estimate_lines(probe);
        if (i > page_start && used + lines > GAMES_SCREEN_LINES)
            break;
        used += lines;
        i++;
    }
    return i;
}

static rg_gui_event_t open_console_store_cb(rg_gui_option_t *dialog, rg_gui_event_t event) {
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    const char* console = dialog->value;

    // Move URL and Config to Heap
    char *url = malloc(256);
    snprintf(url, 256, "%s/%s/%s.json", RG_STORE_BASE_URL, console, console);

    esp_http_client_config_t *config = calloc(1, sizeof(esp_http_client_config_t));
    config->url = url;
    config->crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(config);
    if (!client) {
        rg_gui_alert("Error", "Could not create HTTP client.");
        free(url);
        free(config);
        return RG_DIALOG_REDRAW;
    }

    char *json_buf = calloc(1, 65536);
    if (!json_buf) {
        rg_gui_alert("Error", "Out of memory.");
        esp_http_client_cleanup(client);
        free(url);
        free(config);
        return RG_DIALOG_REDRAW;
    }

    bool fetch_ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);

        // A 404/500 response (e.g. console has no manifest yet) would
        // otherwise be parsed as if it were valid JSON.
        if (esp_http_client_get_status_code(client) == 200) {
            // esp_http_client_read is not guaranteed to return the full body
            // in one call. Loop until EOF or the buffer is full.
            int total = 0, read_bytes;
            while (total < 65535 &&
                   (read_bytes = esp_http_client_read(client, json_buf + total, 65535 - total)) > 0) {
                total += read_bytes;
            }
            fetch_ok = (total > 0);
        }
    }

    if (!fetch_ok) {
        rg_gui_alert("Network Error", "Could not reach store.");
        free(json_buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(url);
        free(config);
        return RG_DIALOG_REDRAW;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(url);
    free(config);

    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);

    if (!root) {
        rg_gui_alert("Error", "Store is empty or offline.");
        return RG_DIALOG_REDRAW;
    }

    int count = cJSON_GetArraySize(root);
    if (count <= 0) {
        rg_gui_alert("Store", "No games listed for this console yet.");
        cJSON_Delete(root);
        return RG_DIALOG_REDRAW;
    }

    // Precompute every page's start index up front. This makes Left/Right
    // properly circular (wrap from last page back to first and vice versa)
    // instead of only being able to go back through pages already visited.
    int page_starts[64];
    int page_total = 0;
    {
        int p = 0;
        while (p < count && page_total < 64) {
            page_starts[page_total++] = p;
            p = pack_page(root, count, p);
        }
    }
    int page_index = 0;
    bool land_on_last_next = false;

    while (1) {
        int page_start = page_starts[page_index];
        int page_end = pack_page(root, count, page_start);

        rg_gui_option_t *page_menu = calloc((page_end - page_start) + 1, sizeof(rg_gui_option_t));
        download_ctx_t *ctx_array = calloc(page_end - page_start, sizeof(download_ctx_t));

        if (!page_menu || !ctx_array) {
            rg_gui_alert("Error", "Out of memory.");
            free(page_menu);
            free(ctx_array);
            break;
        }

        int n = 0;
        for (int i = page_start; i < page_end; i++) {
            cJSON *item = get_entry_newest_first(root, count, i);
            const char *file = entry_filename(item);
            if (!file)
                continue; // skip malformed entries instead of crashing

            ctx_array[n].console = console;
            ctx_array[n].index = n;
            strncpy(ctx_array[n].filename, file, sizeof(ctx_array[n].filename) - 1);

            // "Already downloaded" indicator: cheap existence check against
            // the same folder our romart patch and the launcher both use.
            char local_path[300];
            snprintf(local_path, sizeof(local_path), "/sd/roms/%s/%s", console, file);
            FILE *test = fopen(local_path, "rb");
            bool have_it = (test != NULL);
            if (test) fclose(test);

            char label[160];
            snprintf(label, sizeof(label), "%s%s", have_it ? "[X] " : "", file);

            page_menu[n].label = strdup(label);
            page_menu[n].value = NULL;
            page_menu[n].flags = RG_DIALOG_FLAG_NORMAL;
            page_menu[n].update_cb = download_game_cb;
            page_menu[n].arg = (intptr_t)&ctx_array[n];
            n++;
        }
        page_menu[n] = (rg_gui_option_t)RG_DIALOG_END;

        char title[64];
        snprintf(title, sizeof(title), "Select Game (%d-%d/%d)", page_start + 1, page_end, count);

        g_page_delta = 0;
        g_land_on_last = false;
        g_last_focus_arg = -1;
        g_page_item_count = n;
        int initial_sel = (land_on_last_next && n > 0) ? n - 1 : 0;

        if (n == 0) {
            rg_gui_alert("Store", "No valid entries on this page.");
        } else {
            rg_gui_dialog(title, page_menu, initial_sel);
        }

        for (int i = 0; i < n; i++)
            free((void*)page_menu[i].label);
        free(page_menu);
        free(ctx_array);

        land_on_last_next = g_land_on_last;

        if (g_page_delta > 0) {
            page_index = (page_index + 1) % page_total;
        } else if (g_page_delta < 0) {
            page_index = (page_index - 1 + page_total) % page_total;
        } else {
            break; // real Cancel/B press
        }
    }

    cJSON_Delete(root);
    return RG_DIALOG_REDRAW;
}

// --- 4. NESTED MENU CALLBACK ---

static rg_gui_event_t open_other_consoles_cb(rg_gui_option_t *dialog, rg_gui_event_t event) {
    if (event == RG_DIALOG_ENTER) {
        // ADDING 'static' FIXES THE NESTED MENU CRASH
        static const rg_gui_option_t other_menu[] = {
            {0, "Game Boy Advance", "gba", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            {0, "Sega Genesis (MD)", "md", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            {0, "MSX", "msx", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            {0, "ColecoVision", "col", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            {0, "Atari Lynx", "lnx", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
			{0, "Pico-8", "fake-08", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
         // {0, "Cannonball", "cannonball", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
         // {0, "Celeste", "celeste", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            {0, "Doom", "doom", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            {0, "Duke Nukem 3D", "duke3d", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            {0, "Quake", "quake", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
            RG_DIALOG_END
        };
        
        rg_gui_dialog("Other Consoles", (rg_gui_option_t*)other_menu, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

void show_rom_store_menu() {
    // ADDING 'static' FREES UP STACK MEMORY
    static const rg_gui_option_t console_menu[] = {
        {0, "Nintendo (NES)", "nes", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
        {0, "Game Boy", "gb", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
        {0, "Game Boy Color", "gbc", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
        {0, "Super Nintendo", "snes", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
        {0, "Sega Master System", "sms", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
        {0, "Game Gear", "gg", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
        {0, "PC Engine", "pce", RG_DIALOG_FLAG_NORMAL, open_console_store_cb},
        {0, "Other Consoles...", NULL, RG_DIALOG_FLAG_NORMAL, open_other_consoles_cb}, 
        RG_DIALOG_END
    };
    
    rg_gui_dialog("Retro Go ROM Store", (rg_gui_option_t*)console_menu, 0);
}
