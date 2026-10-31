/* libco and rthreads are no longer needed: MAME's run_game/cpu_run
 * have been refactored into a re-entrant per-frame entry point
 * (mame_run_one_frame, see src/mame.c) that returns up the stack
 * after each rendered frame, so retro_run drives the emulator
 * directly without a coroutine or kernel thread. */

#if (HAS_DRZ80 || HAS_CYCLONE)
#include "frontend_list.h"
#endif
#include "../mamego_prof.h"

#include <stdarg.h>
#include <sys/time.h>
#include <libretro.h>
#include "libretro_core_options.h"
#include "mame.h"
#include "cpuintrf.h"
#include "osdepend.h"
#include "driver.h"
#include "allegro.h"
#include "minimal.h"
#include <file/file_path.h>

#ifndef RETROK_TILDE
#define RETROK_TILDE 178
#endif

#ifdef _WIN32
char slash = '\\';
#else
char slash = '/';
#endif

char *IMAMEBASEPATH = NULL;
char *IMAMESAMPLEPATH = NULL;

/* Path buffer length for the per-game MAME work-directory paths
 * built by joining one of the 1024-byte core_save_directory /
 * core_sys_directory buffers with a short subdirectory suffix
 * ("samples", "nvram", "hi", "cfg", "snap", "memcard", "sta",
 * "artwork", "cheat" -- longest is 7 chars).  Set above 1024 so
 * the compiler can see the snprintf("%s%c%s", core_*_directory,
 * slash, suffix) construction fits without truncation; the older
 * value of 1024 generated nine -Wformat-truncation warnings
 * because the source could itself be up to 1023 chars + nul. */
#define PATH_BUF_SIZE 1280

const char *retro_save_directory;
const char *retro_system_directory;
char       *retro_content_directory;  /* strdup()'d in retro_load_game(); we own it */
char core_save_directory[1024];
char core_sys_directory[1024];

/* Cross-TU yield signal: hook_video_done() (called at the tail of
 * osd_update_video_and_audio() once a frame has been rendered and
 * delivered to gp2x_screen15) sets this to 1, causing cpu_run_step()
 * in src/cpuintrf.c to exit its scheduling loop so retro_run() can
 * resume.  cpu_run_step() clears it on entry. */
int yield_pending = 0;

unsigned frameskip_type                  = 0;
unsigned frameskip_threshold             = 0;
unsigned frameskip_counter               = 0;
unsigned frameskip_interval              = 0;

int retro_audio_buff_active              = false;
unsigned retro_audio_buff_occupancy      = 0;
int retro_audio_buff_underrun            = false;

unsigned retro_audio_latency             = 0;
int update_audio_latency                 = false;

int should_skip_frame                    = 0;

static int sample_rate                   = 22050;
static int stereo_enabled                = true;

int game_index = -1;
unsigned short *gp2x_screen15;
/* Software-framebuffer fast path: when the frontend grants us a buffer of
 * exactly the right size/pitch/format for this frame, point gp2x_screen15
 * at it so blit.c writes the frame straight into frontend memory; the
 * matching video_cb call then becomes a zero-copy "data already there"
 * signal.  When no buffer is granted (or the geometry doesn't match) the
 * core falls back to the internally-allocated buffer and the normal
 * video_cb path. */
static unsigned short *gp2x_screen15_owned;  /* the core's malloc'd buffer */
static size_t          gp2x_screen15_bytes;  /* size of the above in bytes; 0 means unallocated */
static void           *sw_fb_active_data;    /* non-NULL when SW-FB is in use this frame */
static size_t          sw_fb_active_pitch;
extern int gfx_xoffset;
extern int gfx_yoffset;
extern int gfx_width;
extern int gfx_height;
extern const void *mame2000_direct_frame_data;
extern size_t      mame2000_direct_frame_pitch;
extern int usestereo;
extern int samples_per_frame;
extern short *samples_buffer;
extern int joy_pressed[40];
extern int key[KEY_MAX];
extern char *nvdir, *hidir, *cfgdir, *inpdir, *stadir, *memcarddir;
extern char *artworkdir, *screenshotdir, *alternate_name;
extern char *cheatdir;

void decompose_rom_sample_path(char *rompath, char *samplepath);
void init_joy_list(void);

extern uint32_t create_path_recursive(char *path);
	
#if defined(_3DS)
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
#endif

void CLIB_DECL logerror(const char *text,...)
{
#ifdef DISABLE_ERROR_LOGGING
   va_list arg;
   va_start(arg,text);
   vprintf(text,arg);
   va_end(arg);
#endif
}

int global_showinfo = 1;
int emulated_width;
int emulated_height;
int safe_render_path = 1;
unsigned short gp2x_palette[512];
int gp2x_pal_50hz=0;
int global_fps = 1;
int rotate_controls = 0;
int soundcard;
int attenuation = 0;

void gp2x_printf(char* fmt, ...)
{
   va_list marker;
	
   va_start(marker, fmt);
   vprintf(fmt, marker);
   va_end(marker);
}

/* Allocate (or grow) the core-owned framebuffer to fit the game's
 * actual resolution.  Replaces the historical upfront 640x480x2
 * allocation in retro_init() (614,400 bytes regardless of what the
 * game needs) with on-demand sizing driven by MAME's own video-mode
 * setup pathway: src/libretro/video.c's select_display_mode() calls
 * us with the dimensions the driver requested.  Typical horizontal
 * arcade games run at 256x224 (114,688 bytes -- 81% smaller); the
 * few drivers that genuinely need 640x480 (vector games via
 * vector_game + safe_render_path; explicit iOS_fixedRes=3/4) still
 * get exactly that.
 *
 * Grow-only: if a game later increases its visible area or screen_-
 * reinit() re-enters with larger dimensions, we reallocate.  A
 * shrink request is honoured by leaving the larger buffer in place
 * -- cheaper than free/realloc and never wrong (gfx_xoffset/-
 * gfx_yoffset always centre inside gfx_width x gfx_height, and
 * blit.c indexes within that). */
