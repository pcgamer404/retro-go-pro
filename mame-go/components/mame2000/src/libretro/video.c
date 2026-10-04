#include "driver.h"
#include "../mamego_prof.h"
#include <math.h>
#include "vidhrdw/vector.h"
#include "dirty.h"

extern int  global_fps;
extern int isIpad;
extern int emulated_width;
extern int emulated_height;
extern int safe_render_path;

extern unsigned frameskip_type;
extern unsigned frameskip_threshold;
extern unsigned frameskip_counter;
extern unsigned frameskip_interval;

extern int retro_audio_buff_active;
extern unsigned retro_audio_buff_occupancy;
extern int retro_audio_buff_underrun;
extern int should_skip_frame;

int iOS_exitPause = 0;
int iOS_cropVideo = 0;
int iOS_aspectRatio = 0;
int iOS_fixedRes = 0;

void hook_video_done(void);

dirtygrid grid1;
dirtygrid grid2;
char *dirty_old=grid1;
char *dirty_new=grid2;

/* specialized update_screen functions defined in blit.c */
/* dirty mode 1 (VIDEO_SUPPORTS_DIRTY) */
void blitscreen_dirty1_color8(struct osd_bitmap *bitmap);
void blitscreen_dirty1_color16(struct osd_bitmap *bitmap);
void blitscreen_dirty1_palettized16(struct osd_bitmap *bitmap);
/* dirty mode 0 (no osd_mark_dirty calls) */
void blitscreen_dirty0_color8(struct osd_bitmap *bitmap);
void blitscreen_dirty0_color16(struct osd_bitmap *bitmap);
void blitscreen_dirty0_palettized16(struct osd_bitmap *bitmap);

static void update_screen_dummy(struct osd_bitmap *bitmap);
void (*update_screen)(struct osd_bitmap *bitmap) = update_screen_dummy;

/* Bitmap-direct fast path.  When the running game's blit would be a
 * pure memcpy (no palette LUT, no rotation, no offsetting, no
 * cropping) we can skip the blit entirely and hand the MAME screen
 * bitmap straight to the libretro frontend.  osd_update_video_and_audio
 * decides per-frame whether the preconditions hold, sets these
 * globals, and skips update_screen() if it does.  retro_run reads
 * these globals to choose which pointer/pitch to pass to video_cb.
 * Reset to NULL at the start of every frame so a previous frame's
 * decision never leaks into the next one. */
const void *mame2000_direct_frame_data  = 0;
size_t      mame2000_direct_frame_pitch = 0;


static int video_depth,video_fps;
static int modifiable_palette;
static int screen_colors;
static unsigned char *current_palette;
static unsigned int *dirtycolor;
static int dirtypalette;
static int dirty_bright;
static int bright_lookup[256];
extern uint32_t *palette_16bit_lookup;

int frameskip,autoframeskip;
#define FRAMESKIP_LEVELS 12

int video_sync;
int wait_vsync;
int vsync_frame_rate;
int skiplines;
int skipcolumns;
int use_dirty = -1;
float osd_gamma_correction = 1.0;
int brightness;
float brightness_paused_adjust;
int gfx_width;
int gfx_height;

static int viswidth;
static int visheight;
static int skiplinesmax;
static int skipcolumnsmax;
static int skiplinesmin;
static int skipcolumnsmin;

static int vector_game;

int gfx_xoffset;
int gfx_yoffset;
int gfx_display_lines;
int gfx_display_columns;
static int xmultiply,ymultiply;
int throttle = 0;       /* toggled by F10 */

#include "minimal.h"
#define makecol(r,g,b) gp2x_video_color15(r,g,b,0)
#define getr(c) gp2x_video_getr15(c)
#define getg(c) gp2x_video_getg15(c)
#define getb(c) gp2x_video_getb15(c)

int video_scale=0;
int video_border=0;
int video_aspect=0;

/* Create a bitmap. Also calls osd_clearbitmap() to appropriately initialize */
/* it to the background color. */
/* VERY IMPORTANT: the function must allocate also a "safety area" 16 pixels wide all */
/* around the bitmap. This is required because, for performance reasons, some graphic */
/* routines don't clip at boundaries of the bitmap. */

const int safety = 16;


struct osd_bitmap *osd_alloc_bitmap(int width,int height,int depth)
{
	struct osd_bitmap *bitmap;


