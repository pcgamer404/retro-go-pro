// FCEUmm NES core (ported from nod3011) + nes_main() core dispatcher.
// Nofrendo lives in main_nes.c as nofrendo_main().
#include "shared.h"

#include "cheat.h"
#include "fceu.h"
#include <fceumm.h>
#include <palettes.h> // nes_palettes[][192], shared with nofrendo
#include <streams/memory_stream.h>

static const void *unused_gui_pal __attribute__((unused)) = gui_pal;

#define NES_WIDTH 256
#define NES_HEIGHT 240
#define NES_PALETTE_TOTAL ((int)(sizeof(nes_palettes) / sizeof(nes_palettes[0])))

static const char *SETTING_PALETTE = "palette";
static const char *SETTING_CORE = "Core"; // old global default; per-ROM choice is stored as "Core<crc>"
static int running_core = 0;             // 0 = FCEUmm, 1 = Nofrendo

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static bool slowFrame = false;

static uint16_t palette565[256];
static int palette_dirty = 0;
static uint32_t fceu_joystick;
static rg_audio_sample_t audio_buf[1024];

extern void nofrendo_main(void);

void GetKeyboard(void) {}

// --- Save states (memstream from libretro-common)
extern void memstream_set_buffer(uint8_t *buffer, uint64_t size);
extern uint64_t memstream_get_last_size(void);

bool FCEUSS_Save_Fs(const char *path)
{
    size_t size = 1024 * 512;
    uint8_t *buffer = malloc(size);
    if (!buffer)
        return false;

    memstream_set_buffer(buffer, size);
    FCEUSS_Save_Mem();
    size_t used = (size_t)memstream_get_last_size();

    bool success = rg_storage_write_file(path, buffer, used, 0);
    free(buffer);
    return success;
}

bool FCEUSS_Load_Fs(const char *path)
{
    void *buffer = NULL;
    size_t size = 0;
    if (!rg_storage_read_file(path, &buffer, &size, 0))
        return false;

    memstream_set_buffer((uint8_t *)buffer, size);
    FCEUSS_Load_Mem();
    free(buffer);
    return true;
}

// --- SRAM
static CartInfo *get_cart_info(void)
{
    if (iNESCart.mapper >= 0 || iNESCart.PRGRomSize > 0)
        return &iNESCart;
    if (UNIFCart.PRGRomSize > 0)
        return &UNIFCart;
    return NULL;
}

static bool load_sram(void)
{
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    if (!path)
        return false;

    void *buffer = NULL;
    size_t size = 0;
    CartInfo *cart = get_cart_info();
    if (!cart || !rg_storage_read_file(path, &buffer, &size, 0))
    {
        free(path);
        return false;
    }

    bool loaded = false;
    for (int i = 0; i < 4; i++)
    {
        if (cart->SaveGame[i] && cart->SaveGameLen[i] > 0 && size >= cart->SaveGameLen[i])
        {
            memcpy(cart->SaveGame[i], buffer, cart->SaveGameLen[i]);
            loaded = true;
            break;
        }
    }

    free(path);
    free(buffer);
    return loaded;
}

static bool save_sram(void)
{
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    CartInfo *cart = get_cart_info();
    if (!path)
        return false;
    if (!cart)
    {
        free(path);
        return false;
    }

    for (int i = 0; i < 4; i++)
    {
        if (cart->SaveGame[i] && cart->SaveGameLen[i] > 0)
        {
            rg_storage_mkdir(rg_dirname(path));
            bool ret = rg_storage_write_file(path, cart->SaveGame[i], cart->SaveGameLen[i], 0);
            free(path);
            return ret;
        }
    }
    free(path);
    return false;
}

// --- FCEU callbacks
void FCEUD_PrintError(char *c) { RG_LOGE("%s\n", c); }
void FCEUD_Message(char *s) {}
void FCEUD_DispMessage(enum retro_log_level level, unsigned duration, const char *str) {}

void FCEUD_SetPalette(uint8 index, uint8 r, uint8 g, uint8 b)
{
    uint16_t color = (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    palette565[index] = (color >> 8) | (color << 8); // big-endian
    palette_dirty = 2;                               // both buffers need the update
}

// Game Genie hardware stubs
void FCEU_OpenGenie(void) {}
void FCEU_CloseGenie(void) {}
void FCEU_GeniePower(void) {}

// --- Handlers
static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
        rg_display_clear(C_BLACK);
    else if (event == RG_EVENT_SHUTDOWN || event == RG_EVENT_SLEEP)
        save_sram();
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    save_sram();
    return FCEUSS_Save_Fs(filename);
}