void gp2x_set_video_mode(int bpp,int width,int height)
{
   size_t needed;

   (void)bpp;

   if (width <= 0 || height <= 0)
      return;

   needed = (size_t)width * (size_t)height * 2;
   if (needed <= gp2x_screen15_bytes)
      return;  /* current allocation already large enough */

#ifdef _3DS
   if (gp2x_screen15_owned)
      linearFree(gp2x_screen15_owned);
   gp2x_screen15_owned = (unsigned short *) linearMemAlign(needed, 0x80);
#else
   free(gp2x_screen15_owned);
   gp2x_screen15_owned = (unsigned short *) malloc(needed);
#endif
   gp2x_screen15       = gp2x_screen15_owned;
   gp2x_screen15_bytes = gp2x_screen15_owned ? needed : 0;
}

void gp2x_video_setpalette(void)
{
}

int osd_init(void)
{
   return 0;
}

void osd_exit(void)
{
}

int screen_reinit(void)
{
   return 1;
}

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
/* Non-static so src/libretro/sound.c's osd_update_silent_stream() can
 * dispatch a frame of silence directly when pause_action is set, the
 * way mame2003-libretro's osd_update_silent_stream calls audio_batch_-
 * cb itself from inside updatescreen().  retro_run()'s tail dispatch
 * is gated on pause_action so we never double-deliver. */
retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static bool libretro_supports_bitmasks = false;

unsigned skip_disclaimer = 0;

static void retro_audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   retro_audio_buff_active    = active;
   retro_audio_buff_occupancy = occupancy;
   retro_audio_buff_underrun  = underrun_likely;
}

static void retro_set_audio_buff_status_cb(void)
{
   if (frameskip_type > 0)
   {
      struct retro_audio_buffer_status_callback buf_status_cb;

      buf_status_cb.callback = retro_audio_buff_status_cb;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,
            &buf_status_cb))
      {
         retro_audio_buff_active    = false;
         retro_audio_buff_occupancy = 0;
         retro_audio_buff_underrun  = false;
         retro_audio_latency        = 0;
      }
      else
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         uint32_t frame_time_usec = 1000000.0 / Machine->drv->frames_per_second;

         /* Set latency to 6x current frame time... */
         retro_audio_latency = (unsigned)(6 * frame_time_usec / 1000);

         /* ...then round up to nearest multiple of 32 */
         retro_audio_latency = (retro_audio_latency + 0x1F) & ~0x1F;
      }
   }
   else
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      retro_audio_latency = 0;
   }

   update_audio_latency = true;
}

/* Older libretro.h revisions predate this environment call.  Define it
 * defensively; on frontends that do not implement it the callback simply
 * returns false and we fall back to a sensible default. */
#ifndef RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE
#define RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE (81 | 0x10000)
#endif

/* Snap an arbitrary rate to the nearest entry on the standard ladder. */
static int snap_to_standard_rate(int rate)
{
   static const int ladder[] = { 8000, 11025, 22050, 32000, 44100, 48000, 96000 };
   int    best      = ladder[0];
   int    best_dist = (rate > ladder[0]) ? (rate - ladder[0]) : (ladder[0] - rate);
   unsigned i;

   for (i = 1; i < sizeof(ladder) / sizeof(ladder[0]); i++)
   {
      int dist = (rate > ladder[i]) ? (rate - ladder[i]) : (ladder[i] - rate);
      if (dist < best_dist)
      {
         best_dist = dist;
         best      = ladder[i];
      }
   }
   return best;
}

/* Resolve the "auto"/"manual" option to a concrete rate.  Query the
 * frontend's target rate and round it to the nearest standard rate; if
 * the frontend does not report one, fall back to 48000. */
static int resolve_auto_sample_rate(void)
{
   unsigned target = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE, &target) && target != 0)
      return snap_to_standard_rate((int)target);

   return 48000;
}

static void update_variables(bool first_run)
{
    struct retro_variable var;
    bool prev_frameskip_type;

    var.key = "mame2000-frameskip";
    var.value = NULL;

    prev_frameskip_type = frameskip_type;
    frameskip_type      = 0;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
       if (strcmp(var.value, "auto") == 0)
          frameskip_type = 1;
       if (strcmp(var.value, "threshold") == 0)
          frameskip_type = 2;
    }

    var.key = "mame2000-frameskip_threshold";
    var.value = NULL;

    frameskip_threshold = 30;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
       frameskip_threshold = strtol(var.value, NULL, 10);

    var.key = "mame2000-frameskip_interval";
    var.value = NULL;

    frameskip_interval = 1;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
       frameskip_interval = strtol(var.value, NULL, 10);

    var.value = NULL;
    var.key = "mame2000-skip_disclaimer";
    
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if(strcmp(var.value, "enabled") == 0)
            skip_disclaimer = 1;
        else
            skip_disclaimer = 0;
    }
    else
        skip_disclaimer = 0;
    
    var.value = NULL;
    var.key = "mame2000-show_gameinfo";
    
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if(strcmp(var.value, "enabled") == 0)
            global_showinfo = 1;
        else
            global_showinfo = 0;
    }
    else
        global_showinfo = 0;

    var.value = NULL;
    var.key = "mame2000-sample_rate";

    sample_rate = 22050;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
       /* "auto" and "manual" resolve to a concrete rate at read time.
        * MAME 0.78 has no per-machine native output rate (every sound
        * chip renders at Machine->sample_rate), so both currently ask
        * the frontend for its target rate and snap it to the nearest
        * value on the standard ladder, falling back to 48000.  A true
        * "manual" native rate (derived from per-chip clocks) is a
        * future change. */
       if (!strcmp(var.value, "auto") || !strcmp(var.value, "manual"))
          sample_rate = resolve_auto_sample_rate();
       else
          sample_rate = strtol(var.value, NULL, 10);
    }

    var.value = NULL;
    var.key = "mame2000-stereo";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if(strcmp(var.value, "enabled") == 0)
            stereo_enabled = true;
        else
            stereo_enabled = false;
    }
    else
        stereo_enabled = true;

    var.value = NULL;
    var.key = "mame2000-qsound_output_filter";

    /* Default disabled.  This flag lives in src/sound/qsound.c and is
     * read once per output sample in qsound_update to bypass the FIR
     * and output-delay processing when zero. */
    {
        extern int qsound_output_filter_enabled;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value
            && strcmp(var.value, "enabled") == 0)
            qsound_output_filter_enabled = 1;
        else
            qsound_output_filter_enabled = 0;
    }

   /* Reinitialise frameskipping, if required */
   if (!first_run &&
       ((frameskip_type     != prev_frameskip_type)))
      retro_set_audio_buff_status_cb();
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   /* Register the core options via the best API the running frontend
    * supports: v2 (with sublabels and forward-compat categories) when
    * available, falling back to v1 and finally legacy SET_VARIABLES.
    * Definitions live in libretro_core_options.h. */
   libretro_set_core_options(cb);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
   machine_reset();
}