	if ((bitmap = (struct osd_bitmap *)malloc(sizeof(struct osd_bitmap))) != 0)
	{
		int i,rowlen,rdwidth;
		unsigned char *bm;


		if (depth != 8 && depth != 16) depth = 8;

		bitmap->depth = depth;
		bitmap->width = width;
		bitmap->height = height;

		rdwidth = (width + 7) & ~7;     /* round width to a quadword */
		if (depth == 16)
			rowlen = 2 * (rdwidth + 2 * safety) * sizeof(unsigned char);
		else
			rowlen =     (rdwidth + 2 * safety) * sizeof(unsigned char);

		if ((bm = (unsigned char*)malloc((height + 2 * safety) * rowlen)) == 0)
		{
			free(bitmap);
			return 0;
		}

		/* clear ALL bitmap, including safety area, to avoid garbage on right */
		/* side of screen is width is not a multiple of 4 */
		memset(bm,0,(height + 2 * safety) * rowlen);

		if ((bitmap->line = (unsigned char**)malloc((height + 2 * safety) * sizeof(unsigned char *))) == 0)
		{
			free(bm);
			free(bitmap);
			return 0;
		}

		for (i = 0;i < height + 2 * safety;i++)
		{
			if (depth == 16)
				bitmap->line[i] = &bm[i * rowlen + 2*safety];
			else
				bitmap->line[i] = &bm[i * rowlen + safety];
		}
		bitmap->line += safety;

		bitmap->_private = bm;

		osd_clearbitmap(bitmap);
	}

	return bitmap;
}



/* set the bitmap to black */
void osd_clearbitmap(struct osd_bitmap *bitmap)
{
	int i;


	for (i = 0;i < bitmap->height;i++)
	{
		if (bitmap->depth == 16)
			memset(bitmap->line[i],0,2*bitmap->width);
		else
			memset(bitmap->line[i],0,bitmap->width);
	}

	if (bitmap == Machine->scrbitmap)
	{
		extern int bitmap_dirty;        /* in mame.c */

		osd_mark_dirty (0,0,bitmap->width-1,bitmap->height-1,1);
		bitmap_dirty = 1;
	}
}



void osd_free_bitmap(struct osd_bitmap *bitmap)
{
	if (bitmap)
	{
		bitmap->line -= safety;
		free(bitmap->line);
		free(bitmap->_private);
		free(bitmap);
	}
}


void osd_mark_dirty(int _x1, int _y1, int _x2, int _y2, int ui)
{
	if (use_dirty)
	{
		int x, y;

//        logerror("mark_dirty %3d,%3d - %3d,%3d\n", _x1,_y1, _x2,_y2);

		_x1 -= skipcolumns;
		_x2 -= skipcolumns;
		_y1 -= skiplines;
		_y2 -= skiplines;

	if (_y1 >= gfx_display_lines || _y2 < 0 || _x1 > gfx_display_columns || _x2 < 0) return;
		if (_y1 < 0) _y1 = 0;
		if (_y2 >= gfx_display_lines) _y2 = gfx_display_lines - 1;
		if (_x1 < 0) _x1 = 0;
		if (_x2 >= gfx_display_columns) _x2 = gfx_display_columns - 1;

		for (y = _y1; y <= _y2 + 15; y += 16)
			for (x = _x1; x <= _x2 + 15; x += 16)
				MARKDIRTY(x,y);
	}
}

static void init_dirty(char dirty)
{
	memset(dirty_new, dirty, MAX_GFX_WIDTH/16 * MAX_GFX_HEIGHT/16);
}

static INLINE void swap_dirty(void)
{
   char *tmp = dirty_old;
	dirty_old = dirty_new;
	dirty_new = tmp;
}

/*
 * This function tries to find the best display mode.
 */
