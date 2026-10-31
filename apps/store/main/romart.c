// Cover art download.
//
// All the heavy work (finding the cover, resizing it to 160x168, filling the border with the
// transparent colour) is done by the store website's "ROM Art Manager" page, which saves the
// finished PNG next to the ROM in the store: <store_url>/<console>/<rom name>.png
// The device only has to fetch that small file.
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "rg_system.h"
#include "rg_gui.h"
#include "rg_storage.h"
#include "rg_localization.h"
#include "romart.h"

#define ART_MAX_DOWNLOAD   (512 * 1024)   // finished art is ~10-60 KB; anything bigger is not ours
#define ART_MAX_DIMENSION  512            // sanity limit on the PNG header

const char *romart_result_text(romart_result_t r) {
    switch (r) {
        case ROMART_OK:        return _("Cover art saved.");
        case ROMART_CANCELLED: return "";
        case ROMART_NOT_FOUND: return _("No cover art for this game\nin your store yet.\nAdd it with the ROM Art\nManager page.");
        case ROMART_NETWORK:   return _("Network error while\ndownloading cover art.");
        case ROMART_TOO_BIG:   return _("Cover image is too large.");
        case ROMART_DECODE:    return _("Downloaded file is not\na valid PNG image.");
        case ROMART_NOMEM:     return _("Out of memory.");
        case ROMART_WRITE:     return _("Could not save the cover\nto the SD card.");
    }
    return _("Unknown error.");
}

// ---------------------------------------------------------------------------
// Names and URLs
// ---------------------------------------------------------------------------

// ROM filename -> name without extension ("Zelda II (USA).nes" -> "Zelda II (USA)").
// The website uses the very same rule when it names the art file.
static void art_game_name(const char *rom_filename, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", rom_filename);
    char *dot = strrchr(out, '.');
    if (dot && strlen(dot) <= 5) *dot = 0;
}

static void art_url_append(char *dst, size_t dst_size, const char *s) {
    size_t n = strlen(dst);
    for (; *s && n + 4 < dst_size; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[n++] = c;
        } else {
            n += snprintf(dst + n, dst_size - n, "%%%02X", c);
        }
    }
    dst[n] = 0;
}

void romart_path(const char *console_id, const char *rom_filename, char *out, size_t out_size) {
    char name[224];
    art_game_name(rom_filename, name, sizeof(name));
    snprintf(out, out_size, "/sd/roms/%s/%s.png", console_id, name);
}

bool romart_exists(const char *console_id, const char *rom_filename) {
    char dest[256];
    romart_path(console_id, rom_filename, dest, sizeof(dest));
    return rg_storage_exists(dest);
}

// ---------------------------------------------------------------------------
// HTTP (follows redirects). Logs the path only, never the host.
// ---------------------------------------------------------------------------

static const char *art_path_of(const char *url) {
    const char *path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : NULL;
    return path ? path : "/";
}

static void *art_alloc(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static void *art_realloc(void *ptr, size_t size) {
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : realloc(ptr, size);
}

// Open a GET request and return the connected client after a 200 response.
static romart_result_t art_http_open(const char *url, esp_http_client_handle_t *out, int64_t *content_length) {
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ROMART_NOMEM;

    int status = -1;
    for (int hop = 0; hop < 5; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            RG_LOGE("Art: open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return ROMART_NETWORK;
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (esp_http_client_set_redirection(client) != ESP_OK) break;
            esp_http_client_close(client);
            status = -1;
            continue;
        }
        break;
    }
    RG_LOGI("Art: HTTP %d (%s)", status, art_path_of(url));

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return status == 404 ? ROMART_NOT_FOUND : ROMART_NETWORK;
    }
    *out = client;
    *content_length = esp_http_client_get_content_length(client);
    return ROMART_OK;
}