static void update_input(void)
{
#define RK(port,key)     input_state_cb(port, RETRO_DEVICE_KEYBOARD, 0,RETROK_##key)
#define JS(port, button) joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_##button)
	/* Per-player digital direction bits, in the same GP2X bitmask
	 * format the OSD analog/trakball readers expect.  Defined in
	 * src/libretro/input.c at file scope; one variable per port.
	 * joy_analog_x/y[] are the per-port normalised analog stick
	 * positions, range -1.0 .. 1.0, consumed by osd_analogjoy_read()
	 * and osd_trak_read(). */
	extern unsigned long ExKey1, ExKey2, ExKey3, ExKey4;
	extern float joy_analog_x[4], joy_analog_y[4];
	static unsigned long *const exkey_for_player[4] = {
		&ExKey1, &ExKey2, &ExKey3, &ExKey4
	};
	int i, j, c = 0;
	input_poll_cb();
	
	key[KEY_TAB] = 0;
	for (i = 0; i < 4; i++)
	{
		key[KEY_1 + i] = 0;
		key[KEY_5 + i] = 0;
	}
	
	for (i = 0; i < 4; i++)
	{
		int16_t joypad_bits;
		int16_t analog_x, analog_y;
		unsigned long ex_bits = 0;
		
		if (libretro_supports_bitmasks)
			joypad_bits = input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		else
		{
			joypad_bits = 0;
			for (j = 0; j < (RETRO_DEVICE_ID_JOYPAD_R3+1); j++)
				joypad_bits |= input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, j) ? (1 << j) : 0;
		}

		key[KEY_1 + i]   |= JS(i, START);
		key[KEY_5 + i]   |= JS(i, SELECT);
		joy_pressed[c++] = JS(i, LEFT);
		joy_pressed[c++] = JS(i, RIGHT);
		joy_pressed[c++] = JS(i, UP);
		joy_pressed[c++] = JS(i, DOWN);
		joy_pressed[c++] = JS(i, B);
		joy_pressed[c++] = JS(i, A);
		joy_pressed[c++] = JS(i, Y);
		joy_pressed[c++] = JS(i, X);
		joy_pressed[c++] = JS(i, L);
		joy_pressed[c++] = JS(i, R);

		key[KEY_TAB] |= JS(i, R2);

		/* Feed the OSD analog/trakball readers.
		 *
		 * Two parallel paths inside osd_analogjoy_read() / osd_trak_-
		 * read(): the "stick is moved" branch snaps the reported value
		 * to joy_analog_x[player] * 128 (or *30 for trakball), and the
		 * "stick is centred but a direction is held" branch ramps the
		 * accumulator by +/-5 per call.  Both need feeding from libretro:
		 *
		 *   - joy_analog_x/y[i] from RETRO_DEVICE_ANALOG / ANALOG_LEFT,
		 *     normalised to the -1.0 .. +1.0 range the OSD code's
		 *     "* 128.0" / "* 30" arithmetic expects.  Y is negated at
		 *     the assignment so the *-128 the OSD code already applies
		 *     produces RetroArch's positive-down convention downstream.
		 *
		 *   - ExKey1..4 (dispatched by player index via the static
		 *     pointer table above) carry the per-port digital direction
		 *     bits in the GP2X_UP/DOWN/LEFT/RIGHT bitmask format that
		 *     is_joy_axis_pressed() decodes.  Synthesised here from the
		 *     same JS() reads we already did for joy_pressed[] -- a
		 *     single source of truth keeps digital state coherent
		 *     whether the game queries via joy_pressed[] or via the
		 *     analog reader's digital-fallback path.
		 *
		 * Frontend deadzone (RetroArch's per-port stick deadzone, or
		 * any equivalent on other frontends) is already applied before
		 * input_state_cb returns -- we don't add a second one. */
		analog_x = input_state_cb(i, RETRO_DEVICE_ANALOG,
		                          RETRO_DEVICE_INDEX_ANALOG_LEFT,
		                          RETRO_DEVICE_ID_ANALOG_X);
		analog_y = input_state_cb(i, RETRO_DEVICE_ANALOG,
		                          RETRO_DEVICE_INDEX_ANALOG_LEFT,
		                          RETRO_DEVICE_ID_ANALOG_Y);
		joy_analog_x[i] =  (float)analog_x / 32768.0f;
		joy_analog_y[i] = -(float)analog_y / 32768.0f;

		if (JS(i, LEFT))  ex_bits |= GP2X_LEFT;
		if (JS(i, RIGHT)) ex_bits |= GP2X_RIGHT;
		if (JS(i, UP))    ex_bits |= GP2X_UP;
		if (JS(i, DOWN))  ex_bits |= GP2X_DOWN;
		*exkey_for_player[i] = ex_bits;
	}

	key[KEY_A] =RK(0, a);
	key[KEY_B] =RK(0, b);
	key[KEY_C] =RK(0, c);
	key[KEY_D] =RK(0, d);
	key[KEY_E] =RK(0, e);
	key[KEY_F] =RK(0, f);
	key[KEY_G] =RK(0, g);
	key[KEY_H] =RK(0, h);
	key[KEY_I] =RK(0, i);
	key[KEY_J] =RK(0, j);
	key[KEY_K] =RK(0, k);
	key[KEY_L] =RK(0, l);
	key[KEY_M] =RK(0, m);
	key[KEY_N] =RK(0, n);
	key[KEY_O] =RK(0, o);
	key[KEY_P] =RK(0, p);
	key[KEY_Q] =RK(0, q);
	key[KEY_R] =RK(0, r);
	key[KEY_S] =RK(0, s);
	key[KEY_T] =RK(0, t);
	key[KEY_U] =RK(0, u);
	key[KEY_V] =RK(0, v);
	key[KEY_W] =RK(0, w);
	key[KEY_X] =RK(0, x);
	key[KEY_Y] =RK(0, y);
	key[KEY_Z] =RK(0, z);
	key[KEY_0] =RK(0, 0);
	key[KEY_1] |=RK(0, 1);
	key[KEY_2] |=RK(0, 2);
	key[KEY_3] |=RK(0, 3);
	key[KEY_4] |=RK(0, 4);
	key[KEY_5] |=RK(0, 5);
	key[KEY_6] |=RK(0, 6);
	key[KEY_7] |=RK(0, 7);
	key[KEY_8] |=RK(0, 8);
	key[KEY_9] =RK(0, 9);
	key[KEY_0_PAD] =RK(0, KP0);
	key[KEY_1_PAD] =RK(0, KP1);
	key[KEY_2_PAD] =RK(0, KP2);
	key[KEY_3_PAD] =RK(0, KP3);
	key[KEY_4_PAD] =RK(0, KP4);
	key[KEY_5_PAD] =RK(0, KP5);
	key[KEY_6_PAD] =RK(0, KP6);
	key[KEY_7_PAD] =RK(0, KP7);
	key[KEY_8_PAD] =RK(0, KP8);
	key[KEY_9_PAD] =RK(0, KP9);
	key[KEY_F1] =RK(0, F1);
	key[KEY_F2] =RK(0, F2);
	key[KEY_F3] =RK(0, F3);
	key[KEY_F4] =RK(0, F4);
	key[KEY_F5] =RK(0, F5);
	key[KEY_F6] =RK(0, F6);
	key[KEY_F7] =RK(0, F7);
	key[KEY_F8] =RK(0, F8);
	key[KEY_F9] =RK(0, F9);
	key[KEY_F10] =RK(0, F10);
	key[KEY_F11] =RK(0, F11);
	key[KEY_F12] =RK(0, F12);
	key[KEY_ESC] =RK(0, ESCAPE);
	key[KEY_TILDE] =RK(0, TILDE);
	key[KEY_MINUS] =RK(0, MINUS);
	key[KEY_EQUALS] =RK(0, EQUALS);
	key[KEY_BACKSPACE] =RK(0, BACKSPACE);
	key[KEY_TAB] |=RK(0, TAB);
	key[KEY_OPENBRACE] =RK(0, LEFTBRACKET);
	key[KEY_CLOSEBRACE] =RK(0, RIGHTBRACKET);
	key[KEY_ENTER] =RK(0, RETURN);
	key[KEY_COLON] =RK(0, COLON);
	key[KEY_QUOTE] =RK(0, QUOTE);
	key[KEY_BACKSLASH] =RK(0, BACKSLASH);
	key[KEY_BACKSLASH2] =RK(0, LESS);
	key[KEY_COMMA] =RK(0, COMMA);
	key[KEY_STOP] =RK(0, PERIOD);
	key[KEY_SLASH] =RK(0, SLASH);
	key[KEY_SPACE] =RK(0, SPACE);
	key[KEY_INSERT] =RK(0, INSERT);
	key[KEY_DEL] =RK(0, DELETE);
	key[KEY_HOME] =RK(0, HOME);
	key[KEY_END] =RK(0, END);
	key[KEY_PGUP] =RK(0, PAGEUP);
	key[KEY_PGDN] =RK(0, PAGEDOWN);
	key[KEY_LEFT] =RK(0, LEFT);
	key[KEY_RIGHT] =RK(0, RIGHT);
	key[KEY_UP] =RK(0, UP);
	key[KEY_DOWN] =RK(0, DOWN);
	key[KEY_SLASH_PAD] =RK(0, KP_DIVIDE);
	key[KEY_ASTERISK] =RK(0, ASTERISK);
	key[KEY_MINUS_PAD] =RK(0, KP_MINUS);
	key[KEY_PLUS_PAD] =RK(0, KP_PLUS);
	key[KEY_DEL_PAD] =RK(0, KP_PERIOD);
	key[KEY_ENTER_PAD] =RK(0, KP_ENTER);
	key[KEY_PRTSCR] =RK(0, PRINT);
	key[KEY_PAUSE] =RK(0, PAUSE);
	key[KEY_LSHIFT] =RK(0, LSHIFT);
	key[KEY_RSHIFT] =RK(0, RSHIFT);
	key[KEY_LCONTROL] =RK(0, LCTRL);
	key[KEY_RCONTROL] =RK(0, RCTRL);
	key[KEY_ALT] =RK(0, LALT);
	key[KEY_ALTGR] =RK(0, RALT);
	key[KEY_LWIN] =RK(0, LMETA);
	key[KEY_RWIN] =RK(0, RMETA);
	key[KEY_MENU] =RK(0, MENU);
	key[KEY_SCRLOCK] =RK(0, SCROLLOCK);
	key[KEY_NUMLOCK] =RK(0, NUMLOCK);
	key[KEY_CAPSLOCK] =RK(0, CAPSLOCK);