static void select_display_mode(int width,int height,int depth,int attributes,int orientation)
{
	/* 16 bit color is supported only by VESA modes */
	if (depth == 16 || depth == 32)
	{
		logerror("Game needs %d-bit colors.\n",depth);
	}

	emulated_width = width;
	emulated_height = height;


	if (!gfx_width && !gfx_height)//no aspect ratio
	{
		gfx_width = width;
		gfx_height = height;
	}

	if(iOS_fixedRes == 1)
	{
		gfx_width = 320;
		gfx_height = 240;
		emulated_width = 320;
		emulated_height = 240;
	}
	else if(iOS_fixedRes == 2)
	{
		gfx_width = 240;
		gfx_height = 320;
		emulated_width = 240;
		emulated_height = 320;
	}else if(iOS_fixedRes == 3)
	{
		gfx_width = 640;
		gfx_height = 480;
		emulated_width = 640;
		emulated_height = 480;
	}else if(iOS_fixedRes == 4)
	{
		gfx_width = 480;
		gfx_height = 640;
		emulated_width = 480;
		emulated_height = 640;
	}


	if(iOS_cropVideo)
	{

		gfx_width = width;
		gfx_height = height;

		int rx = iOS_cropVideo == 1 ? 4 : 3;
		int ry = iOS_cropVideo == 1 ? 3 : 4;


		//double ratio = 4.0/3.0;
		//printf("%d %d \n",width,height);

		int new_width = //gfx_height * ratio;
		            ((((gfx_height*rx)/ry)+7)&~7);

		if(new_width>gfx_width)
		{
			gfx_height = //gfx_width / ratio;
					((((gfx_width*ry)/rx)+7)&~7);
		}
		else
 		    gfx_width = new_width;

		emulated_width = gfx_width;
		emulated_height = gfx_height;

		//printf("%d %d\n",gfx_width,gfx_height);
	}

/*
	if(iOS_aspectRatio)//aspect ratio
	{

		gfx_width = width;
		gfx_height = height;

		//double ratio = 4.0/3.0;//isIpad ? 1024.0/768.0 :480.0/320.0;

		//printf("%d %d %f\n",width,height,ratio);

		int done = 0;

		iOS_43 = width > height;

		int rx = iOS_43 ? 4 : 3;
		int ry = iOS_43 ? 3 : 4;

		// Try adjusting width to be proportional to height
		int newWidth = //(int) (ratio * gfx_height);
				((((gfx_height*rx)/ry)+7)&~7);

		if (newWidth >= gfx_width) {
			gfx_width = newWidth;
			done = 1;
		}

		// Try adjusting height to be proportional to width
		if (!done) {
			int newHeight = //(int) (gfx_width / ratio);
					((((gfx_width*ry)/rx)+7)&~7);

			if (newHeight >= gfx_height) {
				gfx_height = newHeight;
			}
		}

		//printf("%d %d\n",gfx_width,gfx_height);
	}
*/
	/* Video hardware scaling */
	if (video_scale)
	{
		gfx_width=width;
		gfx_height=height;
	}

	/* vector games use 640x480 as default */
	if (vector_game && !iOS_fixedRes)
	{
		if(safe_render_path)
		{
		   gfx_width = 640;
		   gfx_height = 480;
		   emulated_width = 640;
		   emulated_height = 480;
		}
		else
		{
		   gfx_width = 320;
		   gfx_height = 240;
		   emulated_width = 320;
		   emulated_height = 240;
		}
	}

	gp2x_set_video_mode(16,gfx_width,gfx_height);
}



/* center image inside the display based on the visual area */
void osd_set_visible_area(int min_x,int max_x,int min_y,int max_y)
{
	int act_width;

logerror("set visible area %d-%d %d-%d\n",min_x,max_x,min_y,max_y);

		act_width = gfx_width;

	viswidth  = max_x - min_x + 1;
	visheight = max_y - min_y + 1;

	/* setup xmultiply to handle SVGA driver's (possible) double width */
	xmultiply = 1;
	ymultiply = 1;

	gfx_display_lines = visheight;
	gfx_display_columns = viswidth;

	gfx_xoffset = (act_width - viswidth * xmultiply) / 2;
	if (gfx_display_columns > act_width / xmultiply)
		gfx_display_columns = act_width / xmultiply;

	gfx_yoffset = (gfx_height - visheight * ymultiply) / 2;
		if (gfx_display_lines > gfx_height / ymultiply)
			gfx_display_lines = gfx_height / ymultiply;

	skiplinesmin = min_y;
	skiplinesmax = visheight - gfx_display_lines + min_y;
	skipcolumnsmin = min_x;
	skipcolumnsmax = viswidth - gfx_display_columns + min_x;

	/* Align on a quadword !*/
	gfx_xoffset &= ~7;

	/* the skipcolumns from mame.cfg/cmdline is relative to the visible area */
	skipcolumns = min_x + skipcolumns;
	skiplines   = min_y + skiplines;

	/* Just in case the visual area doesn't fit */
	if (gfx_xoffset < 0)
	{
		skipcolumns -= gfx_xoffset;
		gfx_xoffset = 0;
	}
	if (gfx_yoffset < 0)
	{
		skiplines   -= gfx_yoffset;
		gfx_yoffset = 0;
	}

	/* Failsafe against silly parameters */
	if (skiplines < skiplinesmin)
		skiplines = skiplinesmin;
	if (skipcolumns < skipcolumnsmin)
		skipcolumns = skipcolumnsmin;
	if (skiplines > skiplinesmax)
		skiplines = skiplinesmax;
	if (skipcolumns > skipcolumnsmax)
		skipcolumns = skipcolumnsmax;

	logerror("gfx_width = %d gfx_height = %d\n"
				"gfx_xoffset = %d gfx_yoffset = %d\n"
				"xmin %d ymin %d xmax %d ymax %d\n"
				"skiplines %d skipcolumns %d\n"
				"gfx_display_lines %d gfx_display_columns %d\n"
				"xmultiply %d ymultiply %d\n",
				gfx_width,gfx_height,
				gfx_xoffset,gfx_yoffset,
				min_x, min_y, max_x, max_y, skiplines, skipcolumns,gfx_display_lines,gfx_display_columns,xmultiply,ymultiply);

	set_ui_visarea(skipcolumns, skiplines, skipcolumns+gfx_display_columns-1, skiplines+gfx_display_lines-1);

	/* round to a multiple of 4 to avoid missing pixels on the right side */
	gfx_display_columns  = (gfx_display_columns + 3) & ~3;
}