static bool load_state_handler(const char *filename)
{
    return FCEUSS_Load_Fs(filename);
}

static bool reset_handler(bool hard)
{
    if (hard)
        FCEUI_PowerNES();
    else
        FCEUI_ResetNES();
    return true;
}

// --- Cheats (Game Genie), stored in cheat/<rom>.cht as "NAME|CODE|ON"
static void apply_cheat_code(const char *code, const char *name, int status)
{
    if (!code || strlen(code) < 6)
        return;

    uint16 a_16;
    uint8 v;
    int comp;

    if (!FCEUI_DecodeGG(code, &a_16, &v, &comp))
    {
        RG_LOGE("Invalid Game Genie code: %s\n", code);
        return;
    }

    char full_desc[128];
    snprintf(full_desc, sizeof(full_desc), "%s|%s", name ? name : "Cheat", code);
    FCEUI_AddCheat(full_desc, (uint32)a_16, v, comp, 1);

    int total = 0;
    while (FCEUI_GetCheat(total, NULL, NULL, NULL, NULL, NULL, NULL))
        total++;
    if (total > 0)
        FCEUI_SetCheat(total - 1, NULL, -1, -1, -1, status, 1);
}

static char *cheat_path(void)
{
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    if (!path)
        return NULL;

    char *saves_str = strstr(path, "saves");
    if (saves_str)
        memcpy(saves_str, "cheat", 5);

    char *ext = strrchr(path, '.');
    if (ext)
        strcpy(ext, ".cht");
    return path;
}

static void load_cheats(void)
{
    char *path = cheat_path();
    if (!path)
        return;

    void *buffer = NULL;
    size_t size = 0;
    if (!rg_storage_read_file(path, &buffer, &size, 0))
    {
        free(path);
        return;
    }

    FCEU_ResetCheats();

    char *line = strtok((char *)buffer, "\r\n");
    while (line)
    {
        char *sep1 = strchr(line, '|');
        if (sep1)
        {
            *sep1 = 0;
            char *name = line;
            char *code_part = sep1 + 1;
            int status = 1;

            char *sep2 = strchr(code_part, '|');
            if (sep2)
            {
                *sep2 = 0;
                if (strcmp(sep2 + 1, "OFF") == 0)
                    status = 0;
            }
            apply_cheat_code(code_part, name, status);
        }
        line = strtok(NULL, "\r\n");
    }

    free(buffer);
    free(path);
}

static void save_cheats(void)
{
    char *path = cheat_path();
    if (!path)
        return;

    rg_storage_mkdir(rg_dirname(path));

    const size_t buffer_size = 16384;
    char *buffer = malloc(buffer_size);
    if (!buffer)
    {
        free(path);
        return;
    }
    buffer[0] = 0;
    size_t offset = 0;

    for (int i = 0; i < 64; i++)
    {
        uint32 a;
        uint8 v;
        int s, t, comp;
        char *full_name = NULL;
        if (!FCEUI_GetCheat(i, &full_name, &a, &v, &comp, &s, &t))
            break;

        if (full_name)
        {
            int len = snprintf(buffer + offset, buffer_size - offset, "%s|%s\n", full_name, s ? "ON" : "OFF");
            if (len > 0 && offset + len < buffer_size)
                offset += len;
            else
                break;
        }
    }

    if (offset > 0)
        rg_storage_write_file(path, buffer, offset, 0);
    else
        rg_storage_delete(path);

    free(buffer);
    free(path);
}

// Fills dest[] with the cheat list. Returns the number of entries.
static int build_cheat_list(rg_gui_option_t *choices, char names[][64], char values[][16])
{
    int count = 0;
    for (int i = 0; i < 30; i++)
    {
        uint32 a;
        uint8 v;
        int s, t, comp;
        char *full_name = NULL;
        if (!FCEUI_GetCheat(i, &full_name, &a, &v, &comp, &s, &t))
            break;
        if (!full_name)
            continue;

        char *sep = strchr(full_name, '|');
        size_t len = sep ? RG_MIN((size_t)(sep - full_name), 60) : 63;
        strncpy(names[count], full_name, len);
        names[count][len] = 0;

        choices[count].flags = RG_DIALOG_FLAG_NORMAL;
        choices[count].label = names[count];
        choices[count].value = values ? values[count] : NULL;
        choices[count].arg = (intptr_t)i;
        count++;
    }
    return count;
}