#undef RK
#undef JS
#undef _B
}

/* MAME callbacks invoked from osd_update_audio_stream() (sound.c) and
 * osd_update_video_and_audio() (video.c) at the tail of each frame's
 * audio/video output respectively.  In the legacy libco-coroutine and
 * threaded models these functions used to perform cross-stack
 * synchronisation to yield control back to retro_run; now that the
 * emulator returns up the stack naturally, the audio hook is a no-op
 * and the video hook simply raises yield_pending so cpu_run_step()
 * exits its scheduling loop. */
void hook_audio_done(void)
{
}

void hook_video_done(void)
{
   yield_pending = 1;
}

void retro_init(void)
{
   /* gp2x_screen15 is allocated lazily by gp2x_set_video_mode() once
    * MAME tells us the game's actual resolution.  See the comment
    * there for the rationale (avoids the historical fixed 614 KB
    * allocation regardless of what the game needs). */
   gp2x_screen15       = NULL;
   gp2x_screen15_owned = NULL;
   gp2x_screen15_bytes = 0;
   init_joy_list();
   update_variables(true);

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;
}

void retro_deinit(void)
{
   free(IMAMEBASEPATH);   IMAMEBASEPATH   = NULL;
   free(IMAMESAMPLEPATH); IMAMESAMPLEPATH = NULL;
   /* retro_content_directory is the only string here we own (strdup'd
    * from info->path in retro_load_game).  retro_system_directory /
    * retro_save_directory either point at frontend-owned memory
    * returned from RETRO_ENVIRONMENT_GET_*_DIRECTORY (don't free
    * those) or alias retro_content_directory as a fallback (don't
    * free those either, the underlying buffer is the one we're about
    * to free here).  Null all three so nothing dangles. */
   free(retro_content_directory);
   retro_content_directory = NULL;
   retro_system_directory  = NULL;
   retro_save_directory    = NULL;
   /* If a SW-FB happens to still be patched in (shouldn't, retro_run
    * always restores), free the *owned* buffer rather than the SW-FB
    * pointer to avoid handing a foreign address back to the allocator. */
   gp2x_screen15 = gp2x_screen15_owned;
   if (gp2x_screen15_owned)
   {
#ifdef _3DS
      linearFree(gp2x_screen15_owned);
#else
      free(gp2x_screen15_owned);
#endif
   }
   gp2x_screen15       = NULL;
   gp2x_screen15_owned = NULL;
   gp2x_screen15_bytes = 0;
   sw_fb_active_data   = NULL;

   libretro_supports_bitmasks = false;
   frameskip_type             = 0;
   frameskip_threshold        = 0;
   frameskip_counter          = 0;
   retro_audio_buff_active    = false;
   retro_audio_buff_occupancy = 0;
   retro_audio_buff_underrun  = false;
   retro_audio_latency        = 0;
   update_audio_latency       = false;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name     = "MAME 2000";
#ifdef GIT_VERSION
   info->library_version  = "0.37b5" GIT_VERSION;
#else
   info->library_version  = build_version;
#endif
   info->need_fullpath    = true;
   info->valid_extensions = "zip|ZIP";
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   
   float aspect_ratio = Machine->orientation & ORIENTATION_SWAP_XY? ( (float) 3 / (float) 4) : ( (float) 4/ (float) 3);
   struct retro_game_geometry g = {
     emulated_width,
      emulated_height,
      emulated_width,
      emulated_height,
      aspect_ratio
   };
   struct retro_system_timing t = {
      Machine->drv->frames_per_second,
      (double)Machine->sample_rate
   };
   info->timing = t;
   info->geometry = g;
}

