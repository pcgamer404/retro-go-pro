#ifndef MACHINE_H
#define MACHINE_H

#include "osdepend.h"
#include "drawgfx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef MESS
#include "mess/mess.h"
#endif

extern char build_version[];

#define MAX_GFX_ELEMENTS 32
#define MAX_MEMORY_REGIONS 32

struct RunningMachine
{
	unsigned char *memory_region[MAX_MEMORY_REGIONS];
	unsigned int memory_region_length[MAX_MEMORY_REGIONS];	/* some drivers might find this useful */
	int memory_region_type[MAX_MEMORY_REGIONS];
	struct GfxElement *gfx[MAX_GFX_ELEMENTS];	/* graphic sets (chars, sprites) */
	struct osd_bitmap *scrbitmap;	/* bitmap to draw into */
	struct rectangle visible_area;
	unsigned short *pens;	/* remapped palette pen numbers. When you write */
							/* directly to a bitmap, never use absolute values, */
							/* use this array to get the pen number. For example, */
							/* if you want to use color #6 in the palette, use */
							/* pens[6] instead of just 6. */
	unsigned short *game_colortable;	/* lookup table used to map gfx pen numbers */
										/* to color numbers */
	unsigned short *remapped_colortable;	/* the above, already remapped through */
											/* Machine->pens */
	const struct GameDriver *gamedrv;	/* contains the definition of the game machine */
	const struct MachineDriver *drv;	/* same as gamedrv->drv */
	int color_depth;	/* video color depth: 8 or 16 */
	int sample_rate;	/* the digital audio sample rate; 0 if sound is disabled. */
						/* This is set to a default value, or a value specified by */
						/* the user; osd_init() is allowed to change it to the actual */
						/* sample rate supported by the audio card. */
	int obsolete;	// was sample_bits;	/* 8 or 16 */
	struct GameSamples *samples;	/* samples loaded from disk */
	struct InputPort *input_ports;	/* the input ports definition from the driver */
								/* is copied here and modified (load settings from disk, */
								/* remove cheat commands, and so on) */
	struct InputPort *input_ports_default; /* original input_ports without modifications */
	int orientation;	/* see #defines in driver.h */
	struct GfxElement *uifont;	/* font used by DisplayText() */
	int uifontwidth,uifontheight;
	int uixmin,uiymin;
	int uiwidth,uiheight;
	int ui_orientation;
};

#ifdef MESS
#define MAX_IMAGES	32
/*
 * This is a filename and it's associated peripheral type
 * The types are defined in mess.h (IO_...)
 */
struct ImageFile {
	const char *name;
	int type;
};
#endif

/* The host platform should fill these fields with the preferences specified in the GUI */
/* or on the commandline. */
struct GameOptions {
	void *record;
	void *playback;
	void *language_file; /* LBO 042400 */

	int mame_debug;
	int cheat;
	int gui_host;

	int samplerate;
	int use_samples;
	int use_emulated_ym3812;

	int color_depth;	/* 8 or 16, any other value means auto */
	int vector_width;	/* requested width for vector games; 0 means default (640) */
	int vector_height;	/* requested height for vector games; 0 means default (480) */
	int norotate;
	int ror;
	int rol;
	int flipx;
	int flipy;
	int beam;
	int flicker;
	int translucency;
	int antialias;
	int use_artwork;
    int skip_disclaimer;

	#ifdef MESS
	struct ImageFile image_files[MAX_IMAGES];
	int image_count;
	#endif
};

extern struct GameOptions options;
extern struct RunningMachine *Machine;

/* Re-entrant game lifecycle.  Replaces the historical monolithic
 * run_game() which ran a single game to completion inline.
 *
 *   mame_start_game(game)  -- drive osd_init, init_machine, vh_open,
 *      driver vh_start, sound_start, NVRAM load, and cpu_run_init.
 *      Returns 0 on success, non-zero on failure.  After it returns 0
 *      the emulator is ready to render frames.
 *
 *   mame_run_one_frame()   -- run one frame's worth of CPU
 *      scheduling.  Returns when the timer system has driven
 *      osd_update_video_and_audio() through hook_video_done(), which
 *      sets the cross-TU yield flag (see src/libretro/libretro.c).
 *      Call this once per retro_run().
 *
 *   mame_end_game()        -- reverse the init chain: cpu_run_exit,
 *      driver vh_stop / sound_stop / NVRAM save / cheat StopCheat /
 *      save_input_port_settings, vh_close, shutdown_machine,
 *      osd_exit.  Safe to call even after a partial mame_start_game()
 *      failure -- the implementation tracks how far init progressed
 *      and unwinds only what succeeded. */
int mame_start_game(int game);
void mame_run_one_frame(void);
void mame_end_game(void);
int updatescreen(void);
void draw_screen(int bitmap_dirty);
void update_video_and_audio(void);
/* osd_fopen() must use this to know if high score files can be used */
int mame_highscore_enabled(void);

#endif