/*
Create a display screen, or window, of the given dimensions (or larger).
Attributes are the ones defined in driver.h.
Returns 0 on success.
*/
int osd_create_display(int width,int height,int depth,int fps,int attributes,int orientation)
{
	printf("width %d, height %d\n", width,height);

	video_depth = depth;
	video_fps = fps;
	brightness = 100;
	brightness_paused_adjust = 1.0;
	dirty_bright = 1;

	/* Look if this is a vector game */
	if (attributes & VIDEO_TYPE_VECTOR)
		vector_game = 1;
	else
		vector_game = 0;


	if (use_dirty == -1)	/* dirty=auto in mame.cfg? */
	{
		/* Is the game using a dirty system? */
		if ((attributes & VIDEO_SUPPORTS_DIRTY) || vector_game)
			use_dirty = 1;
		else
			use_dirty = 0;
	}

	select_display_mode(width,height,depth,attributes,orientation);

	if (!osd_set_display(width,height,depth,attributes,orientation))
		return 1;

	/* set visible area to nothing just to initialize it - it will be set by the core */
	osd_set_visible_area(0,0,0,0);

    return 0;
}

/* set the actual display screen but don't allocate the screen bitmap */
int osd_set_display(int width,int height,int depth,int attributes,int orientation)
{
	int     i;

	if (!gfx_height || !gfx_width)
	{
		printf("Please specify height AND width (e.g. -640x480)\n");
		return 0;
	}

	/* Mark the dirty buffers as dirty */

	if (use_dirty)
	{
		if (vector_game)
			/* vector games only use one dirty buffer */
			init_dirty (0);
		else
			init_dirty(1);
		swap_dirty();
		init_dirty(1);
	}
	if (dirtycolor)
	{
		for (i = 0;i < screen_colors;i++)
			dirtycolor[i] = 1;
		dirtypalette = 1;
	}

	/* Set video mode */
	gp2x_set_video_mode(depth,gfx_width,gfx_height);

	vsync_frame_rate = video_fps;

	return 1;
}

/* shut up the display */
void osd_close_display(void)
{
	free(dirtycolor);
	dirtycolor = 0;
	free(current_palette);
	current_palette = 0;
	free(palette_16bit_lookup);
	palette_16bit_lookup = 0;
}