void retro_run(void)
{
#ifdef MAMEGO
   {
      extern unsigned int mamego_gfx_frame; /* drawgfx.c tile cache: tiles used in this frame stay */
      mamego_gfx_frame++;
   }
#endif
   /* Software-framebuffer fast path.
    *
    * Before the emulator runs the next frame, ask the frontend for a
    * buffer matching this frame's geometry and our pixel format.  If
    * granted, point gp2x_screen15 at it for the duration of the frame:
    * blit.c then writes directly into the frontend's memory and the
    * video_cb call that follows is a zero-copy signal.  When no
    * buffer is granted (or the geometry doesn't match exactly) we keep
    * using the core-owned buffer and the existing video_cb path.
    *
    * Geometry must match exactly per the libretro spec: width, height
    * and pitch as returned by the frontend, and the byte pitch must
    * equal gfx_width * 2 because blit.c does its row arithmetic from
    * gfx_width.  The format must be RGB565; if the frontend gives us a
    * different one (e.g. when it would have to convert internally) we
    * pass.  These constraints make the optimisation conservative -- a
    * mismatched frontend just sees the existing slow path. */
   sw_fb_active_data = NULL;
   if (gfx_width > 0 && gfx_height > 0 && gp2x_screen15_owned != NULL)
   {
      struct retro_framebuffer fb;
      fb.data         = NULL;
      fb.width        = gfx_width;
      fb.height       = gfx_height;
      fb.pitch        = 0;
      fb.format       = RETRO_PIXEL_FORMAT_RGB565;
      fb.access_flags = RETRO_MEMORY_ACCESS_WRITE;
      fb.memory_flags = 0;

      if (environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb)
          && fb.data != NULL
          && fb.format == RETRO_PIXEL_FORMAT_RGB565
          && fb.pitch  == (size_t)gfx_width * 2)
      {
         sw_fb_active_data  = fb.data;
         sw_fb_active_pitch = fb.pitch;
         gp2x_screen15      = (unsigned short *)fb.data;
      }
   }

   /* Poll input + pick up frontend variable updates BEFORE running
    * the frame, so the inputs read this turn drive THIS frame's CPU
    * dispatch (otherwise the game would be running one frame behind
    * the player -- mame_run_one_frame() reads from key[] / joy_-
    * pressed[] inside osd_is_key_pressed / osd_is_joy_pressed as
    * the CPU schedule advances, and those arrays would still hold
    * last frame's values).  Mirrors mame2003-libretro's retro_run
    * which puts poll_cb() and the keyboard / joypad sample loop
    * ahead of the frame as well. */
   {
      bool updated = false;
      update_input();
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
         update_variables(false);
   }

   /* Run one frame of CPU scheduling.  Returns when the timer system
    * has fired its VBLANK update path through osd_update_video_and_-
    * audio(), which calls hook_video_done() to raise yield_pending. */
   mame_run_one_frame();

   PROF_PUSH(PROF_OUT);
