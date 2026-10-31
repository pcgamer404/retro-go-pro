#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "rg_system.h"
#include "rg_gui.h"
#include "rg_storage.h"
#include "rg_localization.h"
#include "romart.h"

#define TAG "ROM_STORE"
#define DOWNLOAD_BUFFER_SIZE 4096
#define CATALOG_INITIAL_SIZE (64 * 1024)
#define CATALOG_MAX_SIZE     (8 * 1024 * 1024)

// ---------------------------------------------------------
// EMBEDDED CONSOLE IMAGES
// ---------------------------------------------------------
#include "store_images.h"

typedef struct {
    const char *name;             // Full display name
    const char *short_name;       // Name shown in the carousel/list header
    const char *id;               // Store folder and ROM folder
    const char *play_app;         // OTA partition that can launch this system, or NULL
    bool launcher_supported;      // Has a tab in this launcher build
    const uint8_t *img_data;      // NULL when we only have stylized text
    size_t img_len;
} store_console_t;

static const store_console_t store_consoles[] = {
    {"Nintendo Entertainment System", "NES", "nes", "retro-core", true, nes_img_data, nes_img_len},
    {"Game Boy", "Gameboy", "gb", "retro-core", true, gb_img_data, gb_img_len},
    {"Game Boy Color", "Gameboy Color", "gbc", "retro-core", true, gbc_img_data, gbc_img_len},
    {"Game Boy Advance", "Gameboy Advance", "gba", "gbsp", true, gba_img_data, gba_img_len},
    {"Game & Watch", "Game & Watch", "gw", "retro-core", true, gw_img_data, gw_img_len},
    {"Super Nintendo", "Super Nintendo", "snes", "snes-go", false, snes_img_data, snes_img_len},
    {"Sega Game Gear", "Game Gear", "gg", "retro-core", true, gg_img_data, gg_img_len},
    {"Sega Master System", "Master System", "sms", "retro-core", true, sms_img_data, sms_img_len},
    {"Sega Mega Drive", "Mega Drive", "md", "gwenesis", false, md_img_data, md_img_len},
    {"Sega SG-1000", "SG-1000", "sg1", "retro-core", false, sg1_img_data, sg1_img_len},
    {"Neo Geo Pocket", "Neo Geo Pocket", "ngp", "ngp-go", true, ngp_img_data, ngp_img_len},
    {"PICO-8", "PICO-8", "pico8", "pico8", false, pico8_img_data, pico8_img_len},
    {"Atari Lynx", "Atari Lynx", "lnx", "retro-core", true, lnx_img_data, lnx_img_len},
    {"Atari 2600", "Atari 2600", "a26", "stella", false, a26_img_data, a26_img_len},
    {"WonderSwan", "WonderSwan", "ws", "oswan", false, ws_img_data, ws_img_len},
    {"ColecoVision", "ColecoVision", "col", "retro-core", true, col_img_data, col_img_len},
    {"MSX", "MSX", "msx", "fmsx", false, msx_img_data, msx_img_len},
    {"NEC PC Engine", "PC Engine", "pce", "retro-core", true, pce_img_data, pce_img_len},
    {"Commodore 64", "Commodore 64", "c64", "frodo", false, c64_img_data, c64_img_len},
    {"Atari 5200", "Atari 5200", "a52", "a5200", false, NULL, 0},
    {"Atari 7800", "Atari 7800", "a78", "a7800", false, NULL, 0},
    {"J2ME", "J2ME", "jar", "j2me-go", true, NULL, 0},
    {"MAME (Arcade)", "MAME", "arcade", "mame-go", false, NULL, 0},
    {"ScummVM", "ScummVM", "scummvm", "scummvm", false, NULL, 0},
};
#define CONSOLE_COUNT (sizeof(store_consoles) / sizeof(store_consoles[0]))

#define STORE_NS "store"
#define SETTING_URL "url"
#define SETTING_HIDE_UNAVAILABLE "hide_unavailable"
#define SETTING_SORT "sort"
#define STORE_CONFIG_PATH RG_BASE_PATH_CONFIG "/store.json"
#define STORE_URL_PLACEHOLDER "your-store-url.example.com"

typedef enum {
    SORT_NEWEST = 0,
    SORT_AZ,
    SORT_DOWNLOADED,
    SORT_COUNT
} sort_mode_t;

static const char *const sort_names[SORT_COUNT] = {
    "Newest", "A-Z", "Downloaded"
};

typedef struct {
    cJSON *item;
    bool have;
    int index;
} rom_entry_t;

typedef enum {
    DL_OK = 0,
    DL_CANCELLED,
    DL_NETWORK,
    DL_NO_SPACE,
    DL_WRITE,
    DL_EMPTY
} dl_result_t;

static char g_store_url[200];
static int g_last_http_status = 0;
static int g_sort_mode = SORT_NEWEST;

// ---------------------------------------------------------
// STORE URL
// ---------------------------------------------------------
static bool store_url_valid(const char *url) {
    if (!url || !url[0])
        return false;

    const char *host = NULL;
    if (strncmp(url, "http://", 7) == 0)
        host = url + 7;
    else if (strncmp(url, "https://", 8) == 0)
        host = url + 8;
    if (!host || host[0] == '/' || !host[0])
        return false;
    for (const char *p = host; *p; p++) {
        if (isspace((unsigned char)*p))
            return false;
    }
    if (strchr(host, '?') || strchr(host, '#'))
        return false;
    if (strstr(url, STORE_URL_PLACEHOLDER))
        return false;
    return true;
}

static bool load_store_url(void) {
    g_store_url[0] = 0;

    char *url = rg_settings_get_string(STORE_NS, SETTING_URL, NULL);
    if (!url) {
        rg_settings_set_string(STORE_NS, SETTING_URL, "https://" STORE_URL_PLACEHOLDER);
        rg_settings_commit();
        RG_LOGW("store.json has no \"url\" key, template written");
        rg_gui_alert(_("Store not configured"), _("Set your store URL in:\n" STORE_CONFIG_PATH));
        return false;
    }

    if (strlen(url) >= sizeof(g_store_url)) {
        free(url);
        rg_gui_alert(_("Invalid store URL"), _("The store URL is too long."));
        return false;
    }
    snprintf(g_store_url, sizeof(g_store_url), "%s", url);
    free(url);

    size_t len = strlen(g_store_url);
    while (len > 0 && g_store_url[len - 1] == '/')
        g_store_url[--len] = 0;

    if (!store_url_valid(g_store_url)) {
        g_store_url[0] = 0;
        RG_LOGW("Invalid or placeholder store URL");
        rg_gui_alert(_("Invalid store URL"),
            _("\"url\" in " STORE_CONFIG_PATH "\nmust start with http:// or https://\nand must not contain the placeholder."));
        return false;
    }
    RG_LOGI("Store URL loaded from store.json");
    return true;
}

// ---------------------------------------------------------
// NETWORK HELPERS
// ---------------------------------------------------------
static bool store_wifi_ready(void) {
    int state = rg_network_get_info().state;
    if (state == RG_NETWORK_CONNECTED)
        return true;

    RG_LOGW("Wi-Fi not connected (state %d), reconnecting", state);
    rg_gui_draw_message(_("Reconnecting Wi-Fi..."));
    rg_display_force_redraw();

    if (state == RG_NETWORK_DISCONNECTED)
        rg_network_wifi_start();

    for (int i = 0; i < 150 && rg_network_get_info().state != RG_NETWORK_CONNECTED; i++)
        rg_task_delay(100);

    if (rg_network_get_info().state != RG_NETWORK_CONNECTED) {
        rg_gui_alert(_("Network Error"), _("Wi-Fi is not connected."));
        return false;
    }
    return true;
}