int osd_allocate_colors(unsigned int totalcolors,const unsigned char *palette,unsigned short *pens,int modifiable)
{
	int i;

	modifiable_palette = modifiable;
	screen_colors = totalcolors;
	if (video_depth != 8)
		screen_colors += 2;
	else screen_colors = 256;

	dirtycolor = (unsigned int*)malloc(screen_colors * sizeof(int));
	current_palette = (unsigned char*)malloc(3 * screen_colors * sizeof(unsigned char));
	palette_16bit_lookup = (uint32_t*)malloc(screen_colors * sizeof(palette_16bit_lookup[0]));
	if (dirtycolor == 0 || current_palette == 0 || palette_16bit_lookup == 0)
		return 1;

	for (i = 0;i < screen_colors;i++)
		dirtycolor[i] = 1;
	dirtypalette = 1;
	for (i = 0;i < screen_colors;i++)
		current_palette[3*i+0] = current_palette[3*i+1] = current_palette[3*i+2] = 0;

	if (video_depth != 8 && modifiable == 0)
	{
		int r,g,b;


		for (i = 0;i < totalcolors;i++)
		{
			r = 255 * brightness * pow(palette[3*i+0] / 255.0, 1 / osd_gamma_correction) / 100;
			g = 255 * brightness * pow(palette[3*i+1] / 255.0, 1 / osd_gamma_correction) / 100;
			b = 255 * brightness * pow(palette[3*i+2] / 255.0, 1 / osd_gamma_correction) / 100;
			*pens++ = makecol(r,g,b);
		}

		Machine->uifont->colortable[0] = makecol(0x00,0x00,0x00);
		Machine->uifont->colortable[1] = makecol(0xff,0xff,0xff);
		Machine->uifont->colortable[2] = makecol(0xff,0xff,0xff);
		Machine->uifont->colortable[3] = makecol(0x00,0x00,0x00);
	}
	else
	{
		if (video_depth == 8 && totalcolors >= 255)
		{
			int bestblack,bestwhite;
			int bestblackscore,bestwhitescore;


			bestblack = bestwhite = 0;
			bestblackscore = 3*255*255;
			bestwhitescore = 0;
			for (i = 0;i < totalcolors;i++)
			{
				int r,g,b,score;

				r = palette[3*i+0];
				g = palette[3*i+1];
				b = palette[3*i+2];
				score = r*r + g*g + b*b;

				if (score < bestblackscore)
				{
					bestblack = i;
					bestblackscore = score;
				}
				if (score > bestwhitescore)
				{
					bestwhite = i;
					bestwhitescore = score;
				}
			}

			for (i = 0;i < totalcolors;i++)
				pens[i] = i;

			/* map black to pen 0, otherwise the screen border will not be black */
			pens[bestblack] = 0;
			pens[0] = bestblack;

			Machine->uifont->colortable[0] = pens[bestblack];
			Machine->uifont->colortable[1] = pens[bestwhite];
			Machine->uifont->colortable[2] = pens[bestwhite];
			Machine->uifont->colortable[3] = pens[bestblack];
		}
		else
		{
			/* reserve color 1 for the user interface text */
			current_palette[3*1+0] = current_palette[3*1+1] = current_palette[3*1+2] = 0xff;
			Machine->uifont->colortable[0] = 0;
			Machine->uifont->colortable[1] = 1;
			Machine->uifont->colortable[2] = 1;
			Machine->uifont->colortable[3] = 0;

			/* fill the palette starting from the end, so we mess up badly written */
			/* drivers which don't go through Machine->pens[] */
			for (i = 0;i < totalcolors;i++)
				pens[i] = (screen_colors-1)-i;
		}

		for (i = 0;i < totalcolors;i++)
		{
			current_palette[3*pens[i]+0] = palette[3*i];
			current_palette[3*pens[i]+1] = palette[3*i+1];
			current_palette[3*pens[i]+2] = palette[3*i+2];
		}
	}

	if (video_depth == 16)
	{
		if (modifiable_palette)
		{
			if (use_dirty)
			{
				update_screen = blitscreen_dirty1_palettized16;
				logerror("blitscreen_dirty1_palettized16\n");
			}
			else
			{
				update_screen = blitscreen_dirty0_palettized16;
				logerror("blitscreen_dirty0_palettized16\n");
			}
		}
		else
		{
			if (use_dirty)
			{
				update_screen = blitscreen_dirty1_color16;
				logerror("blitscreen_dirty1_color16\n");
			}
			else
			{
				update_screen = blitscreen_dirty0_color16;
				logerror("blitscreen_dirty0_color16\n");
			}
		}
	}
	else
	{
		if (use_dirty) /* supports dirty ? */
		{
			update_screen = blitscreen_dirty1_color8;
			logerror("blitscreen_dirty1_color8\n");
		}
		else
		{
			update_screen = blitscreen_dirty0_color8;
			logerror("blitscreen_dirty1_color8\n");
		}
	}

	return 0;
}


void osd_modify_pen(int pen,unsigned char red, unsigned char green, unsigned char blue)
{
	if (modifiable_palette == 0)
	{
		logerror("error: osd_modify_pen() called with modifiable_palette == 0\n");
		return;
	}

	if (current_palette[3*pen+0] != red ||
		current_palette[3*pen+1] != green ||
		current_palette[3*pen+2] != blue)
	{
		current_palette[3*pen+0] = red;
		current_palette[3*pen+1] = green;
		current_palette[3*pen+2] = blue;
		dirtycolor[pen] = 1;
		dirtypalette = 1;
	}
}



void osd_get_pen(int pen,unsigned char *red, unsigned char *green, unsigned char *blue)
{
	if (video_depth != 8 && modifiable_palette == 0)
	{
		*red =   getr(pen);
		*green = getg(pen);
		*blue =  getb(pen);
	}
	else
	{
		*red =   current_palette[3*pen+0];
		*green = current_palette[3*pen+1];
		*blue =  current_palette[3*pen+2];
	}
}



static void update_screen_dummy(struct osd_bitmap *bitmap)
{
}

