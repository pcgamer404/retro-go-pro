#include "shared.h"

#if RG_SCREEN_PIXEL_FORMAT == 0
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_BE
#define FB_PAL(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
#else
#define FB_PIXEL_FORMAT RG_PIXEL_PAL565_LE
#define FB_PAL(c) ((uint16_t)(c))
#endif
#include <stdio.h>
#include <stdlib.h>

// Atari 5200 — the Atari800 core as cut down by MCUME (components/atari5200),
// a52-go. ANTIC hands over one line of 320 palette indices at a time
// (emu_DrawLine), POKEY's sound is pulled per frame at 32 kHz, the console is
// NTSC only (60 frames a second).
//
// The 5200 controller has a stick, two fire buttons and a 12-key keypad with
// START, PAUSE and RESET. On the handheld:
//   D-pad          the stick (full left/right/up/down)
//   A              fire (the lower button)       B   the upper (side) button
//   START          START                         SELECT (alone)  PAUSE
//   X  1    Y  2    L  *    R  #                  (the keys most games use to
//                                                 pick players and levels)
//   SELECT held with:  X 3, Y 4, A 5, B 6, UP 7, RIGHT 8, DOWN 9, LEFT 0,
//                      L RESET, R (nothing)
//   OPTION: retro-go's options, MENU: the game menu.

#include "atari5200.h"
#include "emuapi.h"
#include "pokeysnd.h"

#define A52_WIDTH 320
#define A52_HEIGHT 240
#define A52_FPS 60
#define A52_RATE 32000

unsigned char BIOSData[2048];

static rg_app_t *app;
static rg_surface_t *update;
static uint32_t joystick;
static bool draw_this_frame = true;

/* the pot values the core reads for the stick (atari5200.c's POT_LEFT/RIGHT
 * around POT_CENTRE); the core swaps left and right itself (MCUME's INVX) */
#define POT_CENTRE 115
#define POT_LOW 15
#define POT_HIGH 215

/* ---- the platform calls of emuapi.h */

void emu_printf(char *text) { RG_LOGI("%s", text); }
void emu_printi(int val) { RG_LOGI("%d", val); }
void *emu_Malloc(int size) { return rg_alloc(size, MEM_ANY); }
void emu_Free(void *pt) { free(pt); }
void *emu_TmpMemory(void) { return NULL; }

/* The cartridge: read whole (or out of its zip) by a52_main, then handed to
 * the core's loader byte by byte through these. */
static uint8_t *cart_data;
static size_t cart_size, cart_pos;
int emu_FileOpen(char *filename) { cart_pos = 0; return 1; }
int emu_FileGetc(void) { return cart_pos < cart_size ? cart_data[cart_pos++] : 0; }
int emu_FileSeek(int seek) { cart_pos = seek; return seek; }
void emu_FileClose(void) {}
int emu_FileSize(char *filename) { return (int)cart_size; }

int emu_ReadKeys(void)
{
    const bool shift = (joystick & RG_KEY_SELECT) != 0;
    int keys = 0;
    if (joystick & RG_KEY_RIGHT) keys |= MASK_JOY2_LEFT; // swapped back by the core (INVX)
    if (joystick & RG_KEY_LEFT) keys |= MASK_JOY2_RIGHT;
    if (joystick & RG_KEY_UP) keys |= MASK_JOY2_UP;
    if (joystick & RG_KEY_DOWN) keys |= MASK_JOY2_DOWN;
    if (!shift && (joystick & RG_KEY_A)) keys |= MASK_JOY2_BTN;
    if (!shift && (joystick & RG_KEY_B)) keys |= MASK_KEY_USER2;
    if (joystick & RG_KEY_START) keys |= MASK_KEY_USER1;
    return keys;
}

/* The keypad key held, as the core's index + 1 (0: none). The index is the
 * key's POKEY code / 2 (Atari800's AKEY_5200_*): START 12, PAUSE 8, RESET 4,
 * 0 2, 1 15, 2 14, 3 13, 4 11, 5 10, 6 9, 7 7, 8 6, 9 5, # 1, * 3. */
int emu_GetPad(void)
{
    static uint32_t select_used; // SELECT pressed with another key is not PAUSE
    static bool select_was_down;
    const bool shift = (joystick & RG_KEY_SELECT) != 0;
    int key = -1;
    if (shift)
    {
        if (joystick & RG_KEY_X) key = 13;          // 3
        else if (joystick & RG_KEY_Y) key = 11;     // 4
        else if (joystick & RG_KEY_A) key = 10;     // 5
        else if (joystick & RG_KEY_B) key = 9;      // 6
        else if (joystick & RG_KEY_UP) key = 7;     // 7
        else if (joystick & RG_KEY_RIGHT) key = 6;  // 8
        else if (joystick & RG_KEY_DOWN) key = 5;   // 9
        else if (joystick & RG_KEY_LEFT) key = 2;   // 0
        else if (joystick & RG_KEY_L) key = 4;      // RESET
        if (key >= 0) select_used = 1;
    }
    else
    {
        if (select_was_down && !select_used) key = 8; // SELECT tapped alone: PAUSE
        else if (joystick & RG_KEY_X) key = 15;       // 1
        else if (joystick & RG_KEY_Y) key = 14;       // 2
        else if (joystick & RG_KEY_L) key = 3;        // *
        else if (joystick & RG_KEY_R) key = 1;        // #
        select_used = 0;
    }
    select_was_down = shift;
    return key + 1;
}