static rg_gui_event_t cheat_toggle_cb(rg_gui_option_t *opt, rg_gui_event_t event)
{
    if (!opt)
        return RG_DIALOG_VOID;

    int index = (int)opt->arg;
    uint32 a;
    uint8 v;
    int s, t, comp;
    char *name = NULL;

    if (event == RG_DIALOG_INIT || event == RG_DIALOG_UPDATE)
    {
        if (opt->value && FCEUI_GetCheat(index, &name, &a, &v, &comp, &s, &t))
            strcpy(opt->value, s ? "ON" : "OFF");
        return RG_DIALOG_VOID;
    }

    if (event != RG_DIALOG_ENTER && event != RG_DIALOG_SELECT)
        return RG_DIALOG_VOID;

    if (FCEUI_GetCheat(index, &name, &a, &v, &comp, &s, &t))
    {
        FCEUI_SetCheat(index, NULL, -1, -1, -1, !s, t);
        save_cheats();
        return RG_DIALOG_UPDATE;
    }
    return RG_DIALOG_VOID;
}

static void handle_cheat_menu(void)
{
    static rg_gui_option_t choices[32];
    static char names[32][64];
    static char values[32][16];

    int count = build_cheat_list(choices, names, values);
    if (count == 0)
    {
        rg_gui_alert("Game Genie", "No codes active. Use 'Load' or 'Add Code'.");
        return;
    }
    for (int i = 0; i < count; i++)
        choices[i].update_cb = cheat_toggle_cb;
    choices[count] = (rg_gui_option_t)RG_DIALOG_END;

    rg_gui_dialog("Game Genie", choices, 0);
}

static void handle_add_cheat_menu(void)
{
    char *code = rg_gui_input_str("Add Game Genie Code", "Enter Code (ABC-DEF)", "");
    if (code)
    {
        char *name = rg_gui_input_str("Add Game Genie Code", "Enter Description", "");
        if (name)
        {
            apply_cheat_code(code, name, 1);
            save_cheats();
            rg_gui_alert("Game Genie", "Code added successfully.");
            free(name);
        }
        free(code);
    }
}

static void handle_delete_cheat_menu(void)
{
    static rg_gui_option_t choices[32];
    static char names[32][64];

    while (true)
    {
        int count = build_cheat_list(choices, names, NULL);
        if (count == 0)
        {
            rg_gui_alert("Delete Code", "No codes to delete.");
            break;
        }
        choices[count] = (rg_gui_option_t)RG_DIALOG_END;

        intptr_t sel_arg = rg_gui_dialog("Delete Code", choices, 0);
        if (sel_arg == RG_DIALOG_CANCELLED)
            break;
        if (sel_arg >= 0 && sel_arg < 30)
        {
            FCEUI_DelCheat((uint32)sel_arg);
            save_cheats();
        }
    }
}

static rg_gui_event_t cheat_list_cb(rg_gui_option_t *opt, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        handle_cheat_menu();
    return RG_DIALOG_VOID;
}

static rg_gui_event_t cheat_add_cb(rg_gui_option_t *opt, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        handle_add_cheat_menu();
    return RG_DIALOG_VOID;
}

static rg_gui_event_t cheat_delete_cb(rg_gui_option_t *opt, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        handle_delete_cheat_menu();
    return RG_DIALOG_VOID;
}

