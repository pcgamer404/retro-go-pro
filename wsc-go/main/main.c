#include <rg_system.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "WSRender.h"
#include "WSApu.h"
#include "oswan_config.h"

extern void WsInit(void);
extern void WsReset(void);
extern int WsRun(bool drawFrame);
extern int WsCreateFromMemory(const uint8_t *rom, size_t len);
extern int WsCreateFromFile(const char *path);
extern const char *WsGetLastError(void);
extern int WsLoadState(const char *filename);
extern int WsSaveState(const char *filename);
extern void AllocateBuffers(void);
extern uint16_t *FrameBuffer;
extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;

// From WS.h / WSHard.h
// Buttons are active low.
// From WS.h / WSHard.h
// The oswan core expects bits to be SET when pressed (Active High internally).
// Nibble 0: Group 0x10 (Y-Pad)
// Nibble 1: Group 0x20 (X-Pad)
// Nibble 2: Group 0x40 (Buttons)
#define WS_Y1 (1 << 0)
#define WS_Y2 (1 << 1)
#define WS_Y3 (1 << 2)
#define WS_Y4 (1 << 3)
#define WS_X1 (1 << 4)
#define WS_X2 (1 << 5)
#define WS_X3 (1 << 6)
#define WS_X4 (1 << 7)
#define WS_SELECT (1 << 8)
#define WS_START (1 << 9)
#define WS_B (1 << 10)
#define WS_A (1 << 11)

#define WIDTH 224
#define HEIGHT 144
#define LEFT_PAD 8
#define RIGHT_PAD 8
#define BUFFER_WIDTH (WIDTH + LEFT_PAD + RIGHT_PAD)

uint32_t lastPadState = 0;

static rg_app_t *app;
// The SPI display can take longer than one emulated frame to copy a surface.
// Keep a ring of surfaces so the CPU never starts writing the surface the
// display task is still reading. Two was not enough (that caused a horizontal
// tear/seam when rendering became faster than the display).
#define UPDATE_BUFFER_COUNT 4
static rg_surface_t *updates[UPDATE_BUFFER_COUNT];
static rg_surface_t *update;
static int currentBufferIndex = 0;
static bool use_y_pad = false;

// Frameskip, owned by this app (rg_system's own auto-frameskip stays off).
//   0 = draw every frame (~75 fps)
//   1 = draw 2 of 3   (~50 fps) <- default, the most the 80 MHz SPI link can take
//   2 = draw 1 of 3   (~25 fps)
static const char *SETTING_FRAMESKIP = "frameskip";
static int frameskip_opt = 1;

int ws_input_poll(int mode) {
  uint32_t joystick = rg_input_read_gamepad();
  uint16_t state = 0x0000; // Active High for oswan core
  static bool combo_pressed = false;
  static bool menuCancelled = false;
  static bool menuPressed = false;

  if (menuPressed && !(joystick & RG_KEY_MENU)) {
    menuPressed = false;
    if (!menuCancelled) {
      rg_task_delay(50);
      app->frameskip = frameskip_opt; // show the level in the dialog status bars
      rg_gui_game_menu();
      app->frameskip = -1; // keep rg_system's auto-frameskip disabled
      return lastPadState;
    }
    menuCancelled = false;
  } else if (joystick & RG_KEY_OPTION) {
    app->frameskip = frameskip_opt;
    rg_gui_options_menu();
    app->frameskip = -1;
    return lastPadState;
  }

  menuPressed = joystick & RG_KEY_MENU;

  if (menuPressed) {
    if (joystick & ~RG_KEY_MENU) {
      menuCancelled = true;
    }
  }

  bool select_pressed = (joystick & RG_KEY_SELECT) != 0;

  // Toggle X-Pad / Y-Pad mode when Select is pressed
  if (select_pressed) {
    if (!combo_pressed) {
      use_y_pad = !use_y_pad;
      combo_pressed = true;
    }
    // Block the Select input from being sent to the emulator
    return lastPadState;
  } else {
    combo_pressed = false;
  }

  if (use_y_pad) {
    // Map D-Pad to Y-Pad
    if (joystick & RG_KEY_UP)
      state |= WS_Y1;
    if (joystick & RG_KEY_RIGHT)
      state |= WS_Y2;
    if (joystick & RG_KEY_DOWN)
      state |= WS_Y3;
    if (joystick & RG_KEY_LEFT)
      state |= WS_Y4;
  } else {
    // Map D-Pad to X-Pad
    if (joystick & RG_KEY_UP)
      state |= WS_X1;
    if (joystick & RG_KEY_RIGHT)
      state |= WS_X2;
    if (joystick & RG_KEY_DOWN)
      state |= WS_X3;
    if (joystick & RG_KEY_LEFT)
      state |= WS_X4;
  }

  // Buttons
  if (joystick & RG_KEY_A)
    state |= WS_B;
  if (joystick & RG_KEY_B)
    state |= WS_A;
  if (joystick & RG_KEY_START)
    state |= WS_START;
  if (joystick & RG_KEY_SELECT)
    state |= WS_SELECT;

  lastPadState = state;
  return state;
}