int emu_ReadAnalogJoyX(int min, int max)
{
    // the core swaps left and right of this too
    if (joystick & RG_KEY_LEFT) return POT_HIGH;
    if (joystick & RG_KEY_RIGHT) return POT_LOW;
    return POT_CENTRE;
}

int emu_ReadAnalogJoyY(int min, int max)
{
    if (joystick & RG_KEY_UP) return POT_LOW;
    if (joystick & RG_KEY_DOWN) return POT_HIGH;
    return POT_CENTRE;
}

void emu_SetPaletteEntry(unsigned char r, unsigned char g, unsigned char b, int index)
{
    const uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    update->palette[index & 0xFF] = FB_PAL(rgb565);
}

void emu_DrawLine(unsigned char *buf, int width, int height, int line)
{
    if (draw_this_frame && line >= 0 && line < A52_HEIGHT)
        memcpy((uint8_t *)update->data + line * A52_WIDTH, buf, A52_WIDTH);
}

void emu_DrawVsync(void) {}
void emu_sndInit(void) {}
void emu_sndPlaySound(int chan, int volume, int freq) {}

/* The 6502 met an opcode that jams a real one (cpu.c's ENTER_MONITOR): said
 * once per address, with the bytes there, and the emulation goes on. */
extern uint8_t Atari_GetByte(uint16_t addr);
void a52_cim_report(unsigned pc)
{
    static unsigned last_pc = ~0u;
    if (pc == last_pc)
        return;
    last_pc = pc;
    RG_LOGW("6502 jam at $%04X: %02X %02X %02X (the previous byte %02X)", pc & 0xFFFF,
            Atari_GetByte(pc), Atari_GetByte(pc + 1), Atari_GetByte(pc + 2), Atari_GetByte(pc - 1));
}

/* ---- retro-go */

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(update, filename, width, height);
}

static bool reset_handler(bool hard)
{
    at5_Start("");
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
        rg_display_submit(update, 0);
}

void a52_main(void)
{
    const rg_handlers_t handlers = {
        .options = &core_options_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
    };

    app = rg_system_reinit(A52_RATE, &handlers, NULL);
    update = rg_surface_create(A52_WIDTH, A52_HEIGHT, FB_PIXEL_FORMAT, MEM_FAST);

    // The BIOS: the console's own 2 KB, from the card (it is not in the source)
    FILE *bios = fopen(RG_BASE_PATH_BIOS "/5200.rom", "rb");
    const bool bios_ok = bios && fread(BIOSData, 1, sizeof(BIOSData), bios) == sizeof(BIOSData);
    if (bios)
        fclose(bios);
    if (!bios_ok)
    {
        rg_gui_alert(_("BIOS missing"), RG_BASE_PATH_BIOS "/5200.rom");
        rg_system_exit();
    }

    void *rom_data;
    size_t rom_size;
    if (rg_extension_match(app->romPath, "zip"))
    {
        if (!rg_storage_unzip_file(app->romPath, NULL, &rom_data, &rom_size, 0))
            RG_PANIC("ROM file unzipping failed!");
    }
    else if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, 0))
        RG_PANIC("ROM load failed!");
    if (rom_size != 8192 && rom_size != 16384 && rom_size != 32768)
    {
        rg_gui_alert(_("Not supported"), _("Atari 5200 cartridges of 8, 16 or 32 KB only"));
        rg_system_exit();
    }
    cart_data = rom_data;
    cart_size = rom_size;

    at5_Init();
    at5_Start(app->romPath);
    free(rom_data);
    cart_data = NULL;

    rg_system_set_tick_rate(A52_FPS);
    app->frameskip = 1;
    static int16_t mono[A52_RATE / A52_FPS + 2];
    static rg_audio_sample_t stereo[A52_RATE / A52_FPS + 2];
    int sample_frac = 0;
    int skipFrames = 0;

    while (true)
    {
        joystick = rg_input_read_gamepad();

        if (joystick & RG_KEY_MENU)
            rg_gui_game_menu();
        else if (joystick & RG_KEY_OPTION)
            rg_gui_options_menu();

        int64_t startTime = rg_system_timer();
        draw_this_frame = skipFrames == 0;
        bool slowFrame = false;

        at5_Step();

        if (draw_this_frame)
        {
            slowFrame = rg_display_is_busy();
            rg_display_submit(update, 0);
        }

        // 32000 / 60 = 533.33 samples a frame
        sample_frac += A52_RATE % A52_FPS;
        const int samples = A52_RATE / A52_FPS + (sample_frac >= A52_FPS ? 1 : 0);
        if (sample_frac >= A52_FPS)
            sample_frac -= A52_FPS;
        POKEYSND_Process(mono, samples);
        for (int i = 0; i < samples; i++)
            stereo[i].left = stereo[i].right = mono[i];

        rg_system_tick(rg_system_timer() - startTime);
        rg_audio_submit(stereo, samples);

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            if (core_frameskip(app) > 0)
                skipFrames = core_frameskip(app);
            else if (elapsed > app->frameTime + 1500)
                skipFrames = 1;
            else if (draw_this_frame && slowFrame)
                skipFrames = 1;
        }
        else if (skipFrames > 0)
        {
            skipFrames--;
        }
    }
}