static INLINE void pan_display(void)
{
	int pan_changed = 0;

	/* horizontal panning */
	if (input_ui_pressed_repeat(IPT_UI_PAN_LEFT,1))
		if (skipcolumns < skipcolumnsmax)
		{
			skipcolumns++;
			osd_mark_dirty (0,0,Machine->scrbitmap->width-1,Machine->scrbitmap->height-1,1);
			pan_changed = 1;
		}
	if (input_ui_pressed_repeat(IPT_UI_PAN_RIGHT,1))
		if (skipcolumns > skipcolumnsmin)
		{
			skipcolumns--;
			osd_mark_dirty (0,0,Machine->scrbitmap->width-1,Machine->scrbitmap->height-1,1);
			pan_changed = 1;
		}
	if (input_ui_pressed_repeat(IPT_UI_PAN_DOWN,1))
		if (skiplines < skiplinesmax)
		{
			skiplines++;
			osd_mark_dirty (0,0,Machine->scrbitmap->width-1,Machine->scrbitmap->height-1,1);
			pan_changed = 1;
		}
	if (input_ui_pressed_repeat(IPT_UI_PAN_UP,1))
		if (skiplines > skiplinesmin)
		{
			skiplines--;
			osd_mark_dirty (0,0,Machine->scrbitmap->width-1,Machine->scrbitmap->height-1,1);
			pan_changed = 1;
		}

	if (pan_changed)
	{
		if (use_dirty) init_dirty(1);

		set_ui_visarea (skipcolumns, skiplines, skipcolumns+gfx_display_columns-1, skiplines+gfx_display_lines-1);
	}
}

int osd_skip_this_frame(void)
{
   return should_skip_frame;
}

/* Update the display. */
#ifdef MAMEGO
/* mame-go: Neo Geo frames (8-bit pens with the 256-colour gp2x_palette, or
 * 16-bit pens with palette_16bit_lookup) go to the host with that table instead of
 * through update_screen() + video_cb's copy; the host converts them on its
 * second core. The host returns 0 to decline (then the normal path runs).
 * The frame being converted stays untouched: MAME draws the next one into
 * a second bitmap (the Neo Geo redraws the whole screen every frame, so
 * alternating bitmaps is safe). A call with pix == NULL waits for the host. */
int (*mamego_present_indexed)(const void *pix, int bits, int width, int height, int pitch,
                              const void *palette, int colors) = 0;
int mame2000_frame_presented = 0;
static struct osd_bitmap *mamego_bitmaps[2];

/* a frame whose sprites the driver left to the host (Neo Geo, vidhrdw/neogeo.c)
   but that does not go to the host: draw them on this core */
static void mamego_render_now(void)
{
	extern void (*mamego_frame_render)(void);
	void (*render)(void) = mamego_frame_render;
	mamego_frame_render = 0;
	if (render)
	{
		render();
#ifdef NEOPROF
		{ extern volatile int mamego_render_count[3]; mamego_render_count[2]++; }
#endif
	}
}

static int mamego_present_frame(struct osd_bitmap *bitmap)
{
	extern int neogeo_mvs_vh_start(void);
	extern int mamego_set_screen_bitmap(struct osd_bitmap *b);
	struct osd_bitmap *next;

	int bits;

	/* bits 16: pens -> palette_16bit_lookup (uint32_t), bits 8: pens -> gp2x_palette (uint16_t) */
	if (update_screen == blitscreen_dirty0_palettized16)
		bits = 16;
	else if (update_screen == blitscreen_dirty0_color8)
		bits = 8;
	else
		return 0;
	/* Neo Geo only. CPS1 was tried (2026-09-28): the conversion left core 0
	   but its PSRAM traffic slowed the 68000/Z80/video by as much (Final
	   Fight 41 -> 38 fps). */
	if (!mamego_present_indexed || Machine->drv->vh_start != neogeo_mvs_vh_start || bitmap != Machine->scrbitmap)
		return 0;
	if (mamego_bitmaps[0] && bitmap != mamego_bitmaps[0] && bitmap != mamego_bitmaps[1])
		return 0;
	if (!mamego_bitmaps[0])
	{
		mamego_bitmaps[0] = bitmap;
		mamego_bitmaps[1] = osd_alloc_bitmap(bitmap->width, bitmap->height, bitmap->depth);
		if (!mamego_bitmaps[1])
		{
			mamego_present_indexed = 0; /* no memory for it: stay on the normal path */
			return 0;
		}
	}
	if (bits == 16
		? !mamego_present_indexed(((unsigned short *)bitmap->line[skiplines]) + skipcolumns, 16,
			gfx_display_columns, gfx_display_lines, (bitmap->line[1] - bitmap->line[0]) >> 1,
			palette_16bit_lookup, screen_colors)
		: !mamego_present_indexed(bitmap->line[skiplines] + skipcolumns, 8,
			gfx_display_columns, gfx_display_lines, bitmap->line[1] - bitmap->line[0],
			gp2x_palette, 256))
		return 0;
	next = (bitmap == mamego_bitmaps[0]) ? mamego_bitmaps[1] : mamego_bitmaps[0];
	if (!mamego_set_screen_bitmap(next)) /* MAME draws the next frame there */
	{
		mamego_present_indexed(0, 0, 0, 0, 0, 0, 0); /* one bitmap only: let the host finish first */
		return 1;
	}
	{
		static int logged;
		if (!logged++)
			printf("mamego: %dx%d %d-bit frames converted by the host (second core)\n", gfx_display_columns, gfx_display_lines, bits);
	}
	return 1;
}