static rg_gui_event_t frameskip_cb(rg_gui_option_t *option, rg_gui_event_t event) {
  if (event == RG_DIALOG_PREV && frameskip_opt > 0)
    frameskip_opt--;
  else if (event == RG_DIALOG_NEXT && frameskip_opt < 2)
    frameskip_opt++;

  if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
    rg_settings_set_number(NS_APP, SETTING_FRAMESKIP, frameskip_opt);
    app->frameskip = frameskip_opt; // shown in the dialog status bars
  }

  strcpy(option->value, frameskip_opt == 0 ? _("Off") :
                         frameskip_opt == 1 ? _("1 (2/3)") : _("2 (1/3)"));
  return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest) {
  *dest++ = (rg_gui_option_t){0, _("Frameskip"), "-", RG_DIALOG_FLAG_NORMAL, &frameskip_cb};
  *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

// Submit the completed frame to the display task.
static void IRAM_ATTR SubmitFrame(void) {
  update->width = WIDTH;
  update->height = HEIGHT;
  rg_display_submit(update, RG_DISPLAY_WRITE_NOSYNC);
}

#if !OSWAN_NO_AUDIO
// Submit one emulated frame's worth of audio in a single call.
//
// Submitting in several small chunks looked harmless, but the driver's
// no-dongle fallback advances its playback deadline by one chunk per call:
// with N chunks it sleeps (N-1)/N of the frame's audio time *in addition* to
// the time spent emulating. On a heavy WSC game that nearly halved the frame
// rate (Naruto: ~64 fps -> 38 fps). One call per frame lets the emulation
// time count towards the frame period as intended.
static void update_audio(void) {
  int have = apuBufLen();
  if (have <= 0)
    return;
  if (have > SND_RNGSIZE)
    have = SND_RNGSIZE;
  static int16_t samples[SND_RNGSIZE * 2]; // interleaved L/R
  for (int i = 0; i < have; ++i) {
    samples[i * 2] = sndbuffer[0][rBuf];
    samples[i * 2 + 1] = sndbuffer[1][rBuf];
    if (++rBuf >= SND_RNGSIZE)
      rBuf = 0;
  }
  rg_audio_submit((const rg_audio_frame_t *)samples, have);
}
#endif

// Emulated video render tick
void ws_graphics_paint(void) {
  SubmitFrame();
}

static bool reset_handler(bool hard) {
  rg_system_restart();
  return true;
}

static bool screenshot_handler(const char *filename, int width, int height) {
  rg_audio_set_mute(true);
  bool success = rg_surface_save_image_file(update, filename, width, height);
  rg_audio_set_mute(false);
  return success;
}

static bool load_state_handler(const char *filename) {
  return WsLoadState(filename) != 0;
}

static bool save_state_handler(const char *filename) {
  return WsSaveState(filename) != 0;
}

static void event_handler(int event, void *arg) {
  if (event == RG_EVENT_SHUTDOWN || event == RG_EVENT_SLEEP) {
    extern void WsSaveSRAM(void);
    WsSaveSRAM();
  }
}

void app_main(void) {
  const rg_handlers_t handlers = {
      .reset = &reset_handler,
      .screenshot = &screenshot_handler,
      .loadState = &load_state_handler,
      .saveState = &save_state_handler,
      .event = &event_handler,
      .options = &options_handler,
  };

  // The APU emits 4 samples on every scanline (636 per frame); at the
  // WonderSwan's ~75.5 Hz refresh that is ~48 kHz. Submitting at that rate
  // keeps the pitch correct and lets rg_audio_submit pace the emulator
  // (636 samples = ~13.25 ms, one frame).
  const rg_config_t config = {
    .sampleRate = 48000,
    .handlers = handlers,
};
	app = rg_system_init(&config);
	rg_system_set_tick_rate(75);

  // This app owns frameskip. Disable rg_system's auto-frameskip before the
  // splash and ROM load so the monitor task never changes it during boot.
  app->frameskip = -1;

  // The user wants the WonderSwan image to fill the LCD.
  rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);

  // Restore the user's frameskip choice (0 = off, 1 = 2/3, 2 = 1/3).
  frameskip_opt = (int)rg_settings_get_number(NS_APP, SETTING_FRAMESKIP, 1);
  if (frameskip_opt < 0 || frameskip_opt > 2)
    frameskip_opt = 1;

  int margin_offset = LEFT_PAD * sizeof(uint16_t);

  SCREEN_WIDTH = BUFFER_WIDTH;
  SCREEN_HEIGHT = HEIGHT;

  // Allocate the emulator's 64KB work RAM before the display surfaces. It is
  // accessed by every CPU instruction and must land in internal SRAM; if large
  // framebuffers are allocated first, the 64KB block is fragmented and
  // rg_alloc silently falls back to PSRAM (which makes emulation very slow).
  WsInit();

  for (int i = 0; i < UPDATE_BUFFER_COUNT; i++) {
    updates[i] = rg_surface_create(BUFFER_WIDTH, HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
    if (!updates[i])
      RG_PANIC("Failed to allocate WSC framebuffer");
    updates[i]->width = WIDTH;
    updates[i]->height = HEIGHT;
    updates[i]->offset = margin_offset;
  }
  currentBufferIndex = 0;
  update = updates[currentBufferIndex];
  WsSetVidBuf((uint8_t *)update->data + update->offset);

  RG_LOGI("WSC: %dx%d output, %dpx stride, scaling=%d, frameskip=%d",
          WIDTH, HEIGHT, BUFFER_WIDTH, (int)rg_display_get_scaling(),
          frameskip_opt);

  AllocateBuffers();
  extern void WsSplash(void);

  size_t rom_size = 0;
  void *rom_ptr = NULL;
  bool rom_loaded = false;

  if (rg_extension_match(app->romPath, "zip")) {
    if (!rg_storage_unzip_file(app->romPath, NULL, &rom_ptr, &rom_size,
                               RG_FILE_ALIGN_64KB)) {
      RG_LOGE("WSC: unzip failed for %s", app->romPath);
      rg_gui_alert("ROM load failed",
                   "Could not unzip the ROM file.\n"
                   "The archive may be damaged or too large.\n\n"
                   "8 MB ROMs must be unzipped before loading.");
      rg_system_exit();
      return;
    }
    if (WsCreateFromMemory(rom_ptr, rom_size) != 0) {
      rg_gui_alert("Invalid ROM", WsGetLastError());
      rg_system_exit();
      return;
    }
    rom_loaded = true;
  } else {
    rg_stat_t st = rg_storage_stat(app->romPath);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    bool fits_in_ram = (st.size > 0 && st.size + (256 * 1024) <= free_psram);

    if (fits_in_ram &&
        rg_storage_read_file(app->romPath, &rom_ptr, &rom_size,
                             RG_FILE_ALIGN_64KB)) {
      if (WsCreateFromMemory(rom_ptr, rom_size) != 0) {
        rg_gui_alert("Invalid ROM", WsGetLastError());
        rg_system_exit();
        return;
      }
      rom_loaded = true;
    }
  }

  if (!rom_loaded) {
    RG_LOGI("WSC: loading %s with banked SD streaming", app->romPath);
    if (WsCreateFromFile(app->romPath) != 0) {
      rg_gui_alert("ROM load failed", WsGetLastError());
      rg_system_exit();
      return;
    }
  }

  extern void WsLoadSRAM(void);
  WsLoadSRAM();

  if (app->bootFlags & RG_BOOT_RESUME) {
    rg_emu_load_state(app->saveSlot);
  } else {
    extern void WsSplash(void);
    WsSplash();
  }
  
  // This app owns frameskip, so tell rg_system's monitor to leave it alone
  // (-1 disables its auto-frameskip). Every mode still runs game logic and
  // audio at 75 Hz; only the number of drawn frames changes. The 80 MHz SPI
  // link cannot push a full 320x240 RGB565 frame every 13.3 ms, which is why
  // "Off" is available but not the default.
  app->frameskip = -1;
  int drawPhase = 0;

  for (;;) {
    ws_input_poll(0);

    // User-selected draw cadence (0 = all, 1 = 2 of 3, 2 = 1 of 3).
    bool drawFrame;
    if (frameskip_opt == 0)
      drawFrame = true;
    else if (frameskip_opt == 1)
      drawFrame = (drawPhase < 2);
    else
      drawFrame = (drawPhase == 0);
    if (++drawPhase >= 3)
      drawPhase = 0;

    // Switch buffers and set target for next frame. The ring is sized so the
    // display task has finished with a surface before it is reused.
    if (drawFrame) {
      currentBufferIndex = (currentBufferIndex + 1) % UPDATE_BUFFER_COUNT;
      update = updates[currentBufferIndex];
      WsSetVidBuf((uint8_t *)update->data + update->offset);
    }

    int64_t startTime = rg_system_timer();
    WsRun(drawFrame);
    rg_system_tick(rg_system_timer() - startTime);

#if !OSWAN_NO_AUDIO
    // Audio submission provides the pacing (sync)
    update_audio();
#else
    // Audio is compiled out, so rg_system_tick does not throttle. Pace the
    // emulator at the WonderSwan's ~75 Hz refresh ourselves. app->frameTime
    // also follows the user's speed setting.
    static int64_t nextFrame = 0;
    const int64_t framePeriod = app->frameTime > 0 ? app->frameTime : 1000000 / 75;
    if (nextFrame == 0)
      nextFrame = rg_system_timer();
    nextFrame += framePeriod;
    int64_t now = rg_system_timer();
    if (nextFrame > now) {
      rg_usleep(nextFrame - now);
    } else if (now - nextFrame > framePeriod * 2) {
      // A menu or another long stall happened; don't burst to catch up.
      nextFrame = now;
    }
#endif
  }

  rg_system_exit();
}