// esp_http_client_open() does not follow redirects, so do it by hand.
// Returns the final HTTP status, or -1 if the connection itself failed.
static int store_http_open(esp_http_client_handle_t client, const char *url) {
    const char *path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : NULL;
    if (!path)
        path = "/";

    for (int hop = 0; hop < 5; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            RG_LOGE("HTTP open failed: %s (%s)", esp_err_to_name(err), path);
            return -1;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (esp_http_client_set_redirection(client) != ESP_OK)
                return status;
            esp_http_client_close(client);
            continue;
        }
        RG_LOGI("HTTP %d (%s)", status, path);
        return status;
    }
    RG_LOGE("Too many redirects (%s)", path);
    return -1;
}

// Read a response body into a growable buffer. This replaces the old hard 64 KB
// cap, which silently cut large catalogs and made cJSON fail.
static bool read_response_body(esp_http_client_handle_t client, char **out, size_t *out_len) {
    int64_t content_length = esp_http_client_get_content_length(client);
    size_t cap = CATALOG_INITIAL_SIZE;

    if (content_length > 0) {
        if ((uint64_t)content_length > CATALOG_MAX_SIZE) {
            RG_LOGE("Response too large: %lld bytes", (long long)content_length);
            return false;
        }
        cap = (size_t)content_length + 1;
        if (cap < CATALOG_INITIAL_SIZE)
            cap = CATALOG_INITIAL_SIZE;
    }

    char *buf = malloc(cap);
    if (!buf)
        return false;

    size_t len = 0;
    while (1) {
        if (len + 1 >= cap) {
            if (cap >= CATALOG_MAX_SIZE) {
                RG_LOGE("Response exceeded %u bytes", (unsigned)CATALOG_MAX_SIZE);
                free(buf);
                return false;
            }
            size_t new_cap = cap * 2;
            if (new_cap > CATALOG_MAX_SIZE)
                new_cap = CATALOG_MAX_SIZE;
            char *new_buf = realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                return false;
            }
            buf = new_buf;
            cap = new_cap;
        }

        int n = esp_http_client_read(client, buf + len, cap - len - 1);
        if (n < 0) {
            RG_LOGE("Response read error");
            free(buf);
            return false;
        }
        if (n == 0)
            break;
        len += n;
        if (content_length > 0 && len >= (size_t)content_length)
            break;
    }

    buf[len] = 0;
    bool complete = esp_http_client_is_complete_data_received(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    bool truncated = (content_length > 0)
        ? (len != (size_t)content_length)
        : (chunked && !complete);

    if (len == 0 || truncated) {
        RG_LOGE("Incomplete response (%u bytes, expected %lld)", (unsigned)len, (long long)content_length);
        free(buf);
        return false;
    }

    RG_LOGI("Response body: %u bytes", (unsigned)len);
    *out = buf;
    *out_len = len;
    return true;
}