/* game shutdown: MAME frees whichever bitmap is current, this frees the other */
void mamego_present_shutdown(void)
{
	int i;
	if (mamego_present_indexed)
		mamego_present_indexed(0, 0, 0, 0, 0, 0, 0); /* host: wait until the last frame is converted */
	for (i = 0; i < 2; i++)
		if (mamego_bitmaps[i] && mamego_bitmaps[i] != Machine->scrbitmap)
			osd_free_bitmap(mamego_bitmaps[i]);
	mamego_bitmaps[0] = mamego_bitmaps[1] = 0;
}
#endif

static void osd_update_video_and_audio_(struct osd_bitmap *bitmap);
void osd_update_video_and_audio(struct osd_bitmap *bitmap)
{
	PROF_PUSH(PROF_BLIT);
	osd_update_video_and_audio_(bitmap);
#ifdef MAMEGO
	{
		/* a frame drawn but not shown: nothing to draw its sprites for */
		extern void (*mamego_frame_render)(void);
#ifdef NEOPROF
		if (mamego_frame_render) { extern volatile int mamego_render_count[3]; mamego_render_count[2]++; }
#endif
		mamego_frame_render = 0;
	}
#endif
	PROF_POP();
}
static void osd_update_video_and_audio_(struct osd_bitmap *bitmap)
{
	int i;
	int have_to_clear_bitmap = 0;

	if (bitmap->depth == 8)
	{
		if (dirty_bright)
		{
			dirty_bright = 0;
			for (i = 0;i < 256;i++)
			{
				float rate = brightness * brightness_paused_adjust * pow(i / 255.0, 1 / osd_gamma_correction) / 100;
				bright_lookup[i] = 255 * rate + 0.5;
			}
		}
		if (dirtypalette)
		{
			dirtypalette = 0;
			for (i = 0;i < screen_colors;i++)
			{
				if (dirtycolor[i])
				{
					unsigned char r,g,b;

					dirtycolor[i] = 0;

					r = current_palette[3*i+0];
					g = current_palette[3*i+1];
					b = current_palette[3*i+2];
					if (i != Machine->uifont->colortable[1])	/* don't adjust the user interface text */
					{
						r = bright_lookup[r];
						g = bright_lookup[g];
						b = bright_lookup[b];
					}
					gp2x_video_color8(i,r,g,b);
				}
			}
			gp2x_video_setpalette();
		}
	}
	else
	{
		if (dirty_bright)
		{
			dirty_bright = 0;
			for (i = 0;i < 256;i++)
			{
				float rate = brightness * brightness_paused_adjust * pow(i / 255.0, 1 / osd_gamma_correction) / 100;
				bright_lookup[i] = 255 * rate + 0.5;
			}
		}
		if (dirtypalette)
		{
			if (use_dirty) init_dirty(1);	/* have to redraw the whole screen */

			dirtypalette = 0;
			for (i = 0;i < screen_colors;i++)
			{
				if (dirtycolor[i])
				{
					int r,g,b;

					dirtycolor[i] = 0;

					r = current_palette[3*i+0];
					g = current_palette[3*i+1];
					b = current_palette[3*i+2];
					if (i != Machine->uifont->colortable[1])	/* don't adjust the user interface text */
					{
						r = bright_lookup[r];
						g = bright_lookup[g];
						b = bright_lookup[b];
					}
					palette_16bit_lookup[i] = makecol(r,g,b);
				}
			}
		}
	}

		/* Decide whether to drop the current frame BEFORE doing any
		 * blit-equivalent work, so a skipped frame costs only the
		 * bookkeeping below.  Previously this decision lived at the
		 * bottom of the function (after update_screen had already
		 * run), which meant every frame paid the full blit cost
		 * regardless of whether retro_run was about to discard it
		 * with video_cb(NULL, ...).  The decision is purely a
		 * function of frameskip_type, frameskip_counter and the
		 * libretro audio-buffer hints reported by the frontend;
		 * none of those are touched by update_screen, so moving
		 * the block earlier is equivalent. */
		should_skip_frame = 0;
		if ((frameskip_type > 0) &&
		    retro_audio_buff_active)
		{
			int skip_frame;

			switch (frameskip_type)
			{
			case 1: /* auto */
				skip_frame = retro_audio_buff_underrun;
				break;
			case 2: /* threshold */
				skip_frame = (retro_audio_buff_occupancy < frameskip_threshold);
				break;
			default:
				skip_frame = 0;
				break;
			}

			if (skip_frame)
			{
				if(frameskip_counter < frameskip_interval)
				{
					should_skip_frame = 1;
					frameskip_counter++;
				}
				else
					frameskip_counter = 0;
			}
			else
				frameskip_counter = 0;
		}

#ifdef MAMEGO
		{
			/* The skip decision above is for the NEXT frame (osd_skip_this_frame
			   returns it to MAME before drawing). This one was drawn with its
			   sprites left to the host (Neo Geo): show it, or the host never
			   draws them and the core would have to (seen on the board: every
			   drawn frame went that way, sprites back on core 0). */
			extern void (*mamego_frame_render)(void);
			static int noforce = -1;
#ifdef ESP_PLATFORM
			if (noforce < 0) { FILE *f = fopen("/sd/retro-go/mame/neo_noforce", "r"); noforce = f != NULL; if (f) fclose(f); }
#else
			noforce = 0;
#endif
			if (!noforce && should_skip_frame && mamego_frame_render && mamego_present_frame(bitmap))
				mame2000_frame_presented = 1;
		}
#endif
		if (should_skip_frame)
		{
			/* Frame will be dropped: retro_run sees should_skip_frame
			 * first and calls video_cb(NULL, ...), so neither the
			 * blit's output (gp2x_screen15) nor the bitmap-direct
			 * pointer is read.  Skip both to save a full memcpy or
			 * pointer probe.  Zero the direct-frame state for
			 * hygiene; the next non-skipped frame sets it afresh. */
			mame2000_direct_frame_data  = 0;
		}
		/* Decide whether this frame can take the bitmap-direct path.
		 * Preconditions reflect the assumption that the existing
		 * color16 blit is now a pure row-by-row memcpy with no
		 * conversion: if the MAME scrbitmap's visible area exactly
		 * matches the libretro frame size and is at offset (0,0),
		 * the memcpy is redundant and we can deliver the bitmap
		 * pointer to the frontend instead.  The bitmap's row stride
		 * is wider than the visible width (osd_alloc_bitmap rounds
		 * width up to a quadword and pads with `safety` pixels on
		 * each side); libretro's video_cb accepts an arbitrary
		 * pitch, so passing the bitmap stride here is correct. */
		else if (   video_depth          == 16
		    && !modifiable_palette
		    && !vector_game
		    && gfx_xoffset          == 0
		    && gfx_yoffset          == 0
		    && skiplines            == 0
		    && skipcolumns          == 0
		    && gfx_display_columns  == gfx_width
		    && gfx_display_lines    == gfx_height)
		{
			mame2000_direct_frame_data  = bitmap->line[0];
			mame2000_direct_frame_pitch = bitmap->line[1] - bitmap->line[0];
		}
		else
		{
			mame2000_direct_frame_data  = 0;
#ifdef MAMEGO
			if (mamego_present_frame(bitmap))
				mame2000_frame_presented = 1;
			else
			{
				mamego_render_now(); /* sprites left for the host: draw them here */
#endif
			/* copy the bitmap to screen memory */
			update_screen(bitmap);
#ifdef MAMEGO
			}
#endif
		}

		if (have_to_clear_bitmap)
			osd_clearbitmap(bitmap);

		if (use_dirty)
		{
			if (!vector_game)
				swap_dirty();
			init_dirty(0);
		}

		if (have_to_clear_bitmap)
			osd_clearbitmap(bitmap);


	/* Check for PGUP, PGDN and pan screen */
	pan_display();

   hook_video_done();
}

void osd_set_gamma(float _gamma)
{
	int i;

	osd_gamma_correction = _gamma;

	for (i = 0;i < screen_colors;i++)
		dirtycolor[i] = 1;
	dirtypalette = 1;
	dirty_bright = 1;
}

float osd_get_gamma(void)
{
	return osd_gamma_correction;
}

/* brightess = percentage 0-100% */
void osd_set_brightness(int _brightness)
{
	int i;

	brightness = _brightness;

	for (i = 0;i < screen_colors;i++)
		dirtycolor[i] = 1;
	dirtypalette = 1;
	dirty_bright = 1;
}

int osd_get_brightness(void)
{
	return brightness;
}

void osd_save_snapshot(struct osd_bitmap *bitmap)
{
	save_screen_snapshot(bitmap);
}

void osd_pause(int paused)
{
	int i;

	if (paused)
		brightness_paused_adjust = 0.65;
	else
		brightness_paused_adjust = 1.0;

	for (i = 0;i < screen_colors;i++)
		dirtycolor[i] = 1;
	dirtypalette = 1;
	dirty_bright = 1;
}