#ifdef MAMEGO
   extern int mame2000_frame_presented;
   if (mame2000_frame_presented)
      mame2000_frame_presented = 0; /* the host already has it (mamego_present_indexed) */
   else
#endif
   if (should_skip_frame)
      video_cb(NULL, gfx_width, gfx_height, gfx_width * 2);
   else if (mame2000_direct_frame_data != 0)
      /* Bitmap-direct fast path: the just-finished frame skipped the
       * blit and recorded the MAME scrbitmap pointer for us to deliver.
       * The bitmap stride is wider than the visible width (osd_alloc_-
       * bitmap pads each row with safety pixels and rounds the width
       * up to a quadword); video_cb accepts the stride as the pitch. */
      video_cb(mame2000_direct_frame_data, gfx_width, gfx_height,
               mame2000_direct_frame_pitch);
   else
      video_cb(gp2x_screen15, gfx_width, gfx_height, gfx_width * 2);
   PROF_POP();

   /* Restore the core-owned buffer for the next frame so allocation
    * lifetimes stay sane regardless of whether the frontend grants a
    * buffer again next time. */
   if (sw_fb_active_data != NULL)
   {
      gp2x_screen15     = gp2x_screen15_owned;
      sw_fb_active_data = NULL;
   }

   /* Audio dispatch.  When pause_action is set, osd_update_silent_-
    * stream() (called from updatescreen() while we were paused)
    * already delivered a frame of silence via audio_batch_cb -- skip
    * the dispatch here to avoid double-delivery.  This matches
    * mame2003-libretro, where audio always flows through the OSD
    * callbacks (osd_update_audio_stream when running, osd_update_-
    * silent_stream when paused) and retro_run never dispatches
    * directly.  In mame2000 we keep retro_run's tail dispatch for
    * the running case because osd_update_audio_stream() only fills
    * samples_buffer; the libretro-side delivery has historically
    * happened here.
    *
    * samples_buffer is always allocated stereo-sized regardless of
    * the game's native sound layout; the mixer (mixer.c:mixer_sh_-
    * update) writes interleaved L/R directly into it -- mono games
    * duplicate at the clip step.  No conversion needed here. */
   if (samples_per_frame && !pause_action)
      audio_batch_cb(samples_buffer, samples_per_frame);

   /* If frameskip/timing settings have changed,
    * update frontend audio latency
    * > Can do this before or after the frameskip
    *   check, but doing it after means we at least
    *   retain the current frame's audio output */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
                 &retro_audio_latency);
      update_audio_latency = false;
   }

}

bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 0, 0, 0, 0, NULL },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      fprintf(stderr, "[libretro]: RGB565 is not supported.\n");
      return false;
   }

  /* Re-init safety: free any prior allocation so a second load_game
   * (e.g. core restart) does not leak.  Same pattern as IMAMEBASEPATH /
   * IMAMESAMPLEPATH below. */
  free(retro_content_directory);
  retro_content_directory = strdup(info->path);
  path_basedir(retro_content_directory);

  printf("CONTENT_DIRECTORY: %s\n", retro_content_directory);

  /* Get system directory from frontend */
  retro_system_directory = NULL;
  environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&retro_system_directory);
  if (retro_system_directory == NULL || retro_system_directory[0] == '\0')
  {
      printf("libretro system path not set by frontend, using content path\n");
      retro_system_directory = retro_content_directory;
  }
   printf("SYSTEM_DIRECTORY: %s\n", retro_system_directory);


  /* Get save directory from frontend */
  retro_save_directory = NULL;
  environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,&retro_save_directory);
  if (retro_save_directory == NULL || retro_save_directory[0] == '\0')
  {
      printf("libretro save path not set by frontent, using content path\n");
      retro_save_directory = retro_content_directory;
  }
   printf("SAVE_DIRECTORY: %s\n", retro_save_directory);

   snprintf(core_sys_directory, sizeof(core_sys_directory),
            "%s%cmame2000", retro_system_directory, slash);
   snprintf(core_save_directory, sizeof(core_save_directory),
            "%s%cmame2000", retro_save_directory, slash);
   printf("MAME2000_SYS_DIRECTORY: %s\n", core_sys_directory);
   printf("MAME2000_SAVE_DIRECTORY: %s\n", core_save_directory);

   /* Re-init safety: free any prior allocation so a second load_game
    * (e.g. core restart) does not leak. */
   free(IMAMEBASEPATH);
   free(IMAMESAMPLEPATH);
   IMAMEBASEPATH   = (char *) malloc(PATH_BUF_SIZE);
   IMAMESAMPLEPATH = (char *) malloc(PATH_BUF_SIZE);
   if (!IMAMEBASEPATH || !IMAMESAMPLEPATH)
   {
      free(IMAMEBASEPATH);   IMAMEBASEPATH   = NULL;
      free(IMAMESAMPLEPATH); IMAMESAMPLEPATH = NULL;
      printf("Failed to allocate path buffers\n");
      return false;
   }

   int i;
   {
      const char *romName;
      char baseName[1024];
      char *dot;

      strncpy(IMAMEBASEPATH, info->path, PATH_BUF_SIZE - 1);
      IMAMEBASEPATH[PATH_BUF_SIZE - 1] = 0;
      if (strrchr(IMAMEBASEPATH, slash))
         *(strrchr(IMAMEBASEPATH, slash)) = 0;
      else
      {
         IMAMEBASEPATH[0] = '.';
         IMAMEBASEPATH[1] = 0;
      }

      romName = info->path;
      if (strrchr(info->path, slash))
         romName = strrchr(info->path, slash) + 1;
      /* Bounded copy.  Original code did a strlen()-sized memcpy into a
       * fixed 1024-byte buffer; a pathological filename could stack-smash. */
      strncpy(baseName, romName, sizeof(baseName) - 1);
      baseName[sizeof(baseName) - 1] = 0;
      dot = strrchr(baseName, '.');
      if (dot)
         *dot = 0;

      snprintf(IMAMESAMPLEPATH, PATH_BUF_SIZE, "%s/samples", core_sys_directory);

   /* do we have a driver for this? */
#ifdef MAMEGO
   {
      extern int mamego_pick_driver(const char *zip_path, const char *base_name);
      game_index = mamego_pick_driver(info->path, baseName);
   }
#else
   for (i = 0; drivers[i] && (game_index == -1); i++)
   {
	   if (strcasecmp(baseName,drivers[i]->name) == 0)
	   {
		   game_index = i;
		   break;
	   }
   }
#endif

   if (game_index == -1)
   {
	   printf("Game \"%s\" not supported\n", baseName);
	   return false;
   }
   }

   /* parse generic (os-independent) options */
   //parse_cmdline (argc, argv, game_index);

   //Set default path
   nvdir=(char *) malloc(PATH_BUF_SIZE);snprintf(nvdir,PATH_BUF_SIZE,"%s%c%s",core_save_directory,slash,"nvram");
   i=create_path_recursive(nvdir);
   if(i!=0)printf("error %d creating nvram \"%s\"\n", i,nvdir);

   hidir=(char *) malloc(PATH_BUF_SIZE);snprintf(hidir,PATH_BUF_SIZE,"%s%c%s",core_save_directory,slash,"hi");
   i=create_path_recursive(hidir);
   if(i!=0)printf("error %d creating hi \"%s\"\n", i,hidir);

   cfgdir=(char *) malloc(PATH_BUF_SIZE);snprintf(cfgdir,PATH_BUF_SIZE,"%s%c%s",core_save_directory,slash,"cfg");
   i=create_path_recursive(cfgdir);
   if(i!=0)printf("error %d creating cfg \"%s\"\n", i,cfgdir);

   screenshotdir=(char *) malloc(PATH_BUF_SIZE);snprintf(screenshotdir,PATH_BUF_SIZE,"%s%c%s",core_save_directory,slash,"snap");
   i=create_path_recursive(screenshotdir);
   if(i!=0)printf("error %d creating snap \"%s\"\n", i,screenshotdir);

   memcarddir=(char *) malloc(PATH_BUF_SIZE);snprintf(memcarddir,PATH_BUF_SIZE,"%s%c%s",core_save_directory,slash,"memcard");
   i=create_path_recursive(memcarddir);
   if(i!=0)printf("error %d creating memcard \"%s\"\n", i,memcarddir);

   stadir=(char *) malloc(PATH_BUF_SIZE);snprintf(stadir,PATH_BUF_SIZE,"%s%c%s",core_sys_directory,slash,"sta");
   i=create_path_recursive(stadir);
   if(i!=0)printf("error %d creating sta \"%s\"\n", i,stadir);

   artworkdir=(char *) malloc(PATH_BUF_SIZE);snprintf(artworkdir,PATH_BUF_SIZE,"%s%c%s",core_sys_directory,slash,"artwork");
   i=create_path_recursive(artworkdir);
   if(i!=0)printf("error %d creating artwork \"%s\"\n", i,artworkdir);

   cheatdir=(char *) malloc(PATH_BUF_SIZE);snprintf(cheatdir,PATH_BUF_SIZE,"%s%c%s",core_sys_directory,slash,"cheat");
   i=create_path_recursive(cheatdir);
   if(i!=0)printf("error %d creating cheat \"%s\"\n", i,cheatdir);

   Machine->sample_rate = sample_rate;
   options.samplerate = sample_rate;
   usestereo = stereo_enabled;

   /* This is needed so emulated YM3526/YM3812 chips are used instead on physical ones. */
   options.use_emulated_ym3812 = 1;

   /* enable samples - should be stored in "sample" subdirectory from roms */
   options.use_samples = 1;

   /* skip disclaimer - skips 'nag screen' */
   options.skip_disclaimer = skip_disclaimer;

#if (HAS_CYCLONE || HAS_DRZ80)
   int use_cyclone = 1;
   int use_drz80 = 1;
   int use_drz80_snd = 1;

	for (i=0;i<NUMGAMES;i++)
 	{
		if (strcmp(drivers[game_index]->name,fe_drivers[i].name)==0)
		{
			/* ASM cores: 0=None,1=Cyclone,2=DrZ80,3=Cyclone+DrZ80,4=DrZ80(snd),5=Cyclone+DrZ80(snd) */
         switch (fe_drivers[i].cores)
         {
         case 0:
            use_cyclone = 0;
				use_drz80_snd = 0;
				use_drz80 = 0;
            break;
         case 1:
				use_drz80_snd = 0;
				use_drz80 = 0;
            break;
         case 2:
            use_cyclone = 0;
            break;
         case 4:
            use_cyclone = 0;
				use_drz80 = 0;
            break;
         case 5:
				use_drz80 = 0;
            break;
         default:
            break;
         }
			
         break;
		}
	}

   /* Replace M68000 by CYCLONE */
#if (HAS_CYCLONE)
   if (use_cyclone)
   {
	   for (i=0;i<MAX_CPU;i++)
	   {
		   int *type=(int*)&(drivers[game_index]->drv->cpu[i].cpu_type);
#ifdef NEOMAME
		   if (((*type)&0xff)==CPU_M68000)
#else
			   if (((*type)&0xff)==CPU_M68000 || ((*type)&0xff)==CPU_M68010 )
#endif
			   {
				   *type=((*type)&(~0xff))|CPU_CYCLONE;
			   }
	   }
   }
#endif

#if (HAS_DRZ80)
	/* Replace Z80 by DRZ80 */
	if (use_drz80)
	{
		for (i=0;i<MAX_CPU;i++)
		{
			int *type=(int*)&(drivers[game_index]->drv->cpu[i].cpu_type);
			if (((*type)&0xff)==CPU_Z80)
			{
				*type=((*type)&(~0xff))|CPU_DRZ80;
			}
		}
	}

	/* Replace Z80 with DRZ80 only for sound CPUs */
	if (use_drz80_snd)
	{
		for (i=0;i<MAX_CPU;i++)
		{
			int *type=(int*)&(drivers[game_index]->drv->cpu[i].cpu_type);
			if ((((*type)&0xff)==CPU_Z80) && ((*type)&CPU_AUDIO_CPU))
			{
				*type=((*type)&(~0xff))|CPU_DRZ80;
			}
		}
	}
