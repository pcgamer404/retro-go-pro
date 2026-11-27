/*
 * spectemu_core.c - Unified Spectemu core implementation for Retro-Go
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#include "config.h"
#include "spectemu.h"
#include <rg_input.h>

// Include core headers
#include "spperif.h"
#include "z80.h"
#include "sptiming.h"
#include "spscr.h"
#include "spkey.h"
#include "sptape.h"
#include "spsound.h"
#include "snapshot.h"
#include "spver.h"
#include "spconf.h"
#include "interf.h"
#include "misc.h"

// Stubs for functions referenced by various spectemu modules
qbyte sp_int_ctr = 0;
int load_immed = 0;
int showframe = 2;
int sp_nosync = 0;
int endofsingle = 0;

void open_snd(void) {}
void setbufsize(void) {}
void keyboard_close(void) {}
void keyboard_getstate(void) {}
void keyboard_init(void) {}
void keyboard_translatekeys(void) {}
void vga_drawscanline(void) {}
void vga_getgraphmem(void) {}
void vga_setmode(void) {}
void vga_setpalvec(void) {}
void spkey_textmode(void) {}
void spkey_screenmode(void) {}

// Include core C sources directly into this single translation unit
#include "rom_imag.c"
#include "loadim.c"
#include "compr.c"
#include "snapshot.c"
#include "spperif.c"
#include "spect.c"
#undef SKIPTIME
#include "spsound.c"
#include "sptape.c"
#include "tapefile.c"
#undef MAXLINELEN
#include "spconf.c"
#include "misc.c"
#include "interf.c"

#include "z80.c"
#include "z80optab.c"
#include "z80_step.c"
#include "z80_op1.c"
#include "z80_op2.c"
#include "z80_op3.c"
#include "z80_op4.c"
#undef LD_INN_RR
#undef LD_RR_INN
#include "z80_op5.c"
#include "z80_op6.c"
#undef CF
#include "spkey.c"
#undef B0
#include "spscr.c"
#include "sptiming.c"

spkeyboard kb_mkey;

void spkb_init(void) {}

void init_spect_scr(void)
{
    int i;
    spscr_init_colors();
    for (i = 0; i < COLORNUM; i++) {
        sp_colors[i] = i + 1;
    }
    spscr_init_line_pointers(240);
}

void init_spect_key(void)
{
    int i;
    for (i = 0; i < NR_SPKEYS; i++) {
        spkb_kbstate[i].press = 0;
    }
    clear_keystates();
    init_basekeys();
}

// Standard ZX Spectrum RGB565 palette
static const uint16_t s_spectrum_palette[16] = {
    // Normal (0..7)
    0x0000, // 0: Black
    0x0017, // 1: Blue    (0, 0, 184)
    0x9800, // 2: Red     (184, 0, 0)
    0x9817, // 3: Magenta (184, 0, 184)
    0x04C0, // 4: Green   (0, 184, 0)
    0x04D7, // 5: Cyan    (0, 184, 184)
    0x9CC0, // 6: Yellow  (184, 184, 0)
    0xCE79, // 7: White   (205, 205, 205)
    // Bright (8..15)
    0x0000, // 8: Bright Black
    0x001F, // 9: Bright Blue    (0, 0, 255)
    0xF800, // 10: Bright Red    (255, 0, 0)
    0xF81F, // 11: Bright Magenta(255, 0, 255)
    0x07E0, // 12: Bright Green  (0, 255, 0)
    0x07FF, // 13: Bright Cyan   (0, 255, 255)
    0xFFE0, // 14: Bright Yellow (255, 255, 0)
    0xFFFF  // 15: Bright White  (255, 255, 255)
};

static int s_frame_tick = 0;
static char s_injected_key = 0;
static int s_injected_frames = 0;

bool spectemu_init(void)
{
    sp_init();
    spti_init();
    init_spect_scr();
    init_spect_key();
    if (load_immed) snsh_z80_load_intern(loadim, loadim_size);
    sp_quick_load = 1;
    s_frame_tick = 0;
    s_injected_key = 0;
    s_injected_frames = 0;
    return true;
}

void spectemu_reset(bool hard)
{
    if (hard) {
        sp_init();
    } else {
        z80_reset();
    }
    s_frame_tick = 0;
    s_injected_key = 0;
    s_injected_frames = 0;
}

bool spectemu_load_file(const char *path)
{
    if (!path || !path[0]) return false;

    const char *dot = strrchr(path, '.');
    if (!dot) return false;

    if (strcasecmp(dot, ".sna") == 0) {
        load_snapshot_file_type((char *)path, SN_SNA);
        return true;
    }
    if (strcasecmp(dot, ".z80") == 0) {
        load_snapshot_file_type((char *)path, SN_Z80);
        return true;
    }
    if (strcasecmp(dot, ".tap") == 0 || strcasecmp(dot, ".tzx") == 0) {
        start_play_file_type((char *)path, 0, -1);
        sp_quick_load = 1;
        return true;
    }

    return false;
}

bool spectemu_save_snapshot(const char *path)
{
    if (!path || !path[0]) return false;
    save_snapshot_file((char *)path);
    return true;
}

void spectemu_run_frame(void)
{
    // Halfframe 1 (Even)
    sp_scline = 0;
    play_tape();
    sp_updating = 1;
    s_frame_tick += CHKTICK;
    s_frame_tick = sp_halfframe(s_frame_tick, EVENHF);
    if (sp_load_trapped) {
        sp_load_trapped = 0;
        z80_proc.haltstate = 0;
        qload();
    }
    z80_interrupt(0xFF);
    sp_int_ctr++;

    // Halfframe 2 (Odd)
    s_frame_tick += CHKTICK;
    s_frame_tick = sp_halfframe(s_frame_tick, ODDHF);
    if (sp_load_trapped) {
        sp_load_trapped = 0;
        z80_proc.haltstate = 0;
        qload();
    }
    z80_interrupt(0xFF);
    sp_int_ctr++;
    rec_tape();

    // Toggle flash attribute every 25 frames (~0.5s)
    if (!(sp_int_ctr % 50)) {
        sp_flash_state = ~sp_flash_state;
    }
}

void spectemu_render_screen(uint16_t *dest, int width, int height)
{
    if (!dest) return;

    const uint8_t *vram = z80_proc.mem + 0x4000;
    const uint8_t *attrs = z80_proc.mem + 0x5800;
    const bool flash_on = (sp_flash_state != 0);

    if (width == 256 && height == 192) {
        // Borderless 256x192 active display (scaled to full screen by Retro-Go display driver)
        uint16_t *pixel_ptr = dest;
        for (int y = 0; y < 192; ++y) {
            const int third = y >> 6;
            const int char_row = (y >> 3) & 7;
            const int y_in_char = y & 7;
            const int bmp_offset = (third << 11) | (y_in_char << 8) | (char_row << 5);
            const int attr_offset = (y >> 3) << 5;

            const uint8_t *bmp_row = vram + bmp_offset;
            const uint8_t *attr_row = attrs + attr_offset;

            for (int col = 0; col < 32; ++col) {
                uint8_t pix = bmp_row[col];
                uint8_t attr = attr_row[col];

                int ink = attr & 0x07;
                int paper = (attr >> 3) & 0x07;
                if (attr & 0x40) {
                    ink += 8;
                    paper += 8;
                }
                if ((attr & 0x80) && flash_on) {
                    int tmp = ink;
                    ink = paper;
                    paper = tmp;
                }

                const uint16_t ink_color = s_spectrum_palette[ink];
                const uint16_t paper_color = s_spectrum_palette[paper];

                pixel_ptr[0] = (pix & 0x80) ? ink_color : paper_color;
                pixel_ptr[1] = (pix & 0x40) ? ink_color : paper_color;
                pixel_ptr[2] = (pix & 0x20) ? ink_color : paper_color;
                pixel_ptr[3] = (pix & 0x10) ? ink_color : paper_color;
                pixel_ptr[4] = (pix & 0x08) ? ink_color : paper_color;
                pixel_ptr[5] = (pix & 0x04) ? ink_color : paper_color;
                pixel_ptr[6] = (pix & 0x02) ? ink_color : paper_color;
                pixel_ptr[7] = (pix & 0x01) ? ink_color : paper_color;
                pixel_ptr += 8;
            }
        }
    } else {
        // 320x240 with classic Sinclair border
        const int border = z80_proc.ula_outport & 0x07;
        const uint16_t border_pixel = s_spectrum_palette[border];

        // Top border: 24 lines x 320 pixels
        for (int i = 0; i < 320 * 24; ++i) {
            dest[i] = border_pixel;
        }

        uint16_t *line_ptr = dest + (320 * 24);

        // 192 active display lines
        for (int y = 0; y < 192; ++y) {
            // Left border: 32 pixels
            for (int b = 0; b < 32; ++b) {
                line_ptr[b] = border_pixel;
            }

            const int third = y >> 6;
            const int char_row = (y >> 3) & 7;
            const int y_in_char = y & 7;
            const int bmp_offset = (third << 11) | (y_in_char << 8) | (char_row << 5);
            const int attr_offset = (y >> 3) << 5;

            const uint8_t *bmp_row = vram + bmp_offset;
            const uint8_t *attr_row = attrs + attr_offset;
            uint16_t *pixel_ptr = line_ptr + 32;

            for (int col = 0; col < 32; ++col) {
                uint8_t pix = bmp_row[col];
                uint8_t attr = attr_row[col];

                int ink = attr & 0x07;
                int paper = (attr >> 3) & 0x07;
                if (attr & 0x40) {
                    ink += 8;
                    paper += 8;
                }
                if ((attr & 0x80) && flash_on) {
                    int tmp = ink;
                    ink = paper;
                    paper = tmp;
                }

                const uint16_t ink_color = s_spectrum_palette[ink];
                const uint16_t paper_color = s_spectrum_palette[paper];

                pixel_ptr[0] = (pix & 0x80) ? ink_color : paper_color;
                pixel_ptr[1] = (pix & 0x40) ? ink_color : paper_color;
                pixel_ptr[2] = (pix & 0x20) ? ink_color : paper_color;
                pixel_ptr[3] = (pix & 0x10) ? ink_color : paper_color;
                pixel_ptr[4] = (pix & 0x08) ? ink_color : paper_color;
                pixel_ptr[5] = (pix & 0x04) ? ink_color : paper_color;
                pixel_ptr[6] = (pix & 0x02) ? ink_color : paper_color;
                pixel_ptr[7] = (pix & 0x01) ? ink_color : paper_color;
                pixel_ptr += 8;
            }

            // Right border: 32 pixels
            for (int b = 0; b < 32; ++b) {
                line_ptr[288 + b] = border_pixel;
            }

            line_ptr += 320;
        }

        // Bottom border: 24 lines x 320 pixels
        for (int i = 0; i < 320 * 24; ++i) {
            line_ptr[i] = border_pixel;
        }
    }
}

int spectemu_get_audio(int16_t *dest_stereo, int max_frames)
{
    if (!dest_stereo || max_frames <= 0) return 0;

    int count = TMNUM; // 312 scanline samples per frame
    if (count > max_frames) count = max_frames;

    for (int i = 0; i < count; i++) {
        // sp_sound_buf is unsigned 8-bit (EAR/MIC bit), convert to 16-bit signed
        int32_t val = (int32_t)sp_sound_buf[i] - 128;
        int16_t sample = (int16_t)(val * 160);
        dest_stereo[i * 2 + 0] = sample;
        dest_stereo[i * 2 + 1] = sample;
    }
    return count;
}

static void zx_press_ascii(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;

    switch (c) {
        case '1': spkey_state[3] |= 0x01; break;
        case '2': spkey_state[3] |= 0x02; break;
        case '3': spkey_state[3] |= 0x04; break;
        case '4': spkey_state[3] |= 0x08; break;
        case '5': spkey_state[3] |= 0x10; break;
        case '6': spkey_state[4] |= 0x10; break;
        case '7': spkey_state[4] |= 0x08; break;
        case '8': spkey_state[4] |= 0x04; break;
        case '9': spkey_state[4] |= 0x02; break;
        case '0': spkey_state[4] |= 0x01; break;

        case 'Q': spkey_state[2] |= 0x01; break;
        case 'W': spkey_state[2] |= 0x02; break;
        case 'E': spkey_state[2] |= 0x04; break;
        case 'R': spkey_state[2] |= 0x08; break;
        case 'T': spkey_state[2] |= 0x10; break;

        case 'Y': spkey_state[5] |= 0x10; break;
        case 'U': spkey_state[5] |= 0x08; break;
        case 'I': spkey_state[5] |= 0x04; break;
        case 'O': spkey_state[5] |= 0x02; break;
        case 'P': spkey_state[5] |= 0x01; break;

        case 'A': spkey_state[1] |= 0x01; break;
        case 'S': spkey_state[1] |= 0x02; break;
        case 'D': spkey_state[1] |= 0x04; break;
        case 'F': spkey_state[1] |= 0x08; break;
        case 'G': spkey_state[1] |= 0x10; break;

        case 'H': spkey_state[6] |= 0x10; break;
        case 'J': spkey_state[6] |= 0x08; break;
        case 'K': spkey_state[6] |= 0x04; break;
        case 'L': spkey_state[6] |= 0x02; break;

        case 'Z': spkey_state[0] |= 0x02; break;
        case 'X': spkey_state[0] |= 0x04; break;
        case 'C': spkey_state[0] |= 0x08; break;
        case 'V': spkey_state[0] |= 0x10; break;

        case 'B': spkey_state[7] |= 0x10; break;
        case 'N': spkey_state[7] |= 0x08; break;
        case 'M': spkey_state[7] |= 0x04; break;

        case '"':
            spkey_state[7] |= 0x02; // Symbol Shift
            spkey_state[5] |= 0x01; // P
            break;
        case ':':
            spkey_state[7] |= 0x02; // Symbol Shift
            spkey_state[0] |= 0x02; // Z
            break;
        case ';':
            spkey_state[7] |= 0x02; // Symbol Shift
            spkey_state[5] |= 0x02; // O
            break;

        case ' ':  spkey_state[7] |= 0x01; break;
        case '\n':
        case '\r': spkey_state[6] |= 0x01; break;
        default: break;
    }
}

static void update_keyboard_matrix(void)
{
    byte statemx[8];
    for (int i = 0; i < 8; i++) {
        byte pv = spkey_state[i];
        byte mm = pv;
        if (pv) {
            byte *km = spkey_state;
            for (int j = 8; j; j--) {
                if ((*km & pv) & 0x1F) mm |= *km;
                km++;
            }
        }
        statemx[i] = mm;
    }

    for (int port = 0; port < 256; port++) {
        byte *km = statemx;
        byte pv = 0;
        byte pb = (byte)port;
        for (int i = 8; i; i--) {
            if (!(pb & 1)) pv |= *km;
            pb >>= 1;
            km++;
        }
        sp_fe_inport_high[port] = (sp_fe_inport_high[port] | 0x1F) & ~(pv & 0x1F);
    }

    byte pv = spmisc_state[0];
    if ((pv & KEMPR) && (pv & KEMPL)) pv &= ~(KEMPR | KEMPL);
    if ((pv & KEMPU) && (pv & KEMPD)) pv &= ~(KEMPD | KEMPU);
    z80_inports[KEMP_PORT] = pv;
    for (int p = 0; p < 256; p++) {
        if (!(p & 0x20)) {
            z80_inports[p] = pv;
        }
    }
}

#define KEY_QUEUE_CAP 64
static char s_key_queue[KEY_QUEUE_CAP];
static int s_key_head = 0;
static int s_key_tail = 0;
static int s_key_hold_frames = 0;
static int s_key_release_frames = 0;

void spectemu_inject_key(char ascii_char)
{
    int next = (s_key_tail + 1) % KEY_QUEUE_CAP;
    if (next != s_key_head) {
        s_key_queue[s_key_tail] = ascii_char;
        s_key_tail = next;
    }
}

void spectemu_inject_string(const char *str)
{
    if (!str) return;
    while (*str) {
        spectemu_inject_key(*str++);
    }
}

void spectemu_set_input(uint32_t buttons, zx_joy_type_t joy_type)
{
    memset(spkey_state, 0, sizeof(spkey_state));
    spmisc_state[0] = 0;

    // Handle injected key queue with proper frame timing
    if (s_key_hold_frames > 0) {
        zx_press_ascii(s_key_queue[s_key_head]);
        s_key_hold_frames--;
        if (s_key_hold_frames == 0) {
            s_key_release_frames = 4; // 4 frames released between keystrokes
            s_key_head = (s_key_head + 1) % KEY_QUEUE_CAP;
        }
    } else if (s_key_release_frames > 0) {
        s_key_release_frames--;
    } else if (s_key_head != s_key_tail) {
        s_key_hold_frames = 6; // Hold each key for 6 video frames (120ms)
        zx_press_ascii(s_key_queue[s_key_head]);
    }

    // Single Enter on START or Y
    if (buttons & (RG_KEY_START | RG_KEY_Y)) {
        zx_press_ascii('\n');
    }
    // Space on X
    if (buttons & RG_KEY_X) {
        zx_press_ascii(' ');
    }
    // L and R for quick title screen option selection (1 / 2)
    if (buttons & RG_KEY_L) {
        zx_press_ascii('1');
    }
    if (buttons & RG_KEY_R) {
        zx_press_ascii('2');
    }

    switch (joy_type) {
        case ZX_JOY_KEMPSTON:
            if (buttons & RG_KEY_RIGHT) spmisc_state[0] |= KEMPR;
            if (buttons & RG_KEY_LEFT)  spmisc_state[0] |= KEMPL;
            if (buttons & RG_KEY_DOWN)  spmisc_state[0] |= KEMPD;
            if (buttons & RG_KEY_UP)    spmisc_state[0] |= KEMPU;
            if (buttons & RG_KEY_A)     spmisc_state[0] |= KEMPF; // Fire
            if (buttons & RG_KEY_B)     spmisc_state[0] |= KEMPU; // Jump/Up
            break;

        case ZX_JOY_SINCLAIR1: // 6=Left, 7=Right, 8=Down, 9=Up, 0=Fire
            if (buttons & RG_KEY_LEFT)  zx_press_ascii('6');
            if (buttons & RG_KEY_RIGHT) zx_press_ascii('7');
            if (buttons & RG_KEY_DOWN)  zx_press_ascii('8');
            if (buttons & RG_KEY_UP)    zx_press_ascii('9');
            if (buttons & (RG_KEY_A | RG_KEY_B)) zx_press_ascii('0');
            break;

        case ZX_JOY_SINCLAIR2: // 1=Left, 2=Right, 4=Down, 3=Up, 5=Fire
            if (buttons & RG_KEY_LEFT)  zx_press_ascii('1');
            if (buttons & RG_KEY_RIGHT) zx_press_ascii('2');
            if (buttons & RG_KEY_DOWN)  zx_press_ascii('4');
            if (buttons & RG_KEY_UP)    zx_press_ascii('3');
            if (buttons & (RG_KEY_A | RG_KEY_B)) zx_press_ascii('5');
            break;

        case ZX_JOY_CURSOR: // 5=Left, 6=Down, 7=Up, 8=Right, 0=Fire
            if (buttons & RG_KEY_LEFT)  zx_press_ascii('5');
            if (buttons & RG_KEY_DOWN)  zx_press_ascii('6');
            if (buttons & RG_KEY_UP)    zx_press_ascii('7');
            if (buttons & RG_KEY_RIGHT) zx_press_ascii('8');
            if (buttons & (RG_KEY_A | RG_KEY_B)) zx_press_ascii('0');
            break;

        case ZX_JOY_QAOP: // O=Left, P=Right, A=Down, Q=Up, Space=Fire
            if (buttons & RG_KEY_LEFT)  zx_press_ascii('O');
            if (buttons & RG_KEY_RIGHT) zx_press_ascii('P');
            if (buttons & RG_KEY_DOWN)  zx_press_ascii('A');
            if (buttons & RG_KEY_UP)    zx_press_ascii('Q');
            if (buttons & (RG_KEY_A | RG_KEY_B)) zx_press_ascii(' ');
            break;

        default:
            break;
    }

    update_keyboard_matrix();
}