// ---------------------------------------------------------
// FORMATTING HELPERS
// ---------------------------------------------------------
static void format_size(int64_t bytes, char *out, size_t out_size) {
    if (bytes < 0) {
        snprintf(out, out_size, "?");
    } else if (bytes < 1024) {
        snprintf(out, out_size, "%lld B", (long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, out_size, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        snprintf(out, out_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_size, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static void format_eta(int64_t seconds, char *out, size_t out_size) {
    if (seconds < 0) {
        snprintf(out, out_size, "?");
    } else if (seconds >= 3600) {
        snprintf(out, out_size, "%lld:%02lld:%02lld",
            (long long)(seconds / 3600), (long long)((seconds / 60) % 60), (long long)(seconds % 60));
    } else {
        snprintf(out, out_size, "%02lld:%02lld",
            (long long)(seconds / 60), (long long)(seconds % 60));
    }
}

static void fit_text(const char *src, char *out, size_t out_size) {
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (len < out_size) {
        snprintf(out, out_size, "%s", src);
    } else if (out_size > 4) {
        snprintf(out, out_size, "%.*s...", (int)(out_size - 4), src);
    } else {
        out[0] = 0;
    }
}

// ---------------------------------------------------------
// DOWNLOAD ENGINE
// ---------------------------------------------------------
static void download_draw_progress(const char *name, int64_t done, int64_t total, double bytes_per_sec) {
    char short_name[42];
    char size_text[96];
    char speed_text[64];
    char eta_text[32];
    char line[160];

    fit_text(name, short_name, sizeof(short_name));

    rg_display_clear(C_BLACK);
    rg_gui_draw_text(10, 10, 300, _("Downloading..."), C_LIGHT_GRAY, C_BLACK, 0);
    rg_gui_draw_text(10, 34, 300, short_name, C_WHITE, C_BLACK, 0);

    int pct = 0;
    if (total > 0) {
        pct = (int)((done * 100) / total);
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
    }

    rg_gui_draw_rect(20, 100, 280, 18, 2, C_WHITE, C_BLACK);
    int fill = (276 * pct) / 100;
    if (fill > 0)
        rg_gui_draw_rect(22, 102, fill, 14, 0, C_NONE, C_GOLD);

    char done_text[32];
    format_size(done, done_text, sizeof(done_text));
    if (total > 0) {
        char total_text[32];
        format_size(total, total_text, sizeof(total_text));
        snprintf(size_text, sizeof(size_text), "%s / %s", done_text, total_text);
        snprintf(line, sizeof(line), "%d%%", pct);
    } else {
        snprintf(size_text, sizeof(size_text), "%s", done_text);
        snprintf(line, sizeof(line), "%s", _("Unknown size"));
    }
    rg_gui_draw_text(20, 130, 280, size_text, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(20, 150, 280, line, C_GOLD, C_BLACK, RG_TEXT_ALIGN_CENTER);

    if (bytes_per_sec > 0) {
        char bps_text[32];
        format_size((int64_t)bytes_per_sec, bps_text, sizeof(bps_text));
        snprintf(speed_text, sizeof(speed_text), "%s/s", bps_text);
    } else {
        snprintf(speed_text, sizeof(speed_text), " ");
    }
    if (total > 0 && bytes_per_sec > 0) {
        int64_t eta = (int64_t)((total - done) / bytes_per_sec);
        if (eta < 0) eta = 0;
        format_eta(eta, eta_text, sizeof(eta_text));
        snprintf(line, sizeof(line), "%s   ETA %s", speed_text, eta_text);
    } else {
        snprintf(line, sizeof(line), "%s", speed_text);
    }
    rg_gui_draw_text(20, 172, 280, line, C_LIGHT_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(10, 220, 300, _("B: Cancel"), C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_display_force_redraw();
}

static dl_result_t download_file(const char *url, const char *sd_path, const char *display_name) {
    g_last_http_status = 0;
    esp_http_client_config_t *config = calloc(1, sizeof(esp_http_client_config_t));
    if (!config)
        return DL_WRITE;

    config->url = url;
    config->crt_bundle_attach = esp_crt_bundle_attach;
    config->timeout_ms = 15000;

    esp_http_client_handle_t client = esp_http_client_init(config);
    if (!client) {
        free(config);
        return DL_NETWORK;
    }

    int status = store_http_open(client, url);
    g_last_http_status = status;
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(config);
        return DL_NETWORK;
    }

    int64_t content_length = esp_http_client_get_content_length(client);
    int64_t free_space = rg_storage_get_free_space("/sd");
    if (free_space >= 0 && content_length > 0 && content_length + 32768 > free_space) {
        RG_LOGW("Not enough space: need %lld, free %lld",
            (long long)content_length, (long long)free_space);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(config);
        return DL_NO_SPACE;
    }

    FILE *f = fopen(sd_path, "wb");
    if (!f) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(config);
        return DL_WRITE;
    }

    char *buffer = malloc(DOWNLOAD_BUFFER_SIZE);
    if (!buffer) {
        fclose(f);
        remove(sd_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(config);
        return DL_WRITE;
    }

    int64_t total_read = 0;
    int64_t start_time = rg_system_timer();
    int64_t last_update = start_time;
    uint32_t prev_pad = rg_input_read_gamepad();
    bool cancelled = false;
    bool write_error = false;
    bool read_error = false;

    download_draw_progress(display_name, 0, content_length, 0);

    while (1) {
        int read_bytes = esp_http_client_read(client, buffer, DOWNLOAD_BUFFER_SIZE);
        if (read_bytes < 0) {
            read_error = true;
            break;
        }
        if (read_bytes == 0)
            break;

        if (fwrite(buffer, 1, read_bytes, f) != (size_t)read_bytes) {
            write_error = true;
            break;
        }

        total_read += read_bytes;

        uint32_t pad = rg_input_read_gamepad();
        if ((pad & RG_KEY_B) && !(prev_pad & RG_KEY_B)) {
            cancelled = true;
            break;
        }
        prev_pad = pad;

        int64_t now = rg_system_timer();
        if (now - last_update >= 100000 || (content_length > 0 && total_read >= content_length)) {
            double elapsed = (double)(now - start_time) / 1000000.0;
            double bytes_per_sec = elapsed > 0 ? (double)total_read / elapsed : 0;
            download_draw_progress(display_name, total_read, content_length, bytes_per_sec);
            last_update = now;
        }
    }

    bool complete = esp_http_client_is_complete_data_received(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    bool close_error = (fclose(f) != 0);

    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(config);

    if (cancelled) {
        remove(sd_path);
        RG_LOGI("Download cancelled by user");
        return DL_CANCELLED;
    }

    bool truncated = (content_length > 0)
        ? (total_read != content_length)
        : (chunked && !complete);

    if (write_error || close_error)
        return DL_WRITE;
    if (read_error)
        return DL_NETWORK;
    if (total_read <= 0)
        return DL_EMPTY;
    if (truncated) {
        RG_LOGW("Download incomplete (%lld of %lld bytes, complete=%d)",
            (long long)total_read, (long long)content_length, complete);
        return DL_NETWORK;
    }
    return DL_OK;
}

// Write the final file only after the download has completed. This keeps an existing
// ROM intact when Wi-Fi drops halfway through a re-download.
static dl_result_t download_file_atomic(const char *url, const char *sd_path, const char *display_name) {
    char part_path[340];
    char backup_path[340];
    if (snprintf(part_path, sizeof(part_path), "%s.part", sd_path) >= (int)sizeof(part_path) ||
        snprintf(backup_path, sizeof(backup_path), "%s.bak", sd_path) >= (int)sizeof(backup_path))
        return DL_WRITE;

    dl_result_t result = download_file(url, part_path, display_name);
    if (result != DL_OK) {
        remove(part_path);
        return result;
    }

    bool had_old = rg_storage_exists(sd_path);
    if (had_old) {
        remove(backup_path);
        if (rename(sd_path, backup_path) != 0) {
            remove(part_path);
            return DL_WRITE;
        }
    }

    if (rename(part_path, sd_path) != 0) {
        if (had_old)
            rename(backup_path, sd_path);
        remove(part_path);
        return DL_WRITE;
    }

    if (had_old)
        remove(backup_path);
    return DL_OK;
}

static void url_encode(const char *src, char *dst, size_t dst_size) {
    size_t n = 0;
    for (; *src && n + 4 < dst_size; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[n++] = c;
        } else {
            n += snprintf(dst + n, dst_size - n, "%%%02X", c);
        }
    }
    dst[n] = 0;
}

static void rom_path_for(const store_console_t *console, const char *filename, char *out, size_t out_size) {
    snprintf(out, out_size, "/sd/roms/%s/%s", console->id, filename);
}

static dl_result_t download_rom(const store_console_t *console, const char *filename, const char *display_name) {
    g_last_http_status = 0;
    if (!store_wifi_ready())
        return DL_NETWORK;

    char url[1024];
    char enc_filename[768];
    char folder[128];
    char sd_path[320];

    url_encode(filename, enc_filename, sizeof(enc_filename));
    snprintf(url, sizeof(url), "%s/%s/%s", g_store_url, console->id, enc_filename);

    snprintf(folder, sizeof(folder), "/sd/roms/%s", console->id);
    rg_storage_mkdir("/sd/roms");
    rg_storage_mkdir(folder);
    rom_path_for(console, filename, sd_path, sizeof(sd_path));

    return download_file_atomic(url, sd_path, display_name);
}

// ---------------------------------------------------------
// CATALOG ENTRIES AND SORTING
// ---------------------------------------------------------
static const char *entry_filename(cJSON *item) {
    cJSON *f_item = item ? cJSON_GetObjectItem(item, "f") : NULL;
    if (!cJSON_IsString(f_item) || !f_item->valuestring || !f_item->valuestring[0])
        return NULL;
    const char *file = f_item->valuestring;
    if (strchr(file, '/') || strchr(file, '\\'))
        return NULL;
    if (strcmp(file, ".") == 0 || strcmp(file, "..") == 0)
        return NULL;
    return file;
}

static const char *entry_display_name(cJSON *item) {
    cJSON *n_item = item ? cJSON_GetObjectItem(item, "n") : NULL;
    if (cJSON_IsString(n_item) && n_item->valuestring && n_item->valuestring[0])
        return n_item->valuestring;
    return entry_filename(item);
}

static int64_t entry_remote_size(cJSON *item) {
    cJSON *s_item = item ? cJSON_GetObjectItem(item, "s") : NULL;
    if (cJSON_IsNumber(s_item))
        return (int64_t)s_item->valuedouble;
    if (cJSON_IsString(s_item) && s_item->valuestring)
        return strtoll(s_item->valuestring, NULL, 10);
    return -1;
}

static bool contains_ci(const char *haystack, const char *needle) {
    if (!needle || !needle[0])
        return true;
    if (!haystack)
        return false;
    size_t needle_len = strlen(needle);
    for (; *haystack; haystack++) {
        size_t i = 0;
        while (i < needle_len && haystack[i] &&
               tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == needle_len)
            return true;
    }
    return false;
}

static bool entry_matches_filter(cJSON *item, const char *filter) {
    if (!filter || !filter[0])
        return true;
    const char *file = entry_filename(item);
    const char *name = entry_display_name(item);
    return contains_ci(file, filter) || contains_ci(name, filter);
}

static int compare_entries(const void *a, const void *b) {
    rom_entry_t *ea = *(rom_entry_t *const *)a;
    rom_entry_t *eb = *(rom_entry_t *const *)b;

    if (g_sort_mode == SORT_DOWNLOADED && ea->have != eb->have)
        return ea->have ? -1 : 1;

    if (g_sort_mode == SORT_AZ) {
        const char *fa = entry_display_name(ea->item);
        const char *fb = entry_display_name(eb->item);
        int cmp = strcasecmp(fa ? fa : "", fb ? fb : "");
        if (cmp)
            return cmp;
    }

    // Newest is the reverse of the catalog order, which is what the old list did.
    return eb->index - ea->index;
}

static void rebuild_view(rom_entry_t *all, int all_count, rom_entry_t **view,
    int *view_count, const char *filter, int sort_mode) {
    int count = 0;
    for (int i = 0; i < all_count; i++) {
        if (entry_matches_filter(all[i].item, filter))
            view[count++] = &all[i];
    }
    g_sort_mode = sort_mode;
    qsort(view, count, sizeof(rom_entry_t *), compare_entries);
    *view_count = count;
}

static rom_entry_t *build_entries(cJSON *root, const store_console_t *console, int *out_count) {
    int count = cJSON_GetArraySize(root);
    rom_entry_t *all = calloc(count, sizeof(rom_entry_t));
    if (!all)
        return NULL;

    int valid = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        const char *file = entry_filename(item);
        if (!file || strlen(file) > 200)
            continue;
        // PICO-8 carts are .png files. For every other system, .png is cover art.
        if (strcmp(console->id, "pico8") != 0 && rg_extension_match(file, "png"))
            continue;

        char path[384];
        rom_path_for(console, file, path, sizeof(path));
        all[valid].item = item;
        all[valid].have = rg_storage_exists(path);
        all[valid].index = i;
        valid++;
    }

    *out_count = valid;
    return all;
}

static void clamp_selection(int *selected, int *scroll, int count) {
    if (count <= 0) {
        *selected = 0;
        *scroll = 0;
        return;
    }
    if (*selected < 0)
        *selected = 0;
    if (*selected >= count)
        *selected = count - 1;
    if (*scroll > *selected)
        *scroll = *selected;
    if (*selected >= *scroll + 9)
        *scroll = *selected - 9 + 1;
    if (*scroll < 0)
        *scroll = 0;
}

static const char *download_result_text(dl_result_t result, char *buffer, size_t buffer_size) {
    switch (result) {
        case DL_OK:
            return _("Game saved to SD Card.");
        case DL_CANCELLED:
            return _("Download cancelled.");
        case DL_NO_SPACE:
            return _("Not enough free space on the SD card.");
        case DL_WRITE:
            return _("Could not write to the SD card.");
        case DL_EMPTY:
            return _("The store returned an empty file.");
        case DL_NETWORK:
            if (g_last_http_status > 0 && g_last_http_status != 200)
                snprintf(buffer, buffer_size, _("Store returned HTTP %d."), g_last_http_status);
            else
                snprintf(buffer, buffer_size, _("Could not reach store.\nCheck Wi-Fi and try again."));
            return buffer;
    }
    return _("Unknown error.");
}

// ---------------------------------------------------------
// ROM LIST DRAWING
// ---------------------------------------------------------
#define ROM_LIST_ROWS       9
#define ROM_LIST_ROW_HEIGHT 20
#define ROM_LIST_TOP        35
#define ROM_LIST_TEXT_CHARS 37

static void rom_list_draw_chrome(const store_console_t *console, const char *filter, int sort_mode) {
    char title[96];
    snprintf(title, sizeof(title), "%s %s", console->short_name, _("Store"));
    rg_gui_draw_text(10, 10, 200, title, C_LIGHT_GRAY, C_BLACK, 0);
    rg_gui_draw_text(10, 220, 300, _("A: Install   B: Back   MENU: Menu"),
        C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);

    if (filter && filter[0]) {
        char filter_text[96];
        snprintf(filter_text, sizeof(filter_text), "%s: %s", _("Filter"), filter);
        rg_gui_draw_text(10, 24, 200, filter_text, C_DIM_GRAY, C_BLACK, 0);
    }

    if (sort_mode < 0 || sort_mode >= SORT_COUNT)
        sort_mode = SORT_NEWEST;
    char sort_text[48];
    snprintf(sort_text, sizeof(sort_text), "%s", _(sort_names[sort_mode]));
    rg_gui_draw_text(215, 10, 95, sort_text, C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_RIGHT);
}

static void rom_list_draw_status(int selected, int total) {
    char status[32];
    if (total > 0)
        snprintf(status, sizeof(status), "%d/%d", selected + 1, total);
    else
        snprintf(status, sizeof(status), "0/0");
    rg_gui_draw_text(220, 24, 90, status, C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_RIGHT);
}

static void rom_list_format_line(char *out, size_t out_size, const char *text, int offset, int width) {
    if (width < 0)
        width = 0;
    if ((size_t)width + 1 > out_size)
        width = (int)out_size - 1;

    int len = (int)strlen(text);
    if (len <= width) {
        memcpy(out, text, len);
        memset(out + len, ' ', width - len);
    } else {
        int span = len + 4;
        offset %= span;
        if (offset < 0)
            offset += span;
        for (int i = 0; i < width; i++) {
            int pos = (offset + i) % span;
            out[i] = pos < len ? text[pos] : ' ';
        }
    }
    out[width] = 0;
}

static void rom_list_draw_row(rom_entry_t *entry, int row, bool selected, int marquee_offset) {
    int y = ROM_LIST_TOP + row * ROM_LIST_ROW_HEIGHT;
    const char *file = entry ? entry_filename(entry->item) : NULL;
    if (!file || !entry) {
        rg_gui_draw_text(10, y, 300, "", C_WHITE, C_BLACK, 0);
        return;
    }

    char full[240];
    char line[ROM_LIST_TEXT_CHARS + 1];
    snprintf(full, sizeof(full), "%s%s", entry->have ? "[X] " : "", file);

    int offset = (selected && (int)strlen(full) > ROM_LIST_TEXT_CHARS) ? marquee_offset : 0;
    rom_list_format_line(line, sizeof(line), full, offset, ROM_LIST_TEXT_CHARS);
    rg_gui_draw_text(10, y, 300, line, selected ? C_GOLD : C_WHITE, C_BLACK, 0);
}

static void rom_list_draw_page(rom_entry_t **view, int count, int scroll, int selected,
    int marquee_offset, const store_console_t *console, const char *filter, int sort_mode) {
    rg_display_clear(C_BLACK);
    rom_list_draw_chrome(console, filter, sort_mode);

    if (count <= 0) {
        rg_gui_draw_text(10, 100, 300, _("No matches."), C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);
        rom_list_draw_status(0, 0);
        rg_display_force_redraw();
        return;
    }

    for (int i = 0; i < ROM_LIST_ROWS; i++) {
        int idx = scroll + i;
        if (idx >= count) {
            rg_gui_draw_text(10, ROM_LIST_TOP + i * ROM_LIST_ROW_HEIGHT, 300,
                "", C_WHITE, C_BLACK, 0);
            continue;
        }
        rom_list_draw_row(view[idx], i, idx == selected,
            idx == selected ? marquee_offset : 0);
    }

    rom_list_draw_status(selected, count);
    rg_display_force_redraw();
}

// ---------------------------------------------------------
// GAME ACTIONS
// ---------------------------------------------------------
static bool edit_filter(char *filter, size_t filter_size) {
    char *text = rg_gui_input_str(_("Search Games"), _("Enter part of a file name:"), filter);
    if (!text)
        return false;
    snprintf(filter, filter_size, "%s", text);
    free(text);
    return true;
}

static bool choose_sort(int *sort_mode) {
    const rg_gui_option_t options[] = {
        {SORT_NEWEST, _("Newest"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {SORT_AZ, _("A-Z"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {SORT_DOWNLOADED, _("Downloaded"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };
    intptr_t selected = rg_gui_dialog(_("Sort Games"), options, *sort_mode);
    if (selected == RG_DIALOG_CANCELLED)
        return false;
    if (selected < 0 || selected >= SORT_COUNT)
        return false;
    *sort_mode = (int)selected;
    rg_settings_set_number(STORE_NS, SETTING_SORT, *sort_mode);
    rg_settings_commit();
    return true;
}

static void fetch_cover_art(const store_console_t *console, rom_entry_t *entry) {
    const char *file = entry_filename(entry->item);
    if (!file || !entry->have) {
        rg_gui_alert(_("Cover Art"), _("Install the game first."));
        return;
    }
    if (strcmp(console->id, "pico8") == 0) {
        rg_gui_alert(_("Cover Art"), _("PICO-8 carts include their own art."));
        return;
    }

    romart_result_t result = romart_offer(console->id, file, g_store_url);
    if (result == ROMART_OK)
        rg_gui_alert(_("Cover Art"), _("Cover art saved."));
}

static void show_entry_info(const store_console_t *console, rom_entry_t *entry) {
    const char *file = entry_filename(entry->item);
    if (!file)
        return;

    char path[320];
    char art_path[320];
    char size_text[32];
    char remote_text[32];
    char art_text[320];
    char message[768];

    rom_path_for(console, file, path, sizeof(path));
    rg_stat_t stat = rg_storage_stat(path);
    format_size(stat.exists ? (int64_t)stat.size : -1, size_text, sizeof(size_text));

    int64_t remote_size = entry_remote_size(entry->item);
    format_size(remote_size, remote_text, sizeof(remote_text));

    bool pico8 = strcmp(console->id, "pico8") == 0;
    romart_path(console->id, file, art_path, sizeof(art_path));
    bool have_art = pico8 || rg_storage_exists(art_path);

    const char *file_name = _("Not installed");
    if (stat.exists)
        file_name = size_text;

    snprintf(art_text, sizeof(art_text), "%s", pico8 ? _("Included") : (have_art ? _("Yes") : _("No")));
    snprintf(message, sizeof(message),
        "%s\n\n%s: %s\n%s: %s\n%s: %s\n%s: %s\n%s: %s\n%s: %s",
        entry_display_name(entry->item),
        _("System"), console->name,
        _("File"), file,
        _("Size"), file_name,
        _("Remote size"), remote_size > 0 ? remote_text : _("Unknown"),
        _("Cover art"), art_text,
        _("Path"), path);

    rg_gui_alert(_("Game Info"), message);
}

static bool delete_entry(const store_console_t *console, rom_entry_t *entry) {
    const char *file = entry_filename(entry->item);
    if (!file || !entry->have)
        return false;

    char message[256];
    snprintf(message, sizeof(message), _("Delete this game and its cover art?\n%s"), file);
    if (!rg_gui_confirm(_("Delete Game"), message, false))
        return false;

    char path[320];
    rom_path_for(console, file, path, sizeof(path));
    bool ok = rg_storage_delete(path);

    if (ok && strcmp(console->id, "pico8") != 0) {
        char art_path[320];
        romart_path(console->id, file, art_path, sizeof(art_path));
        if (rg_storage_exists(art_path))
            rg_storage_delete(art_path);
    }

    if (ok) {
        entry->have = false;
        rg_gui_alert(_("Deleted"), _("Game removed from the SD card."));
    } else {
        rg_gui_alert(_("Delete failed"), _("Could not delete the file."));
    }
    return ok;
}

static bool play_entry(const store_console_t *console, rom_entry_t *entry) {
    if (!entry || !entry->have)
        return false;
    if (!console->play_app || !rg_system_have_app(console->play_app)) {
        rg_gui_alert(_("Cannot play"), _("This system is not installed.\nInstall its app first."));
        return false;
    }

    const char *file = entry_filename(entry->item);
    if (!file)
        return false;

    char path[320];
    rom_path_for(console, file, path, sizeof(path));
    rg_system_switch_app(console->play_app, console->id, path, 0, 0);
    return true;
}

static bool install_entry(const store_console_t *console, rom_entry_t *entry) {
    const char *file = entry_filename(entry->item);
    if (!file)
        return false;

    if (entry->have &&
        !rg_gui_confirm(_("Reinstall game?"), _("Replace the existing file?"), false))
        return false;

    while (1) {
        rg_gui_draw_message(_("Connecting..."));
        rg_display_force_redraw();
        dl_result_t result = download_rom(console, file, file);
        if (result == DL_OK)
            break;
        if (result == DL_CANCELLED)
            return false;

        char error_text[192];
        const char *message = download_result_text(result, error_text, sizeof(error_text));
        if (result == DL_NO_SPACE) {
            rg_gui_alert(_("Download failed"), message);
            return false;
        }
        if (!rg_gui_confirm(_("Download failed"), message, true))
            return false;
    }

    entry->have = true;

    // Keep the existing post-download cover-art offer.
    (void)romart_offer(console->id, file, g_store_url);

    if (console->play_app && rg_system_have_app(console->play_app) &&
        rg_gui_confirm(_("Download complete"), _("Play now?"), true)) {
        char path[320];
        rom_path_for(console, file, path, sizeof(path));
        rg_system_switch_app(console->play_app, console->id, path, 0, 0);
    } else {
        rg_gui_alert(_("Success!"), _("Game saved to SD Card."));
    }
    return true;
}

// ---------------------------------------------------------
// STORE SETTINGS
// ---------------------------------------------------------
static bool edit_store_url(void) {
    char *url = rg_gui_input_str(_("Store URL"),
        _("Enter the store base URL\n(for example https://site/roms):"), g_store_url);
    if (!url)
        return false;

    size_t len = strlen(url);
    while (len > 0 && url[len - 1] == '/')
        url[--len] = 0;

    if (!store_url_valid(url)) {
        rg_gui_alert(_("Invalid store URL"),
            _("The URL must start with http:// or https://\nand must not contain the placeholder."));
        free(url);
        return false;
    }

    rg_settings_set_string(STORE_NS, SETTING_URL, url);
    rg_settings_commit();
    snprintf(g_store_url, sizeof(g_store_url), "%s", url);
    free(url);
    rg_gui_alert(_("Store URL"), _("Store URL saved."));
    return true;
}

static void test_connection(const store_console_t *console) {
    if (!store_url_valid(g_store_url)) {
        rg_gui_alert(_("Test Connection"), _("Set a valid store URL first."));
        return;
    }
    if (!store_wifi_ready())
        return;

    // The store has no index at the base URL; catalogs live at
    // <base>/<console>/<console>.json. Test the active catalog instead of /roms.
    char test_url[512];
    if (console && console->id)
        snprintf(test_url, sizeof(test_url), "%s/%s/%s.json", g_store_url, console->id, console->id);
    else
        snprintf(test_url, sizeof(test_url), "%s", g_store_url);

    esp_http_client_config_t config = {
        .url = test_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        rg_gui_alert(_("Test Connection"), _("Could not create HTTP client."));
        return;
    }

    int status = store_http_open(client, test_url);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    char message[160];
    if (status == 200)
        snprintf(message, sizeof(message), _("Connection OK (HTTP %d)."), status);
    else if (status == 404 && console)
        snprintf(message, sizeof(message), _("No %s catalog found (HTTP 404)."), console->short_name);
    else if (status == 404)
        snprintf(message, sizeof(message), _("Server reached, but no catalog was found (HTTP 404)."));
    else if (status > 0)
        snprintf(message, sizeof(message), _("Connection failed (HTTP %d)."), status);
    else
        snprintf(message, sizeof(message), "%s", _("Could not reach store."));
    rg_gui_alert(_("Test Connection"), message);
}

static void store_settings_dialog(const store_console_t *console) {
    int selected = 0;

    while (1) {
        bool hide = rg_settings_get_boolean(STORE_NS, SETTING_HIDE_UNAVAILABLE, false);
        int sort_mode = (int)rg_settings_get_number(STORE_NS, SETTING_SORT, SORT_NEWEST);
        if (sort_mode < 0 || sort_mode >= SORT_COUNT)
            sort_mode = SORT_NEWEST;

        char url_value[48];
        char hide_value[16];
        char sort_value[48];
        fit_text(g_store_url[0] ? g_store_url : _("Not set"), url_value, sizeof(url_value));
        snprintf(hide_value, sizeof(hide_value), "%s", hide ? _("On") : _("Off"));
        snprintf(sort_value, sizeof(sort_value), "%s", _(sort_names[sort_mode]));

        // Keep the URL out of the narrow value column. Long labels such as
        // "Hide unavailable systems" otherwise leave only a few characters for it.
        const rg_gui_option_t options[] = {
            {0, _("Store URL"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {1, url_value, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {2, _("Test connection"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {3, _("Hide unavailable systems"), hide_value, RG_DIALOG_FLAG_NORMAL, NULL},
            {4, _("Default sort"), sort_value, RG_DIALOG_FLAG_NORMAL, NULL},
            {5, _("System Options"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {6, _("Back"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            RG_DIALOG_END
        };

        intptr_t choice = rg_gui_dialog(_("Store Settings"), options, selected);
        if (choice == RG_DIALOG_CANCELLED || choice == 6)
            break;
        if (choice < 0 || choice > 6 || choice == 1)
            break;

        selected = (int)choice;

        if (choice == 0) {
            edit_store_url();
        } else if (choice == 2) {
            test_connection(console);
        } else if (choice == 3) {
            rg_settings_set_boolean(STORE_NS, SETTING_HIDE_UNAVAILABLE, !hide);
            rg_settings_commit();
        } else if (choice == 4) {
            choose_sort(&sort_mode);
        } else if (choice == 5) {
            rg_gui_options_menu();
        }
    }
}

static void store_menu_dialog(const store_console_t *console) {
    const rg_gui_option_t options[] = {
        {0, _("Resume Browsing"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {1, _("Store Settings"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, _("System Options"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3, _("Exit to Launcher"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };

    intptr_t choice = rg_gui_dialog(_("Store Menu"), options, 0);
    if (choice == 1)
        store_settings_dialog(console);
    else if (choice == 2)
        rg_gui_options_menu();
    else if (choice == 3)
        esp_restart();
}

// ---------------------------------------------------------
// GAME MENU
// ---------------------------------------------------------
enum {
    ACTION_INSTALL = 0,
    ACTION_PLAY,
    ACTION_ART,
    ACTION_INFO,
    ACTION_DELETE,
    ACTION_SEARCH,
    ACTION_CLEAR_FILTER,
    ACTION_SORT,
    ACTION_SETTINGS,
    ACTION_SYSTEM,
    ACTION_EXIT,
    ACTION_RESUME,
};

static void rom_list_actions_menu(const store_console_t *console, rom_entry_t *entry,
    char *filter, size_t filter_size, int *sort_mode, bool *view_changed, bool *reset_selection) {
    rg_gui_option_t options[14];
    int actions[14];
    int count = 0;

    char install_label[32];
    char sort_label[64];

    if (entry) {
        snprintf(install_label, sizeof(install_label), "%s",
            entry->have ? _("Reinstall") : _("Install"));
        options[count] = (rg_gui_option_t){ACTION_INSTALL, install_label, NULL,
            RG_DIALOG_FLAG_NORMAL, NULL};
        actions[count++] = ACTION_INSTALL;

        if (entry->have && console->play_app && rg_system_have_app(console->play_app)) {
            options[count] = (rg_gui_option_t){ACTION_PLAY, _("Play"), NULL,
                RG_DIALOG_FLAG_NORMAL, NULL};
            actions[count++] = ACTION_PLAY;
        }
        if (entry->have && strcmp(console->id, "pico8") != 0) {
            const char *entry_file = entry_filename(entry->item);
            char art_path[320];
            bool have_art = false;
            if (entry_file) {
                romart_path(console->id, entry_file, art_path, sizeof(art_path));
                have_art = rg_storage_exists(art_path);
            }
            if (!have_art) {
                options[count] = (rg_gui_option_t){ACTION_ART, _("Fetch cover art"), NULL,
                    RG_DIALOG_FLAG_NORMAL, NULL};
                actions[count++] = ACTION_ART;
            }
        }

        options[count] = (rg_gui_option_t){ACTION_INFO, _("Info"), NULL,
            RG_DIALOG_FLAG_NORMAL, NULL};
        actions[count++] = ACTION_INFO;

        if (entry->have) {
            options[count] = (rg_gui_option_t){ACTION_DELETE, _("Delete from SD"), NULL,
                RG_DIALOG_FLAG_NORMAL, NULL};
            actions[count++] = ACTION_DELETE;
        }
    }

    options[count] = (rg_gui_option_t){ACTION_SEARCH, _("Search / Filter"), NULL,
        RG_DIALOG_FLAG_NORMAL, NULL};
    actions[count++] = ACTION_SEARCH;

    if (filter[0]) {
        options[count] = (rg_gui_option_t){ACTION_CLEAR_FILTER, _("Clear filter"), NULL,
            RG_DIALOG_FLAG_NORMAL, NULL};
        actions[count++] = ACTION_CLEAR_FILTER;
    }

    snprintf(sort_label, sizeof(sort_label), "%s: %s", _("Sort"), _(sort_names[*sort_mode]));
    options[count] = (rg_gui_option_t){ACTION_SORT, sort_label, NULL,
        RG_DIALOG_FLAG_NORMAL, NULL};
    actions[count++] = ACTION_SORT;

    options[count] = (rg_gui_option_t){ACTION_SETTINGS, _("Store Settings"), NULL,
        RG_DIALOG_FLAG_NORMAL, NULL};
    actions[count++] = ACTION_SETTINGS;

    options[count] = (rg_gui_option_t){ACTION_SYSTEM, _("System Options"), NULL,
        RG_DIALOG_FLAG_NORMAL, NULL};
    actions[count++] = ACTION_SYSTEM;

    options[count] = (rg_gui_option_t){ACTION_EXIT, _("Exit to Launcher"), NULL,
        RG_DIALOG_FLAG_NORMAL, NULL};
    actions[count++] = ACTION_EXIT;

    options[count] = (rg_gui_option_t){ACTION_RESUME, _("Resume Browsing"), NULL,
        RG_DIALOG_FLAG_NORMAL, NULL};
    actions[count++] = ACTION_RESUME;

    options[count] = (rg_gui_option_t)RG_DIALOG_END;

    // rg_gui_dialog returns option->arg, not the option index. Make arg match
    // the array index so the actions[] lookup below is correct even when
    // optional entries (Play, Delete, Clear filter) are omitted.
    for (int i = 0; i < count; i++)
        options[i].arg = i;

    intptr_t choice = rg_gui_dialog(_("Game Menu"), options, 0);
    if (choice == RG_DIALOG_CANCELLED || choice < 0 || choice >= count)
        return;

    switch (actions[choice]) {
        case ACTION_INSTALL:
            if (entry && install_entry(console, entry)) {
                *view_changed = true;
                if (*sort_mode == SORT_DOWNLOADED)
                    *reset_selection = true;
            }
            break;
        case ACTION_PLAY:
            if (entry)
                play_entry(console, entry);
            break;
        case ACTION_ART:
            if (entry)
                fetch_cover_art(console, entry);
            break;
        case ACTION_INFO:
            if (entry)
                show_entry_info(console, entry);
            break;
        case ACTION_DELETE:
            if (entry && delete_entry(console, entry)) {
                *view_changed = true;
                *reset_selection = true;
            }
            break;
        case ACTION_SEARCH:
            if (edit_filter(filter, filter_size)) {
                *view_changed = true;
                *reset_selection = true;
            }
            break;
        case ACTION_CLEAR_FILTER:
            filter[0] = 0;
            *view_changed = true;
            *reset_selection = true;
            break;
        case ACTION_SORT:
            if (choose_sort(sort_mode)) {
                *view_changed = true;
                *reset_selection = true;
            }
            break;
        case ACTION_SETTINGS: {
            store_settings_dialog(console);
            int new_sort = (int)rg_settings_get_number(STORE_NS, SETTING_SORT, SORT_NEWEST);
            if (new_sort < 0 || new_sort >= SORT_COUNT)
                new_sort = SORT_NEWEST;
            if (new_sort != *sort_mode) {
                *sort_mode = new_sort;
                *view_changed = true;
                *reset_selection = true;
            }
            break;
        }
        case ACTION_SYSTEM:
            rg_gui_options_menu();
            break;
        case ACTION_EXIT:
            esp_restart();
            break;
        case ACTION_RESUME:
        default:
            break;
    }
}

// ---------------------------------------------------------
// ROM LIST
// ---------------------------------------------------------
static void open_rom_list(const store_console_t *console) {
    if (!store_wifi_ready())
        return;

    char url[512];
    snprintf(url, sizeof(url), "%s/%s/%s.json", g_store_url, console->id, console->id);

    rg_gui_draw_message(_("Loading catalog..."));
    rg_display_force_redraw();

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        rg_gui_alert(_("Network Error"), _("Could not create HTTP client."));
        return;
    }

    int status = store_http_open(client, url);
    char *json = NULL;
    size_t json_len = 0;
    bool body_ok = false;

    if (status == 200)
        body_ok = read_response_body(client, &json, &json_len);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        char message[96];
        if (status > 0)
            snprintf(message, sizeof(message), _("Store returned HTTP %d."), status);
        else
            snprintf(message, sizeof(message), "%s", _("Could not reach store."));
        rg_gui_alert(_("Network Error"), message);
        return;
    }
    if (!body_ok || !json) {
        free(json);
        rg_gui_alert(_("Store Error"), _("Could not read the store catalog."));
        return;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        rg_gui_alert(_("Store Error"), _("The store catalog is not valid JSON."));
        return;
    }

    int all_count = 0;
    rom_entry_t *all = build_entries(root, console, &all_count);
    if (!all || all_count == 0) {
        free(all);
        cJSON_Delete(root);
        rg_gui_alert(_("No Games"), _("The store has no games for this system."));
        return;
    }

    rom_entry_t **view = malloc((size_t)all_count * sizeof(rom_entry_t *));
    if (!view) {
        free(all);
        cJSON_Delete(root);
        rg_gui_alert(_("Out of Memory"), _("Could not allocate the game list."));
        return;
    }

    int sort_mode = (int)rg_settings_get_number(STORE_NS, SETTING_SORT, SORT_NEWEST);
    if (sort_mode < 0 || sort_mode >= SORT_COUNT)
        sort_mode = SORT_NEWEST;

    char list_url[sizeof(g_store_url)];
    snprintf(list_url, sizeof(list_url), "%s", g_store_url);
    char filter[64] = "";
    int view_count = 0;
    int selected = 0;
    int scroll = 0;
    int marquee_offset = 0;
    int repeat_count = 0;
    int64_t next_repeat = rg_system_timer() + 400000;
    int64_t next_marquee = rg_system_timer() + 250000;
    bool needs_full_redraw = true;
    uint32_t prev_pad = rg_input_read_gamepad();

    rebuild_view(all, all_count, view, &view_count, filter, sort_mode);
    clamp_selection(&selected, &scroll, view_count);

    while (1) {
        uint32_t pad = rg_input_read_gamepad();
        uint32_t pressed = pad & ~prev_pad;

        if (!pressed && (pad & (RG_KEY_UP | RG_KEY_DOWN)) && rg_system_timer() > next_repeat) {
            repeat_count++;
            if (repeat_count > 3)
                pressed = pad & (RG_KEY_UP | RG_KEY_DOWN);
            next_repeat = rg_system_timer() + 150000;
        } else if (pressed) {
            repeat_count = 0;
            next_repeat = rg_system_timer() + 400000;
        }

        if (pressed) {
            bool selection_changed = false;

            if (view_count > 0) {
                if (pressed & RG_KEY_DOWN) {
                    selected = (selected + 1) % view_count;
                    selection_changed = true;
                } else if (pressed & RG_KEY_UP) {
                    selected = (selected + view_count - 1) % view_count;
                    selection_changed = true;
                } else if (pressed & RG_KEY_RIGHT) {
                    selected = (selected + ROM_LIST_ROWS) % view_count;
                    selection_changed = true;
                } else if (pressed & RG_KEY_LEFT) {
                    selected -= ROM_LIST_ROWS;
                    if (selected < 0)
                        selected = 0;
                    selection_changed = true;
                }
            }

            if (pressed & RG_KEY_B)
                break;

            if (pressed & RG_KEY_MENU) {
                bool view_changed = false;
                bool reset_selection = false;
                rom_list_actions_menu(console, view_count > 0 ? view[selected] : NULL,
                    filter, sizeof(filter), &sort_mode, &view_changed, &reset_selection);

                // The settings screen can change the store URL. The catalog in
                // memory belongs to the old URL, so leave it rather than mixing
                // old file names with a new server.
                if (strcmp(list_url, g_store_url) != 0)
                    break;

                if (view_changed)
                    rebuild_view(all, all_count, view, &view_count, filter, sort_mode);
                if (reset_selection) {
                    selected = 0;
                    scroll = 0;
                }
                clamp_selection(&selected, &scroll, view_count);
                marquee_offset = 0;
                next_marquee = rg_system_timer() + 250000;
                needs_full_redraw = true;
                selection_changed = false;
            }

            if (view_count > 0 && (pressed & RG_KEY_A)) {
                rom_entry_t *entry = view[selected];
                if (install_entry(console, entry)) {
                    if (sort_mode == SORT_DOWNLOADED) {
                        rebuild_view(all, all_count, view, &view_count, filter, sort_mode);
                        selected = 0;
                        scroll = 0;
                    }
                    clamp_selection(&selected, &scroll, view_count);
                }
                // Also redraw after cancel / a failed download, so the progress screen
                // never stays on screen.
                needs_full_redraw = true;
                selection_changed = false;
            }

            if (pressed & RG_KEY_SELECT) {
                if (edit_filter(filter, sizeof(filter))) {
                    rebuild_view(all, all_count, view, &view_count, filter, sort_mode);
                    selected = 0;
                    scroll = 0;
                    marquee_offset = 0;
                    needs_full_redraw = true;
                }
                selection_changed = false;
            }

            if (selection_changed) {
                marquee_offset = 0;
                next_marquee = rg_system_timer() + 250000;
                needs_full_redraw = true;
            }
        }

        if (needs_full_redraw) {
            clamp_selection(&selected, &scroll, view_count);
            rom_list_draw_page(view, view_count, scroll, selected, marquee_offset,
                console, filter, sort_mode);
            needs_full_redraw = false;
            next_marquee = rg_system_timer() + 250000;
        }

        if (rg_system_timer() > next_marquee) {
            marquee_offset++;
            next_marquee = rg_system_timer() + 250000;
            if (view_count > 0) {
                const char *file = entry_filename(view[selected]->item);
                int full_len = file ? (int)strlen(file) + (view[selected]->have ? 4 : 0) : 0;
                if (full_len > ROM_LIST_TEXT_CHARS)
                    rom_list_draw_row(view[selected],
                        selected - scroll, true, marquee_offset);
            }
        }

        prev_pad = pad;
        rg_task_delay(15);
    }

    free(view);
    free(all);
    cJSON_Delete(root);
}

// ---------------------------------------------------------
// CONSOLE CAROUSEL
// ---------------------------------------------------------
static bool hide_unavailable_systems(void) {
    return rg_settings_get_boolean(STORE_NS, SETTING_HIDE_UNAVAILABLE, false);
}

static bool console_available(const store_console_t *console) {
    if (!console->launcher_supported)
        return false;
    if (console->play_app && !rg_system_have_app(console->play_app))
        return false;
    return true;
}

static int build_visible_consoles(int *visible, bool hide) {
    int count = 0;
    for (int i = 0; i < (int)CONSOLE_COUNT; i++) {
        if (!hide || console_available(&store_consoles[i]))
            visible[count++] = i;
    }
    return count;
}

static void draw_console_placeholder(const store_console_t *console) {
    rg_gui_draw_rect(40, 55, 240, 125, 2, C_GOLD, C_BLACK);
    rg_gui_draw_rect(44, 59, 232, 117, 1, C_DIM_GRAY, C_BLACK);

    char label[24];
    fit_text(console->short_name, label, sizeof(label));
    rg_gui_draw_text(48, 88, 224, label, C_GOLD, C_BLACK,
        RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
    rg_gui_draw_text(48, 128, 224, _("Store"), C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);
}

static void draw_store_carousel(int selected, int visible_count, const int *visible) {
    rg_display_clear(C_BLACK);
    rg_gui_draw_text(10, 10, 300, _("Store"), C_LIGHT_GRAY, C_BLACK, 0);

    if (visible_count <= 0) {
        rg_gui_draw_text(10, 100, 300, _("No available systems."), C_DIM_GRAY, C_BLACK,
            RG_TEXT_ALIGN_CENTER);
        rg_gui_draw_text(10, 220, 300, _("MENU: Menu"), C_DIM_GRAY, C_BLACK,
            RG_TEXT_ALIGN_CENTER);
        rg_display_force_redraw();
        return;
    }

    const store_console_t *console = &store_consoles[visible[selected]];

    if (console->img_data && console->img_len) {
        rg_image_t *image = rg_surface_load_image(console->img_data, console->img_len, 0);
        if (image) {
            // The embedded cards are 300x150. Keep that footprint between the
            // title and footer instead of stretching them over the whole screen.
            const int art_max_w = 300;
            const int art_max_h = 150;
            const int art_top = 35;
            int art_w = image->width;
            int art_h = image->height;

            if (art_w > 0 && art_h > 0) {
                if (art_w > art_max_w) {
                    art_h = (art_h * art_max_w) / art_w;
                    art_w = art_max_w;
                }
                if (art_h > art_max_h) {
                    art_w = (art_w * art_max_h) / art_h;
                    art_h = art_max_h;
                }
                int art_x = (320 - art_w) / 2;
                int art_y = art_top + (art_max_h - art_h) / 2;
                rg_gui_draw_image(art_x, art_y, art_w, art_h,
                    art_w != image->width || art_h != image->height, image);
            } else {
                draw_console_placeholder(console);
            }
            rg_surface_free(image);
        } else {
            draw_console_placeholder(console);
        }
    } else {
        draw_console_placeholder(console);
    }

    char name[64];
    fit_text(console->short_name, name, sizeof(name));
    rg_gui_draw_text(10, 188, 300, name, C_GOLD, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(10, 220, 300, _("A: Open   B: Back   MENU: Menu"),
        C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);

    char status[32];
    snprintf(status, sizeof(status), "%d/%d", selected + 1, visible_count);
    rg_gui_draw_text(220, 10, 90, status, C_DIM_GRAY, C_BLACK, RG_TEXT_ALIGN_RIGHT);

    rg_display_force_redraw();
}

void show_rom_store_menu(void) {
    if (!load_store_url()) {
        // The config file may be missing or still contain the example URL. Let the
        // user fix it on the device instead of sending them back to the launcher.
        if (!edit_store_url())
            return;
    }

    int visible[CONSOLE_COUNT];
    bool hide = hide_unavailable_systems();
    int visible_count = build_visible_consoles(visible, hide);
    int selected = 0;
    int repeat_count = 0;
    int64_t next_repeat = rg_system_timer() + 400000;
    bool needs_full_redraw = true;
    uint32_t prev_pad = rg_input_read_gamepad();

    while (1) {
        uint32_t pad = rg_input_read_gamepad();
        uint32_t pressed = pad & ~prev_pad;

        if (!pressed && (pad & (RG_KEY_LEFT | RG_KEY_RIGHT)) && rg_system_timer() > next_repeat) {
            repeat_count++;
            if (repeat_count > 3)
                pressed = pad & (RG_KEY_LEFT | RG_KEY_RIGHT);
            next_repeat = rg_system_timer() + 150000;
        } else if (pressed) {
            repeat_count = 0;
            next_repeat = rg_system_timer() + 400000;
        }

        if (pressed) {
            bool selection_changed = false;

            if (visible_count > 0) {
                if (pressed & RG_KEY_RIGHT) {
                    selected = (selected + 1) % visible_count;
                    selection_changed = true;
                } else if (pressed & RG_KEY_LEFT) {
                    selected = (selected + visible_count - 1) % visible_count;
                    selection_changed = true;
                }
            }

            if (pressed & RG_KEY_B)
                break;

            if (pressed & RG_KEY_MENU) {
                store_menu_dialog(visible_count > 0 ? &store_consoles[visible[selected]] : NULL);
                hide = hide_unavailable_systems();
                visible_count = build_visible_consoles(visible, hide);
                if (visible_count <= 0)
                    selected = 0;
                else if (selected >= visible_count)
                    selected = visible_count - 1;
                needs_full_redraw = true;
                selection_changed = false;
            }

            if (visible_count > 0 && (pressed & RG_KEY_A)) {
                open_rom_list(&store_consoles[visible[selected]]);
                hide = hide_unavailable_systems();
                visible_count = build_visible_consoles(visible, hide);
                if (visible_count <= 0)
                    selected = 0;
                else if (selected >= visible_count)
                    selected = visible_count - 1;
                needs_full_redraw = true;
                selection_changed = false;
            }

            if (selection_changed)
                needs_full_redraw = true;
        }

        if (needs_full_redraw) {
            draw_store_carousel(selected, visible_count, visible);
            needs_full_redraw = false;
        }

        prev_pad = pad;
        rg_task_delay(15);
    }
}