// Download a whole (small) file into memory
static romart_result_t art_http_get(const char *url, uint8_t **out, size_t *out_len) {
    esp_http_client_handle_t client;
    int64_t content_length;
    romart_result_t result = art_http_open(url, &client, &content_length);
    if (result != ROMART_OK) return result;

    if (content_length > ART_MAX_DOWNLOAD) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ROMART_TOO_BIG;
    }

    size_t cap = content_length > 0 ? (size_t)content_length : 64 * 1024;
    size_t len = 0;
    uint8_t *buf = art_alloc(cap);
    if (!buf) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ROMART_NOMEM;
    }

    while (1) {
        if (len == cap) {
            if (cap >= ART_MAX_DOWNLOAD) {
                if (!esp_http_client_is_complete_data_received(client)) result = ROMART_TOO_BIG;
                break;
            }
            size_t new_cap = cap * 2 > ART_MAX_DOWNLOAD ? ART_MAX_DOWNLOAD : cap * 2;
            uint8_t *nb = art_realloc(buf, new_cap);
            if (!nb) { result = ROMART_NOMEM; break; }
            buf = nb; cap = new_cap;
        }
        int n = esp_http_client_read(client, (char *)buf + len, cap - len);
        if (n < 0) { RG_LOGE("Art: read error"); result = ROMART_NETWORK; break; }
        if (n == 0) break;
        len += n;
    }
    bool complete = esp_http_client_is_complete_data_received(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    esp_http_client_close(client); esp_http_client_cleanup(client);

    bool truncated = (content_length > 0)
        ? (len != (size_t)content_length)
        : (chunked && !complete);
    if (result == ROMART_OK && (len == 0 || truncated)) {
        RG_LOGE("Art: incomplete download (%d of %d bytes)", (int)len, (int)content_length);
        result = ROMART_NETWORK;
    }
    if (result != ROMART_OK) { free(buf); return result; }

    *out = buf;
    *out_len = len;
    return ROMART_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Is this a PNG of a sensible size? (Guards against an HTML error page or a huge picture.)
static romart_result_t art_check_png(const uint8_t *d, size_t len) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (len < 33 || memcmp(d, sig, 8) != 0 || memcmp(d + 12, "IHDR", 4) != 0) return ROMART_DECODE;
    uint32_t w = ((uint32_t)d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
    uint32_t h = ((uint32_t)d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    if (w == 0 || h == 0) return ROMART_DECODE;
    if (w > ART_MAX_DIMENSION || h > ART_MAX_DIMENSION) return ROMART_TOO_BIG;
    return ROMART_OK;
}

romart_result_t romart_download(const char *console_id, const char *rom_filename, const char *store_url) {
    if (!store_url || !store_url[0]) return ROMART_NETWORK;

    char name[224], dest[256], tmp[264], backup[264], rom_path[256];
    art_game_name(rom_filename, name, sizeof(name));
    romart_path(console_id, rom_filename, dest, sizeof(dest));
    snprintf(rom_path, sizeof(rom_path), "/sd/roms/%s/%s", console_id, rom_filename);
    // PICO-8 carts are .png files. Never replace the cart itself with cover art.
    if (strcasecmp(dest, rom_path) == 0) return ROMART_CANCELLED;

    char *url = malloc(1024);
    if (!url) return ROMART_NOMEM;
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest);
    snprintf(backup, sizeof(backup), "%s.bak", dest);

    snprintf(url, 1024, "%s/%s/", store_url, console_id);
    art_url_append(url, 1024, name);
    strncat(url, ".png", 1024 - strlen(url) - 1);

    uint8_t *png = NULL;
    size_t png_len = 0;
    romart_result_t r = art_http_get(url, &png, &png_len);
    free(url);
    if (r != ROMART_OK) return r;

    r = art_check_png(png, png_len);
    if (r == ROMART_OK) {
        // Write next to the target first, so a failed write cannot destroy art that is already there
        FILE *f = fopen(tmp, "wb");
        if (!f) {
            r = ROMART_WRITE;
        } else {
            bool ok = fwrite(png, 1, png_len, f) == png_len;
            ok = (fclose(f) == 0) && ok;
            bool had_old = rg_storage_exists(dest);
            if (ok && had_old) {
                remove(backup);
                ok = rename(dest, backup) == 0;
            }
            if (ok) {
                ok = rename(tmp, dest) == 0;
                if (!ok && had_old) rename(backup, dest);
                if (ok && had_old) remove(backup);
            }
            if (!ok) { remove(tmp); r = ROMART_WRITE; }
        }
        if (r == ROMART_OK) RG_LOGI("Art: saved %d bytes", (int)png_len);
    }
    free(png);
    return r;
}

romart_result_t romart_offer(const char *console_id, const char *rom_filename, const char *store_url) {
    char dest[256], rom_path[256];
    romart_path(console_id, rom_filename, dest, sizeof(dest));
    snprintf(rom_path, sizeof(rom_path), "/sd/roms/%s/%s", console_id, rom_filename);
    // A .png ROM (PICO-8) has no separate cover-art path; skip instead of overwriting it.
    if (strcasecmp(dest, rom_path) == 0) return ROMART_CANCELLED;

    bool have = romart_exists(console_id, rom_filename);
    bool yes = have
        ? rg_gui_confirm(_("Cover Art"), _("Cover art is already present.\nDownload a new one?"), false)
        : rg_gui_confirm(_("Cover Art"), _("Download cover art for this game?"), true);
    if (!yes) return ROMART_CANCELLED;

    rg_gui_draw_message(_("Downloading cover art..."));
    rg_display_force_redraw();
    romart_result_t r = romart_download(console_id, rom_filename, store_url);
    if (r != ROMART_OK) rg_gui_alert(_("Cover Art"), romart_result_text(r));
    return r;
}