static rg_gui_event_t cheat_load_cb(rg_gui_option_t *opt, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        load_cheats();
        rg_gui_alert("Game Genie", "Codes loaded from SD Card.");
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t cheat_save_cb(rg_gui_option_t *opt, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        save_cheats();
        rg_gui_alert("Game Genie", "Codes saved to SD Card.");
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t cheat_menu_cb(rg_gui_option_t *opt, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t choices[] = {
            {0, "Active Codes", ">", RG_DIALOG_FLAG_NORMAL, &cheat_list_cb},
            {0, "Add Game Genie Code", "-", RG_DIALOG_FLAG_NORMAL, &cheat_add_cb},
            {0, "Delete Code", "-", RG_DIALOG_FLAG_NORMAL, &cheat_delete_cb},
            {0, "Load from SD", "-", RG_DIALOG_FLAG_NORMAL, &cheat_load_cb},
            {0, "Save to SD", "-", RG_DIALOG_FLAG_NORMAL, &cheat_save_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog("Game Genie", choices, 0);
    }
    return RG_DIALOG_VOID;
}

// --- Options
static void update_palette(int n)
{
    if (n < 0 || n >= NES_PALETTE_TOTAL)
        n = 0;
    FCEUI_SetPaletteArray((uint8_t *)nes_palettes[n]);
    palette_dirty = 2;
}

static rg_gui_event_t palette_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *names[] = {"Nofrendo", "Composite", "NES Classic", "NTSC", "PVM", "Smooth"};
    int pal = rg_settings_get_number(NS_APP, SETTING_PALETTE, 0);
    int max = NES_PALETTE_TOTAL - 1;

    if (pal < 0 || pal > max)
        pal = 0;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (event == RG_DIALOG_PREV)
            pal = pal > 0 ? pal - 1 : max;
        else
            pal = pal < max ? pal + 1 : 0;
        rg_settings_set_number(NS_APP, SETTING_PALETTE, pal);
        update_palette(pal);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, pal < 6 ? names[pal] : "-");
    return RG_DIALOG_VOID;
}

// Shared with main_nes.c (nofrendo): read-only label, the core is chosen when the game starts
rg_gui_event_t nes_core_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    strcpy(option->value, running_core ? "Nofrendo" : "FCEUmm");
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, "Core", "-", RG_DIALOG_FLAG_SKIP, &nes_core_update_cb};
    *dest++ = (rg_gui_option_t){0, "Game Genie", ">", RG_DIALOG_FLAG_NORMAL, &cheat_menu_cb};
    *dest++ = (rg_gui_option_t){0, _("Palette"), "-", RG_DIALOG_FLAG_NORMAL, &palette_update_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

// --- Main
static void submit_audio(const int32_t *samples, int count)
{
    if (!samples || count <= 0)
        return;
    if (count > (int)(sizeof(audio_buf) / sizeof(audio_buf[0])))
        count = sizeof(audio_buf) / sizeof(audio_buf[0]);

    for (int i = 0; i < count; i++)
    {
        int32_t s = samples[i];
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        audio_buf[i].left = audio_buf[i].right = (int16_t)s;
    }
    rg_audio_submit(audio_buf, count);
}

static void fceumm_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .event = &event_handler,
        .screenshot = &screenshot_handler,
        .options = &options_handler,
    };

    app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);

    if (!FCEUI_Initialize())
        RG_PANIC("FCEUI_Initialize failed");

    void *rom_data = NULL;
    size_t rom_size = 0;
    if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, 0))
        rom_data = NULL;
    if (!rom_data)
        RG_PANIC("Failed to load ROM");

    if (!FCEUI_LoadGame(app->romPath, rom_data, rom_size, NULL))
        RG_PANIC("FCEUI_LoadGame failed");

    load_sram();
    load_cheats();

    int slstart, slend;
    int is_pal = FCEUI_GetCurrentVidSystem(&slstart, &slend);
    rg_system_set_tick_rate(is_pal ? 50 : 60);
    app->frameskip = 0;

    extern unsigned normal_scanlines;
    int surface_height = (normal_scanlines > 0 && normal_scanlines <= 312) ? normal_scanlines : NES_HEIGHT;

    // 256 lines: FCEUmm's PPU can write past line 240 and PAL needs more
    updates[0] = rg_surface_create(NES_WIDTH, 256, RG_PIXEL_PAL565_BE, MEM_FAST);
    updates[1] = rg_surface_create(NES_WIDTH, 256, RG_PIXEL_PAL565_BE, MEM_FAST);
    currentUpdate = updates[0];
    XBuf = (uint8_t *)currentUpdate->data;

    extern void FCEUI_DisableSpriteLimitation(int a);
    FCEUI_DisableSpriteLimitation(0);

    FSettings.soundq = 0;
    FSettings.SoundVolume = 100;
    FSettings.TriangleVolume = 256;
    FSettings.SquareVolume[0] = 256;
    FSettings.SquareVolume[1] = 256;
    FSettings.NoiseVolume = 256;
    FSettings.PCMVolume = 256;
    FCEUI_Sound(app->sampleRate);
    FCEUI_SetInput(0, SI_GAMEPAD, &fceu_joystick, 0);

    update_palette(rg_settings_get_number(NS_APP, SETTING_PALETTE, 0));

    if (app->bootFlags & RG_BOOT_RESUME)
        rg_emu_load_state(app->saveSlot);

    uint32_t joystick_old = 0;
    bool menu_cancelled = false;
    bool menu_pressed = false;
    bool turbo_a_toggled = false;
    bool turbo_b_toggled = false;
    int turbo_counter = 0;
    int skipFrames = 0;

    while (true)
    {
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();
        const uint32_t joystick_down = joystick & ~joystick_old;
        const uint32_t joystick_prev = joystick_old;
        uint32_t input_buf = 0;
        joystick_old = joystick;
        turbo_counter++;

        // MENU is a modifier: MENU+A/B = toggle turbo, MENU+UP/DOWN/SELECT = FDS insert/eject/side.
        // A plain tap opens the menu on release.
        if (joystick & RG_KEY_MENU)
        {
            if (joystick_down & RG_KEY_A) turbo_a_toggled = !turbo_a_toggled;
            if (joystick_down & RG_KEY_B) turbo_b_toggled = !turbo_b_toggled;
            if (joystick_down & RG_KEY_UP) FCEUI_FDSInsert(0);
            if (joystick_down & RG_KEY_DOWN) FCEUI_FDSEject();
            if (joystick_down & RG_KEY_SELECT) FCEUI_FDSSelect();

            if (joystick & ~RG_KEY_MENU)
                menu_cancelled = true;
            menu_pressed = true;
        }
        else
        {
            if ((joystick_prev & RG_KEY_MENU) && !menu_cancelled)
            {
                save_sram();
                rg_gui_game_menu();
            }
            menu_cancelled = false;
            menu_pressed = false;
        }

        if (joystick & RG_KEY_OPTION)
        {
            save_sram();
            rg_gui_options_menu();
        }

        if (!menu_pressed)
        {
            if (joystick & RG_KEY_UP)     input_buf |= JOY_UP;
            if (joystick & RG_KEY_DOWN)   input_buf |= JOY_DOWN;
            if (joystick & RG_KEY_LEFT)   input_buf |= JOY_LEFT;
            if (joystick & RG_KEY_RIGHT)  input_buf |= JOY_RIGHT;
            if ((joystick & RG_KEY_A) && (!turbo_a_toggled || (turbo_counter & 4))) input_buf |= JOY_A;
            if ((joystick & RG_KEY_B) && (!turbo_b_toggled || (turbo_counter & 4))) input_buf |= JOY_B;
            if (joystick & RG_KEY_START)  input_buf |= JOY_START;
            if (joystick & RG_KEY_SELECT) input_buf |= JOY_SELECT;
        }
        fceu_joystick = input_buf;

        bool drawFrame = !skipFrames;

        if (drawFrame)
        {
            currentUpdate = updates[currentUpdate == updates[0]];
            if (palette_dirty > 0)
            {
                memcpy(currentUpdate->palette, palette565, 512);
                palette_dirty--;
            }
            currentUpdate->width = NES_WIDTH;
            currentUpdate->height = surface_height;
            currentUpdate->offset = 0;
            XBuf = (uint8_t *)currentUpdate->data;
        }

        uint8_t *gfx = NULL;
        int32_t *sound = NULL;
        int32_t sound_samples = 0;

        FCEUI_Emulate(&gfx, &sound, &sound_samples, drawFrame ? 0 : 1);

        extern unsigned extrascanlines;
        extrascanlines = 0;

        if (drawFrame && gfx)
        {
            slowFrame = rg_display_is_busy();
            rg_display_submit(currentUpdate, 0);
        }

        // Tick before submitting audio/syncing. "busy" excludes the audio wait, which is idle time
        // and must not count as a slow frame.
        const int busy = rg_system_timer() - startTime;
        rg_system_tick(busy);

        // Audio is used to pace emulation
        submit_audio(sound, sound_samples);

        if (skipFrames == 0)
        {
            if (app->frameskip > 0)
                skipFrames = app->frameskip;
            else if (busy > app->frameTime + 1500)
                skipFrames = 1;
            else if (drawFrame && slowFrame)
                skipFrames = 1;
        }
        else if (skipFrames > 0)
        {
            skipFrames--;
        }
    }

    RG_PANIC("FCEUmm died!");
}

// NES entry point. FDS always uses FCEUmm. Otherwise the core is chosen in the launcher's ROM menu
// ("Core" row) and stored per ROM as "Core<crc32 of rom path>" (default: FCEUmm).
void nes_main(void)
{
    rg_app_t *a = rg_system_get_app();
    int core = 0;

    if (strcmp(a->configNs, "fds") != 0)
    {
        char key[16];
        snprintf(key, sizeof(key), "Core%08x", (unsigned)rg_crc32(0, (const uint8_t *)a->romPath, strlen(a->romPath)));
        core = rg_settings_get_number(NS_APP, key, rg_settings_get_number(NS_APP, SETTING_CORE, 0)) ? 1 : 0;
    }

    running_core = core;
    if (core == 0)
        fceumm_main();
    else
        nofrendo_main();
}
