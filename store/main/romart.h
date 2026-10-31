#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ROMART_OK = 0,
    ROMART_CANCELLED,   // user said no to the prompt (no message needed)
    ROMART_NOT_FOUND,   // the store has no art for this game yet
    ROMART_NETWORK,     // connection / HTTP error
    ROMART_TOO_BIG,     // file bigger than the limits below
    ROMART_DECODE,      // downloaded data is not a PNG
    ROMART_NOMEM,
    ROMART_WRITE,       // could not save to the SD card
} romart_result_t;

// Cover art is prepared on the store website (the "ROM Art Manager" page: it finds the cover,
// resizes it to 160x168 and fills the border with transparent magenta FF00FF). The device only
// downloads the finished PNG:
//     <store_url>/<console_id>/<rom name>.png   ->   /sd/roms/<console_id>/<rom name>.png

// Path where the cover art is stored: /sd/roms/<console_id>/<rom name>.png
void romart_path(const char *console_id, const char *rom_filename, char *out, size_t out_size);

// Does /sd/roms/<console_id>/<rom name>.png exist already?
bool romart_exists(const char *console_id, const char *rom_filename);

// Download the art for a ROM saved as /sd/roms/<console_id>/<rom_filename>. No prompts.
// An existing file is only replaced once the new download has been checked, so a failed
// download never destroys the art you already have.
romart_result_t romart_download(const char *console_id, const char *rom_filename, const char *store_url);

// Ask first, then download:
//   no art yet      -> "Download cover art for this game?"
//   art is present  -> "Cover art is already present. Download a new one?"  (default: No)
// Errors are shown to the user. Returns ROMART_CANCELLED if the user declined or
// the ROM is a PICO-8 cart (which has no separate cover file).
romart_result_t romart_offer(const char *console_id, const char *rom_filename, const char *store_url);

const char *romart_result_text(romart_result_t r);