#endif

#endif

   // Remove the mouse usage for certain games
   if ( (strcasecmp(drivers[game_index]->name,"hbarrel")==0) || (strcasecmp(drivers[game_index]->name,"hbarrelw")==0) ||
		   (strcasecmp(drivers[game_index]->name,"midres")==0) || (strcasecmp(drivers[game_index]->name,"midresu")==0) ||
		   (strcasecmp(drivers[game_index]->name,"midresj")==0) || (strcasecmp(drivers[game_index]->name,"tnk3")==0) ||
		   (strcasecmp(drivers[game_index]->name,"tnk3j")==0) || (strcasecmp(drivers[game_index]->name,"ikari")==0) ||
		   (strcasecmp(drivers[game_index]->name,"ikarijp")==0) || (strcasecmp(drivers[game_index]->name,"ikarijpb")==0) ||
		   (strcasecmp(drivers[game_index]->name,"victroad")==0) || (strcasecmp(drivers[game_index]->name,"dogosoke")==0) ||
		   (strcasecmp(drivers[game_index]->name,"gwar")==0) || (strcasecmp(drivers[game_index]->name,"gwarj")==0) ||
		   (strcasecmp(drivers[game_index]->name,"gwara")==0) || (strcasecmp(drivers[game_index]->name,"gwarb")==0) ||
		   (strcasecmp(drivers[game_index]->name,"bermudat")==0) || (strcasecmp(drivers[game_index]->name,"bermudaj")==0) ||
		   (strcasecmp(drivers[game_index]->name,"bermudaa")==0) || (strcasecmp(drivers[game_index]->name,"mplanets")==0) ||
		   (strcasecmp(drivers[game_index]->name,"forgottn")==0) || (strcasecmp(drivers[game_index]->name,"lostwrld")==0) ||
		   (strcasecmp(drivers[game_index]->name,"gondo")==0) || (strcasecmp(drivers[game_index]->name,"makyosen")==0) ||
		   (strcasecmp(drivers[game_index]->name,"topgunr")==0) || (strcasecmp(drivers[game_index]->name,"topgunbl")==0) ||
		   (strcasecmp(drivers[game_index]->name,"tron")==0) || (strcasecmp(drivers[game_index]->name,"tron2")==0) ||
		   (strcasecmp(drivers[game_index]->name,"kroozr")==0) ||(strcasecmp(drivers[game_index]->name,"crater")==0) ||
		   (strcasecmp(drivers[game_index]->name,"dotron")==0) || (strcasecmp(drivers[game_index]->name,"dotrone")==0) ||
		   (strcasecmp(drivers[game_index]->name,"zwackery")==0) || (strcasecmp(drivers[game_index]->name,"ikari3")==0) ||
		   (strcasecmp(drivers[game_index]->name,"searchar")==0) || (strcasecmp(drivers[game_index]->name,"sercharu")==0) ||
		   (strcasecmp(drivers[game_index]->name,"timesold")==0) || (strcasecmp(drivers[game_index]->name,"timesol1")==0) ||
		   (strcasecmp(drivers[game_index]->name,"btlfield")==0) || (strcasecmp(drivers[game_index]->name,"aztarac")==0))
   {
	   extern int use_mouse;
	   use_mouse=0;
   }

   decompose_rom_sample_path(IMAMEBASEPATH, IMAMESAMPLEPATH);

   /* Drive MAME's per-game init chain synchronously: osd_init,
    * init_machine, run_machine_init (which calls cpu_run_init at its
    * tail).  After this returns 0 the emulator is ready to render
    * frames; the first retro_run() call will deliver frame 0. */
   if (mame_start_game(game_index) != 0)
      return false;

   /* Driver-conditional core-option visibility: the QSound output
    * filter option only makes sense for QSound-using drivers (mostly
    * the CPS1.5 / CPS Dash family: dino, slammast, punisher, mbombrd,
    * wof, etc.).  Scan Machine->drv->sound[] for SOUND_QSOUND and
    * tell the frontend to hide the option in the menu for everything
    * else.  Uses RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY which is
    * compatible with the legacy SET_VARIABLES API used above. */
   {
      struct retro_core_option_display option_display;
      int snd_idx;
      bool qsound_active = false;

      for (snd_idx = 0; snd_idx < MAX_SOUND; snd_idx++)
      {
         if (Machine->drv->sound[snd_idx].sound_type == SOUND_QSOUND)
         {
            qsound_active = true;
            break;
         }
      }
      option_display.key     = "mame2000-qsound_output_filter";
      option_display.visible = qsound_active;
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
   }

   retro_set_audio_buff_status_cb();
   return true;
}

void retro_unload_game(void)
{
   /* Reverse the init chain: cpu_run_exit (via run_machine_exit) then
    * shutdown_machine, osd_exit.  All driven synchronously from the
    * libretro main thread now that there is no background coroutine
    * or kernel thread to wind down. */
   mame_end_game();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

#ifdef MAMEGO
/* generic 8-bit board snapshot, see mamego_state_walk() in cpuintrf.c */
size_t mamego_state_size(void);
int mamego_state_save(void *data, size_t size);
int mamego_state_load(const void *data, size_t size);

size_t retro_serialize_size(void)
{
   return mamego_state_size();
}

bool retro_serialize(void *data_, size_t size)
{
   return mamego_state_save(data_, size);
}

bool retro_unserialize(const void *data_, size_t size)
{
   return mamego_state_load(data_, size);
}
#else
size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data_, size_t size)
{
   (void)data_;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data_, size_t size)
{
   (void)data_;
   (void)size;
   return false;
}

#endif

void *retro_get_memory_data(unsigned id)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
