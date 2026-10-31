/***************************************************************************

  vidhrdw.c

  Functions to emulate the video hardware of the machine.

Important!  There are two types of NeoGeo romdump - MVS & MGD2.  They are both
converted to a standard format in the vh_start routines.


Graphics information:

0x00000 - 0xdfff	: Blocks of sprite data, each 0x80 bytes:
	Each 0x80 block is made up of 0x20 double words, their format is:
	Word: Sprite number (16 bits)
	Byte: Palette number (8 bits)
	Byte: Bit 0: X flip
		  Bit 1: Y flip
		  Bit 2: Automatic animation flag (4 tiles?)
		  Bit 3: Automatic animation flag (8 tiles?)
		  Bit 4: MSB of sprite number (confirmed, Karnov_r, Mslug). See note.
		  Bit 5: MSB of sprite number (MSlug2)
		  Bit 6: MSB of sprite number (Kof97)
		  Bit 7: Unknown for now

	Each double word sprite is drawn directly underneath the previous one,
	based on the starting coordinates.

0x0e000 - 0x0ea00	: Front plane fix tiles (8*8), 2 bytes each

0x10000: Control for sprites banks, arranged in words

	Bit 0 to 3 - Y zoom LSB
	Bit 4 to 7 - Y zoom MSB (ie, 1 byte for Y zoom).
	Bit 8 to 11 - X zoom, 0xf is full size (no scale).
	Bit 12 to 15 - Unknown, probably unused

0x10400: Control for sprite banks, arranged in words

	Bit 0 to 5: Number of sprites in this bank (see note below).
	Bit 6 - If set, this bank is placed to right of previous bank (same Y-coord).
	Bit 7 to 15 - Y position for sprite bank.

0x10800: Control for sprite banks, arranged in words
	Bit 0 to 5: Unknown
	Bit 7 to 15 - X position for sprite bank.

Notes:

* If rom set has less than 0x10000 tiles then msb of tile must be ignored
(see Magician Lord).

***************************************************************************/

#include "driver.h"
#include "common.h"
#include "usrintrf.h"
#include "vidhrdw/generic.h"

//#define NEO_DEBUG

static unsigned char *vidram;
static unsigned char *neogeo_paletteram;	   /* pointer to 1 of the 2 palette banks */
static unsigned char *pal_bank1;		/* 0x100*16 2 byte palette entries */
static unsigned char *pal_bank2;		/* 0x100*16 2 byte palette entries */
static int palno,modulo,where,high_tile,vhigh_tile,vvhigh_tile;
int no_of_tiles;
static int palette_swap_pending,fix_bank;

extern unsigned char *neogeo_ram;
extern unsigned int neogeo_frame_counter;
extern int neogeo_game_fix;

void NeoMVSDrawGfx(unsigned char **line,const struct GfxElement *gfx,
		unsigned int code,unsigned int color,int flipx,int flipy,int sx,int sy,
		int zx,int zy,const struct rectangle *clip);

static char dda_x_skip[16];
static char dda_y_skip[17];
static char full_y_skip[16]={0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

#ifdef NEO_DEBUG

int dotiles = 0;
int screen_offs = 0x0000;
int screen_yoffs = 0;

#endif



/******************************************************************************/

#ifdef MAMEGO
/* mame-go: sprite tiles paged from the SD card, see mamego_neospr.c */
int neospr_active(void);
int neospr_start(uint32_t *pen_usage, unsigned total_tiles);
uint32_t *neospr_tile(int tileno);
void neospr_frame(void);
void neospr_stop(void);
#endif

#ifdef MAMEGO
void (*mamego_frame_render)(void);
extern int (*mamego_present_indexed)(const void *pix, int bits, int width, int height, int pitch,
                                     const void *palette, int colors);
static int render_defer = -1;
static unsigned char *vidram_r;          /* the renderer's copy of the video RAM */
#endif

void neogeo_vh_stop(void)
{
#ifdef MAMEGO
	{ extern void neosnd_disable(void); neosnd_disable(); } /* core 1 done with the sound board */
	if (mamego_frame_render || render_defer > 0)
	{
		/* the host may still be drawing from vidram_r and the page cache */
		mamego_frame_render = 0;
		if (mamego_present_indexed)
			mamego_present_indexed(0, 0, 0, 0, 0, 0, 0);
	}
	{ extern unsigned int m68ki_idle_enable; extern unsigned z80_idle_enable; m68ki_idle_enable = 0; z80_idle_enable = 0; }
	neospr_stop();
	{ extern void neosnd_stop(void); neosnd_stop(); }
#endif
   	if (pal_bank1) free(pal_bank1);
	if (pal_bank2) free(pal_bank2);
	if (vidram) free(vidram);
	if (neogeo_ram) free(neogeo_ram);
#ifdef MAMEGO
	if (vidram_r) free(vidram_r);
	vidram_r = 0;
	render_defer = -1;
	mamego_frame_render = 0;
#endif

	pal_bank1=pal_bank2=vidram=neogeo_ram=0;
}

static int common_vh_start(void)
{
	pal_bank1=pal_bank2=vidram=0;

	pal_bank1 = (unsigned char*)malloc(0x2000);
	if (!pal_bank1) {
		neogeo_vh_stop();
		return 1;
	}

	pal_bank2 = (unsigned char*)malloc(0x2000);
	if (!pal_bank2) {
		neogeo_vh_stop();
		return 1;
	}

	vidram = (unsigned char*)malloc(0x20000); /* 0x20000 bytes even though only 0x10c00 is used */
	if (!vidram) {
		neogeo_vh_stop();
		return 1;
	}
	memset(vidram,0,0x20000);

	neogeo_paletteram = pal_bank1;
	palno=0;
	modulo=1;
	where=0;
	fix_bank=0;
	palette_swap_pending=0;

	return 0;
}

static int get_num_tiles (void)
{
    return ((memory_region_length(REGION_GFX2)/128)+(memory_region_length(REGION_GFX3)/128));
}

static uint32_t *get_tile (int tileno)
{
    uint32_t *gfxdata=NULL;

#ifdef MAMEGO
    if (neospr_active())
        return neospr_tile(tileno);
#endif
    if (memory_region_length(REGION_GFX3)>0)
    {
        unsigned int tiles_gfx2=memory_region_length(REGION_GFX2)>>7;
        if (tileno>=tiles_gfx2)
        {
    	    gfxdata = (uint32_t *)&memory_region(REGION_GFX3)[(tileno-tiles_gfx2)<<7];
        }
        else
        {
    	    gfxdata = (uint32_t *)&memory_region(REGION_GFX2)[tileno<<7];
        }
    }
    else
    {
	    gfxdata = (uint32_t *)&memory_region(REGION_GFX2)[tileno<<7];
    }
    
    return gfxdata;
}

static void decodetile(int tileno)
{
	unsigned char swap[128];
	uint32_t *gfxdata;
	int x,y;
	unsigned int pen;

    gfxdata=get_tile (tileno);

	memcpy(swap,gfxdata,128);

	for (y = 0;y < 16;y++)
	{
		uint32_t dw;

		dw = 0;
		for (x = 0;x < 8;x++)
		{
			pen  = ((swap[64 + 4*y + 3] >> x) & 1) << 3;
			pen |= ((swap[64 + 4*y + 1] >> x) & 1) << 2;
			pen |= ((swap[64 + 4*y + 2] >> x) & 1) << 1;
			pen |=  (swap[64 + 4*y    ] >> x) & 1;
			dw |= pen << 4*(7-x);
			Machine->gfx[2]->pen_usage[tileno] |= (1 << pen);
		}
		*(gfxdata++) = dw;

		dw = 0;
		for (x = 0;x < 8;x++)
		{
			pen  = ((swap[4*y + 3] >> x) & 1) << 3;
			pen |= ((swap[4*y + 1] >> x) & 1) << 2;
			pen |= ((swap[4*y + 2] >> x) & 1) << 1;
			pen |=  (swap[4*y    ] >> x) & 1;
			dw |= pen << 4*(7-x);
			Machine->gfx[2]->pen_usage[tileno] |= (1 << pen);
		}
		*(gfxdata++) = dw;
	}
}

int neogeo_mvs_vh_start(void)
{
	no_of_tiles=get_num_tiles();
	if (no_of_tiles>0x10000) high_tile=1; else high_tile=0;
	if (no_of_tiles>0x20000) vhigh_tile=1; else vhigh_tile=0;
	if (no_of_tiles>0x40000) vvhigh_tile=1; else vvhigh_tile=0;
	Machine->gfx[2]->total_elements = no_of_tiles;
	if (Machine->gfx[2]->pen_usage)
		free(Machine->gfx[2]->pen_usage);
	Machine->gfx[2]->pen_usage = (unsigned int*)malloc(no_of_tiles * sizeof(int));
	memset(Machine->gfx[2]->pen_usage,0,no_of_tiles * sizeof(int));
#ifdef MAMEGO
	/* paged sprites come decoded, with pen_usage precomputed */
	if (neospr_active() && neospr_start((uint32_t *)Machine->gfx[2]->pen_usage, no_of_tiles) != 0)
		return 1;
#endif

	/* tiles are not decoded yet. They will be decoded later as they are used. */
	/* pen_usage is used as a marker of decoded tiles: if it is 0, then the tile */
	/* hasn't been decoded yet. */

	return common_vh_start();
}

/******************************************************************************/

static void swap_palettes(void)
{
	int i,newword;

	for (i=0; i<0x2000; i+=2)
	{
		int r,g,b;


	   	newword = READ_WORD(&neogeo_paletteram[i]);

		r = ((newword >> 7) & 0x1e) | ((newword >> 14) & 0x01);
		g = ((newword >> 3) & 0x1e) | ((newword >> 13) & 0x01) ;
		b = ((newword << 1) & 0x1e) | ((newword >> 12) & 0x01) ;

		r = (r << 3) | (r >> 2);
		g = (g << 3) | (g >> 2);
		b = (b << 3) | (b >> 2);

		palette_change_color(i / 2,r,g,b);
	}

	palette_swap_pending=0;
}

WRITE_HANDLER( neogeo_setpalbank0_w )
{
	if (palno != 0) {
		palno = 0;
		neogeo_paletteram = pal_bank1;
		palette_swap_pending=1;
	}
}

WRITE_HANDLER( neogeo_setpalbank1_w )
{
	if (palno != 1) {
		palno = 1;
		neogeo_paletteram = pal_bank2;
		palette_swap_pending=1;
	}
}

READ_HANDLER( neogeo_paletteram_r )
{
	return READ_WORD(&neogeo_paletteram[offset]);
}

WRITE_HANDLER( neogeo_paletteram_w )
{
	int oldword = READ_WORD (&neogeo_paletteram[offset]);
	int newword = COMBINE_WORD (oldword, data);
	int r,g,b;


	WRITE_WORD (&neogeo_paletteram[offset], newword);

	r = ((newword >> 7) & 0x1e) | ((newword >> 14) & 0x01);
	g = ((newword >> 3) & 0x1e) | ((newword >> 13) & 0x01) ;
	b = ((newword << 1) & 0x1e) | ((newword >> 12) & 0x01) ;

	r = (r << 3) | (r >> 2);
	g = (g << 3) | (g >> 2);
	b = (b << 3) | (b >> 2);

	palette_change_color(offset / 2,r,g,b);
}

/******************************************************************************/

static const unsigned char *neogeo_palette(const struct rectangle *clip)
{
	int color,code,pal_base,y,my=0,count,offs,i;
 	int colmask[256];
	unsigned int *pen_usage; /* Save some struct derefs */

	int sx =0,sy =0,oy =0,zx = 1, rzy = 1;
	int tileno,tileatr,t1,t2,t3;
	char fullmode=0;
	int ddax=0,dday=0,rzx=15,yskip=0;

	if (Machine->scrbitmap->depth == 16)
	{
		return palette_recalc();
	}

	palette_init_used_colors();

	/* character foreground */
	pen_usage= Machine->gfx[fix_bank]->pen_usage;
	pal_base = Machine->drv->gfxdecodeinfo[fix_bank].color_codes_start;
	for (color = 0;color < 16;color++) colmask[color] = 0;
	for (offs=0xe000;offs<0xea00;offs+=2) {
		code = READ_WORD( &vidram[offs] );
		color = code >> 12;
		colmask[color] |= pen_usage[code&0xfff];
	}
	for (color = 0;color < 16;color++)
	{
		for (i = 1;i < 16;i++)
		{
			if (colmask[color] & (1 << i))
				palette_used_colors[pal_base + 16 * color + i] = PALETTE_COLOR_VISIBLE;
		}
	}

	/* Tiles */
	pen_usage= Machine->gfx[2]->pen_usage;
	pal_base = Machine->drv->gfxdecodeinfo[2].color_codes_start;
	for (color = 0;color < 256;color++) colmask[color] = 0;
	for (count=0;count<0x300;count+=2) {
		t3 = READ_WORD( &vidram[0x10000 + count] );
		t1 = READ_WORD( &vidram[0x10400 + count] );
		t2 = READ_WORD( &vidram[0x10800 + count] );

		/* If this bit is set this new column is placed next to last one */
		if (t1 & 0x40) {
			sx += rzx;
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			/* Get new zoom for this column */
			zx = (t3 >> 8) & 0x0f;
			sy = oy;
		} else {	/* nope it is a new block */
			/* Sprite scaling */
			zx = (t3 >> 8) & 0x0f;
			rzy = t3 & 0xff;

			sx = (t2 >> 7);
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			/* Number of tiles in this strip */
			my = t1 & 0x3f;
			if (my == 0x20) fullmode = 1;
			else if (my >= 0x21) fullmode = 2;	/* most games use 0x21, but */
												/* Alpha Mission II uses 0x3f */
			else fullmode = 0;

			sy = 0x200 - (t1 >> 7);
			if (clip->max_y - clip->min_y > 8 ||	/* kludge to improve the ssideki games */
					clip->min_y == Machine->visible_area.min_y)
			{
				if (sy > 0x110) sy -= 0x200;
				if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
				{
					while (sy < 0) sy += 2 * (rzy + 1);
				}
			}
			oy = sy;

			if (rzy < 0xff && my < 0x10 && my)
			{
				my = (my*256)/(rzy+1);
				if (my > 0x10) my = 0x10;
			}
			if (my > 0x20) my=0x20;

			ddax=0;	/* =16; NS990110 neodrift fix */		/* setup x zoom */
		}

		/* No point doing anything if tile strip is 0 */
		if (my==0) continue;

		/* Process x zoom */
		if(zx!=15) {
			rzx=0;
			for(i=0;i<16;i++) {
				ddax-=zx+1;
				if(ddax<=0) {
					ddax+=15+1;
					rzx++;
				}
			}
		}
		else rzx=16;

		if(sx>=320) continue;

		/* Setup y zoom */
		if(rzy==255)
			yskip=16;
		else
			dday=0;	/* =256; NS990105 mslug fix */

		offs = count<<6;

		/* my holds the number of tiles in each vertical multisprite block */
		for (y=0; y < my ;y++) {
			tileno  = READ_WORD(&vidram[offs]);
			offs+=2;
			tileatr = READ_WORD(&vidram[offs]);
			offs+=2;

			if (high_tile && tileatr&0x10) tileno+=0x10000;
			if (vhigh_tile && tileatr&0x20) tileno+=0x20000;
			if (vvhigh_tile && tileatr&0x40) tileno+=0x40000;

			if (tileatr&0x8) tileno=(tileno&~7)+((tileno+neogeo_frame_counter)&7);
			else if (tileatr&0x4) tileno=(tileno&~3)+((tileno+neogeo_frame_counter)&3);

			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
			{
				if (sy >= 248) sy -= 2 * (rzy + 1);
			}
			else if (fullmode == 1)
			{
				if (y == 0x10) sy -= 2 * (rzy + 1);
			}
			else if (sy > 0x110) sy -= 0x200;	/* NS990105 mslug2 fix */

			if(rzy!=255) {
				yskip=0;
				for(i=0;i<16;i++) {
					dday-=rzy+1;
					if(dday<=0) {
						dday+=256;
						yskip++;
					}
				}
			}

			if (sy+yskip-1 >= clip->min_y && sy <= clip->max_y)
			{
				tileatr=tileatr>>8;
				tileno %= no_of_tiles;
				if (pen_usage[tileno] == 0)	/* decode tile if it hasn't been yet */
					decodetile(tileno);
				colmask[tileatr] |= pen_usage[tileno];
			}

			sy +=yskip;

		}  /* for y */
	}  /* for count */

	for (color = 0;color < 256;color++)
	{
		for (i = 1;i < 16;i++)
		{
			if (colmask[color] & (1 << i))
				palette_used_colors[pal_base + 16 * color + i] = PALETTE_COLOR_VISIBLE;
		}
	}

	palette_used_colors[4095] = PALETTE_COLOR_VISIBLE;

	return palette_recalc();
}

/******************************************************************************/

WRITE_HANDLER( vidram_offset_w )
{
	where = data;
}

READ_HANDLER( vidram_data_r )
{
	return (READ_WORD(&vidram[where << 1]));
}

#ifdef MAMEGO
/* Sprites drawn on core 1 (see neogeo_vh_screenrefresh): the renderer reads a
   copy of the video RAM taken at the end of the frame, while core 0 already
   runs the next one. Only the 256-byte blocks written during the frame are
   copied (a few hundred words a frame against 67 KB). */
#define VRAM_USED  0x10c00
static uint32_t vram_dirty[(VRAM_USED / 256 + 31) / 32];
#define VRAM_TOUCH(off) do { unsigned _o = (off); if (_o < VRAM_USED) vram_dirty[_o >> 13] |= 1u << ((_o >> 8) & 31); } while (0)
void neogeo_vram_all_dirty(void) { memset(vram_dirty, 0xff, sizeof(vram_dirty)); }
static void vram_sync(void)
{
	unsigned w, b;
	for (w = 0; w < sizeof(vram_dirty) / 4; w++)
		while (vram_dirty[w])
		{
			b = __builtin_ctz(vram_dirty[w]);
			vram_dirty[w] &= vram_dirty[w] - 1;
			if ((w * 32 + b) * 256 < VRAM_USED)
				memcpy(vidram_r + (w * 32 + b) * 256, vidram + (w * 32 + b) * 256, 256);
		}
}
#else
#define VRAM_TOUCH(off) ((void)0)
#endif

WRITE_HANDLER( vidram_data_w )
{
	WRITE_WORD(&vidram[where << 1],data);
	VRAM_TOUCH(where << 1);
	where = (where + modulo) & 0xffff;
}

/* Modulo can become negative , Puzzle Bobble Super Sidekicks and a lot */
/* of other games use this */
WRITE_HANDLER( vidram_modulo_w )
{
	modulo = data;
}

READ_HANDLER( vidram_modulo_r )
{
	return modulo;
}


/* Two routines to enable videoram to be read in debugger */
READ_HANDLER( mish_vid_r )
{
	return READ_WORD(&vidram[offset]);
}

WRITE_HANDLER( mish_vid_w )
{
	COMBINE_WORD_MEM(&vidram[offset],data);
	VRAM_TOUCH(offset);
}

WRITE_HANDLER( neo_board_fix_w )
{
	fix_bank=1;
}

WRITE_HANDLER( neo_game_fix_w )
{
	fix_bank=0;
}

/******************************************************************************/


void NeoMVSDrawGfx(unsigned char **line,const struct GfxElement *gfx, /* AJP */
		unsigned int code,unsigned int color,int flipx,int flipy,int sx,int sy,
		int zx,int zy,const struct rectangle *clip)
{
	int /*ox,*/oy,ey,y,dy;
	unsigned char *bm;
	int col;
	int l; /* Line skipping counter */

	int mydword;

	uint32_t *fspr;

	char *l_y_skip;


	/* Mish/AJP - Most clipping is done in main loop */
	oy = sy;
  	ey = sy + zy -1; 	/* Clip for size of zoomed object */

	if (sy < clip->min_y) sy = clip->min_y;
	if (ey >= clip->max_y) ey = clip->max_y;
	if (sx <= -16) return;

	/* Safety feature */
	code=code%no_of_tiles;

	if (gfx->pen_usage[code] == 0)	/* decode tile if it hasn't been yet */
		decodetile(code);

	/* Check for total transparency, no need to draw */
	if ((gfx->pen_usage[code] & ~1) == 0)
		return;

   	if(zy==16)
		 l_y_skip=full_y_skip;
	else
		 l_y_skip=dda_y_skip;

    fspr=get_tile(code);

	if (flipy)	/* Y flip */
	{
		dy = -2;
		fspr+=32 - 2 - (sy-oy)*2;
	}
	else		/* normal */
	{
		dy = 2;
		fspr+= (sy-oy)*2;
	}

	{
		const unsigned short *paldata;	/* ASG 980209 */
		paldata = &gfx->colortable[gfx->color_granularity * color];
		if (flipx)	/* X flip */
		{
			l=0;
			if(zx==16)
			{
				for (y = sy;y <= ey;y++)
				{
					bm  = line[y]+sx;

					fspr+=l_y_skip[l]*dy;

					mydword = fspr[1];
					col = (mydword>> 0)&0xf; if (col) bm[ 0] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[ 1] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[ 2] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[ 3] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[ 4] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[ 5] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[ 6] = paldata[col];
					col = (mydword>>28)&0xf; if (col) bm[ 7] = paldata[col];

					mydword = fspr[0];
					col = (mydword>> 0)&0xf; if (col) bm[ 8] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[ 9] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[10] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[11] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[12] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[13] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[14] = paldata[col];
					col = (mydword>>28)&0xf; if (col) bm[15] = paldata[col];

					l++;
				}
			}
			else
			{
				for (y = sy;y <= ey;y++)
				{
					bm  = line[y]+sx;
					fspr+=l_y_skip[l]*dy;

					mydword = fspr[1];
					if (dda_x_skip[ 0]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 1]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 2]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 3]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 4]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 5]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 6]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 7]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }

					mydword = fspr[0];
					if (dda_x_skip[ 8]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 9]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[10]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[11]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[12]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[13]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[14]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[15]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }

					l++;
				}
			}
		}
		else		/* normal */
		{
	  		l=0;
			if(zx==16)
			{
				for (y = sy ;y <= ey;y++)
				{
					bm  = line[y] + sx;
					fspr+=l_y_skip[l]*dy;

					mydword = fspr[0];
					col = (mydword>>28)&0xf; if (col) bm[ 0] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[ 1] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[ 2] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[ 3] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[ 4] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[ 5] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[ 6] = paldata[col];
					col = (mydword>> 0)&0xf; if (col) bm[ 7] = paldata[col];

					mydword = fspr[1];
					col = (mydword>>28)&0xf; if (col) bm[ 8] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[ 9] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[10] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[11] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[12] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[13] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[14] = paldata[col];
					col = (mydword>> 0)&0xf; if (col) bm[15] = paldata[col];

					l++;
				}
			}
			else
			{
				for (y = sy ;y <= ey;y++)
				{
					bm  = line[y] + sx;
					fspr+=l_y_skip[l]*dy;

					mydword = fspr[0];
					if (dda_x_skip[ 0]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 1]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 2]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 3]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 4]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 5]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 6]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 7]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }

					mydword = fspr[1];
					if (dda_x_skip[ 8]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 9]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[10]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[11]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[12]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[13]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[14]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[15]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }

					l++;
				}
			}
		}
	}
}

void NeoMVSDrawGfx16(unsigned char **line,const struct GfxElement *gfx, /* AJP */
		unsigned int code,unsigned int color,int flipx,int flipy,int sx,int sy,
		int zx,int zy,const struct rectangle *clip)
{
	int /*ox,*/oy,ey,y,dy;
	unsigned short *bm;
	int col;
	int l; /* Line skipping counter */

	int mydword;

	uint32_t *fspr;

	char *l_y_skip;


	/* Mish/AJP - Most clipping is done in main loop */
	oy = sy;
  	ey = sy + zy -1; 	/* Clip for size of zoomed object */

	if (sy < clip->min_y) sy = clip->min_y;
	if (ey >= clip->max_y) ey = clip->max_y;
	if (sx <= -16) return;

	/* Safety feature */
	code=code%no_of_tiles;

	if (gfx->pen_usage[code] == 0)	/* decode tile if it hasn't been yet */
		decodetile(code);

	/* Check for total transparency, no need to draw */
	if ((gfx->pen_usage[code] & ~1) == 0)
		return;

   	if(zy==16)
		 l_y_skip=full_y_skip;
	else
		 l_y_skip=dda_y_skip;

    fspr=get_tile(code);

	if (flipy)	/* Y flip */
	{
		dy = -2;
		fspr+=32 - 2 - (sy-oy)*2;
	}
	else		/* normal */
	{
		dy = 2;
		fspr+=(sy-oy)*2;
	}

	{
		const unsigned short *paldata;	/* ASG 980209 */
		paldata = &gfx->colortable[gfx->color_granularity * color];
		if (flipx)	/* X flip */
		{
			l=0;
			if(zx==16)
			{
				for (y = sy;y <= ey;y++)
				{
					bm  = ((unsigned short *)line[y])+sx;

					fspr+=l_y_skip[l]*dy;

					mydword = fspr[1];
					col = (mydword>> 0)&0xf; if (col) bm[ 0] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[ 1] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[ 2] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[ 3] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[ 4] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[ 5] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[ 6] = paldata[col];
					col = (mydword>>28)&0xf; if (col) bm[ 7] = paldata[col];

					mydword = fspr[0];
					col = (mydword>> 0)&0xf; if (col) bm[ 8] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[ 9] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[10] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[11] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[12] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[13] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[14] = paldata[col];
					col = (mydword>>28)&0xf; if (col) bm[15] = paldata[col];

					l++;
				}
			}
			else
			{
				for (y = sy;y <= ey;y++)
				{
					bm  = ((unsigned short *)line[y])+sx;
					fspr+=l_y_skip[l]*dy;

					mydword = fspr[1];
					if (dda_x_skip[ 0]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 1]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 2]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 3]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 4]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 5]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 6]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 7]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }

					mydword = fspr[0];
					if (dda_x_skip[ 8]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 9]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[10]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[11]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[12]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[13]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[14]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[15]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }

					l++;
				}
			}
		}
		else		/* normal */
		{
	  		l=0;
			if(zx==16)
			{
				for (y = sy ;y <= ey;y++)
				{
					bm  = ((unsigned short *)line[y]) + sx;
					fspr+=l_y_skip[l]*dy;

					mydword = fspr[0];
					col = (mydword>>28)&0xf; if (col) bm[ 0] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[ 1] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[ 2] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[ 3] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[ 4] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[ 5] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[ 6] = paldata[col];
					col = (mydword>> 0)&0xf; if (col) bm[ 7] = paldata[col];

					mydword = fspr[1];
					col = (mydword>>28)&0xf; if (col) bm[ 8] = paldata[col];
					col = (mydword>>24)&0xf; if (col) bm[ 9] = paldata[col];
					col = (mydword>>20)&0xf; if (col) bm[10] = paldata[col];
					col = (mydword>>16)&0xf; if (col) bm[11] = paldata[col];
					col = (mydword>>12)&0xf; if (col) bm[12] = paldata[col];
					col = (mydword>> 8)&0xf; if (col) bm[13] = paldata[col];
					col = (mydword>> 4)&0xf; if (col) bm[14] = paldata[col];
					col = (mydword>> 0)&0xf; if (col) bm[15] = paldata[col];

					l++;
				}
			}
			else
			{
				for (y = sy ;y <= ey;y++)
				{
					bm  = ((unsigned short *)line[y]) + sx;
					fspr+=l_y_skip[l]*dy;

					mydword = fspr[0];
					if (dda_x_skip[ 0]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 1]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 2]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 3]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 4]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 5]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 6]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 7]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }

					mydword = fspr[1];
					if (dda_x_skip[ 8]) { col = (mydword>>28)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[ 9]) { col = (mydword>>24)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[10]) { col = (mydword>>20)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[11]) { col = (mydword>>16)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[12]) { col = (mydword>>12)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[13]) { col = (mydword>> 8)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[14]) { col = (mydword>> 4)&0xf; if (col) *bm = paldata[col]; bm++; }
					if (dda_x_skip[15]) { col = (mydword>> 0)&0xf; if (col) *bm = paldata[col]; bm++; }

					l++;
				}
			}
		}
	}
}



/******************************************************************************/

/* do_palette: MAME palette bookkeeping (core 0 only: global palette state);
   do_draw: sprites + fix layer into bitmap, from the given video RAM and the
   frame counter / fix bank of that frame (core 1 when deferred) */
static void screenrefresh_(struct osd_bitmap *bitmap,const struct rectangle *clip,int do_palette,int do_draw,
		const unsigned char *vidram,unsigned int neogeo_frame_counter,int fix_bank)
{
	int sx =0,sy =0,oy =0,my =0,zx = 1, rzy = 1;
	int offs,i,count,y,x;
	int tileno,tileatr,t1,t2,t3;
	char fullmode=0;
	int ddax=0,dday=0,rzx=15,yskip=0;
	unsigned char **line=bitmap->line;
	unsigned int *pen_usage;
	struct GfxElement *gfx=Machine->gfx[2]; /* Save constant struct dereference */

	#ifdef NEO_DEBUG
	char buf[80];
	struct DisplayText dt[2];

	/* debug setting, tile view mode connected to '8' */
	if (keyboard_pressed(KEYCODE_8))
	{
		while (keyboard_pressed(KEYCODE_8)) ;
		dotiles ^= 1;
	}

	/* tile view - 0x80, connected to '9' */
	if (keyboard_pressed(KEYCODE_9) && !keyboard_pressed(KEYCODE_LSHIFT))
	{
		if (screen_offs > 0)
			screen_offs -= 0x80;
	}
	if (keyboard_pressed(KEYCODE_9) && keyboard_pressed(KEYCODE_LSHIFT))
	{
		if (screen_yoffs > 0)
			screen_yoffs--;
	}

	/* tile view + 0x80, connected to '0' */
	if (keyboard_pressed(KEYCODE_0) && !keyboard_pressed(KEYCODE_LSHIFT))
	{
		if (screen_offs < 0x10000)
			screen_offs += 0x80;
	}
	if (keyboard_pressed(KEYCODE_0) && keyboard_pressed(KEYCODE_LSHIFT))
	{
		screen_yoffs++;
	}
	#endif

	if (do_palette && (clip->max_y - clip->min_y > 8 ||	/* kludge to speed up raster effects */
			clip->min_y == Machine->visible_area.min_y))
    {
		/* Palette swap occured after last frame but before this one */
		if (palette_swap_pending) swap_palettes();

		/* Do compressed palette stuff */
		neogeo_palette(clip);
		/* no need to check the return code since we redraw everything each frame */
	}
	if (!do_draw)
		return;

	fillbitmap(bitmap,Machine->pens[4095],clip);

#ifdef NEO_DEBUG
if (!dotiles) { 					/* debug */
#endif

	/* Draw sprites */
	for (count=0;count<0x300;count+=2) {
		t3 = READ_WORD( &vidram[0x10000 + count] );
		t1 = READ_WORD( &vidram[0x10400 + count] );
		t2 = READ_WORD( &vidram[0x10800 + count] );

		/* If this bit is set this new column is placed next to last one */
		if (t1 & 0x40) {
			sx += rzx;
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			/* Get new zoom for this column */
			zx = (t3 >> 8) & 0x0f;
			sy = oy;
		} else {	/* nope it is a new block */
			/* Sprite scaling */
			zx = (t3 >> 8) & 0x0f;
			rzy = t3 & 0xff;

			sx = (t2 >> 7);
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			/* Number of tiles in this strip */
			my = t1 & 0x3f;
			if (my == 0x20) fullmode = 1;
			else if (my >= 0x21) fullmode = 2;	/* most games use 0x21, but */
												/* Alpha Mission II uses 0x3f */
			else fullmode = 0;

			sy = 0x200 - (t1 >> 7);
			if (clip->max_y - clip->min_y > 8 ||	/* kludge to improve the ssideki games */
					clip->min_y == Machine->visible_area.min_y)
			{
				if (sy > 0x110) sy -= 0x200;
				if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
				{
					while (sy < 0) sy += 2 * (rzy + 1);
				}
			}
			oy = sy;

			if (rzy < 0xff && my < 0x10 && my)
			{
				my = (my*256)/(rzy+1);
				if (my > 0x10) my = 0x10;
			}
			if (my > 0x20) my=0x20;

			ddax=0;	/* =16; NS990110 neodrift fix */		/* setup x zoom */
		}

		/* No point doing anything if tile strip is 0 */
		if (my==0) continue;

		/* Process x zoom */
		if(zx!=15) {
			rzx=0;
			for(i=0;i<16;i++) {
				ddax-=zx+1;
				if(ddax<=0) {
					ddax+=15+1;
					dda_x_skip[i]=1;
					rzx++;
				}
				else dda_x_skip[i]=0;
			}
		}
		else rzx=16;

		if(sx>=320) continue;

		/* Setup y zoom */
		if(rzy==255)
			yskip=16;
		else
			dday=0;	/* =256; NS990105 mslug fix */

		offs = count<<6;

		/* my holds the number of tiles in each vertical multisprite block */
		for (y=0; y < my ;y++) {
			tileno  = READ_WORD(&vidram[offs]);
			offs+=2;
			tileatr = READ_WORD(&vidram[offs]);
			offs+=2;

			if (high_tile && tileatr&0x10) tileno+=0x10000;
			if (vhigh_tile && tileatr&0x20) tileno+=0x20000;
			if (vvhigh_tile && tileatr&0x40) tileno+=0x40000;

			if (tileatr&0x8) tileno=(tileno&~7)+((tileno+neogeo_frame_counter)&7);
			else if (tileatr&0x4) tileno=(tileno&~3)+((tileno+neogeo_frame_counter)&3);

			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
			{
				if (sy >= 248) sy -= 2 * (rzy + 1);
			}
			else if (fullmode == 1)
			{
				if (y == 0x10) sy -= 2 * (rzy + 1);
			}
			else if (sy > 0x110) sy -= 0x200;	/* NS990105 mslug2 fix */

			if(rzy!=255)
			{
				yskip=0;
				dda_y_skip[0]=0;
				for(i=0;i<16;i++)
				{
					dda_y_skip[i+1]=0;
					dday-=rzy+1;
					if(dday<=0)
					{
						dday+=256;
						yskip++;
						dda_y_skip[yskip]++;
					}
					else dda_y_skip[yskip]++;
				}
			}

			if (sy+15 >= clip->min_y && sy <= clip->max_y)
			{
				if (Machine->scrbitmap->depth == 16)
					NeoMVSDrawGfx16(line,
						gfx,
						tileno,
						tileatr >> 8,
						tileatr & 0x01,tileatr & 0x02,
						sx,sy,rzx,yskip,
						clip
					);
				else
					NeoMVSDrawGfx(line,
						gfx,
						tileno,
						tileatr >> 8,
						tileatr & 0x01,tileatr & 0x02,
						sx,sy,rzx,yskip,
						clip
					);
			}

			sy +=yskip;
		}  /* for y */
	}  /* for count */



	/* Save some struct de-refs */
	gfx=Machine->gfx[fix_bank];
	pen_usage=gfx->pen_usage;

	/* Character foreground */
	for (y=clip->min_y/8; y<=clip->max_y/8; y++) {
		for (x=0; x<40; x++) {

  			int byte1 = (READ_WORD(&vidram[0xE000 + 2*(y + 32*x)]));
  			int byte2 = byte1 >> 12;
		   	byte1 = byte1 & 0xfff;

			if ((pen_usage[byte1] & ~1) == 0) continue;

  			drawgfx(bitmap,gfx,
					byte1,
					byte2,
					0,0,
					x*8,y*8,
					clip,TRANSPARENCY_PEN,0);
  		}
	}



#ifdef NEO_DEBUG
	} else {	/* debug */
		offs = screen_offs;
		for (y=screen_yoffs;y<screen_yoffs+15;y++) {
			for (x=0;x<20;x++) {
				tileno = READ_WORD(&vidram[offs + 4*y+x*128]);
				tileatr = READ_WORD(&vidram[offs + 4*y+x*128+2]);

				if (high_tile && tileatr&0x10) tileno+=0x10000;
				if (vhigh_tile && tileatr&0x20) tileno+=0x20000;
				if (vvhigh_tile && tileatr&0x40) tileno+=0x40000;

				if (tileatr&0x8) tileno=(tileno&~7)+((tileno+neogeo_frame_counter)&7);
				else if (tileatr&0x4) tileno=(tileno&~3)+((tileno+neogeo_frame_counter)&3);

				NeoMVSDrawGfx(line,
					Machine->gfx[2],
					tileno,
					tileatr >> 8,
					tileatr & 0x01,tileatr & 0x02,
					x*16,(y-screen_yoffs+1)*16,16,16,
					&Machine->visible_area
				 );


			}
		}

{
	int j;
	sprintf(buf,"%04X",screen_offs+4*screen_yoffs);
	for (j = 0;j < 4;j++)
		drawgfx(bitmap,Machine->uifont,buf[j],UI_COLOR_NORMAL,0,0,3*8+8*j,8*2,0,TRANSPARENCY_NONE,0);
}
if (keyboard_pressed(KEYCODE_D))
{
	FILE *fp;

	fp=fopen("video.dmp","wb");
	if (fp) {
		fwrite(vidram,0x10c00, 1 ,fp);
		fclose(fp);
	}
}
	}	/* debug */
#endif

#ifdef NEO_DEBUG
/* More debug stuff :) */
{



	int j;
	char mybuf[20];
	struct osd_bitmap *mybitmap = Machine->scrbitmap;



  /*
for (i = 0;i < 8;i+=2)
{
	sprintf(mybuf,"%04X",READ_WORD(&vidram[0x100a0+i]));
	for (j = 0;j < 4;j++)
		drawgfx(mybitmap,Machine->uifont,mybuf[j],UI_COLOR_NORMAL,0,0,3*8*i+8*j,8*5,0,TRANSPARENCY_NONE,0);
}


	sprintf(mybuf,"%04X",READ_WORD(&vidram[0x10002]));
	for (j = 0;j < 4;j++)
		drawgfx(mybitmap,Machine->uifont,mybuf[j],UI_COLOR_NORMAL,0,0,8*j+4*8,8*7,0,TRANSPARENCY_NONE,0);
	sprintf(mybuf,"%04X",0x200-(READ_WORD(&vidram[0x10402])>>7));
	for (j = 0;j < 4;j++)
		drawgfx(mybitmap,Machine->uifont,mybuf[j],UI_COLOR_NORMAL,0,0,8*j+10*8,8*7,0,TRANSPARENCY_NONE,0);
	sprintf(mybuf,"%04X",READ_WORD(&vidram[0x10802])>> 7);
	for (j = 0;j < 4;j++)
		drawgfx(mybitmap,Machine->uifont,mybuf[j],UI_COLOR_NORMAL,0,0,8*j+16*8,8*7,0,TRANSPARENCY_NONE,0);

*/


//		logerror("X: %04x Y: %04x Video: %04x\n",READ_WORD(&vidram[0x1089c]),READ_WORD(&vidram[0x1049c]),READ_WORD(&vidram[0x1009c]));

//logerror("X: %04x Y: %04x Video: %04x\n",READ_WORD(&vidram[0x10930]),READ_WORD(&vidram[0x10530]),READ_WORD(&vidram[0x10130]));


}
#endif

}

static void screenrefresh(struct osd_bitmap *bitmap,const struct rectangle *clip)
{
	screenrefresh_(bitmap,clip,1,1,vidram,neogeo_frame_counter,fix_bank);
}

#ifdef MAMEGO
/* Set when this frame's sprites are left to the host's present task (core 1,
   mame-go main.c), which calls it before converting the bitmap; libretro
   video.c runs it itself when the frame does not go that way. */
static struct { struct osd_bitmap *bitmap; unsigned int frame_counter; int fix_bank; } render_job;

static void neogeo_render_job(void)
{
	neospr_frame();
	screenrefresh_(render_job.bitmap,&Machine->visible_area,0,1,vidram_r,render_job.frame_counter,render_job.fix_bank);
}
#endif

void neogeo_vh_screenrefresh(struct osd_bitmap *bitmap,int full_refresh)
{
#ifdef MAMEGO
	if (render_defer < 0)
	{
		/* only with paged sprites (tiles decoded ahead, pen_usage fixed: nothing
		   the renderer writes is read by core 0) and the host's present path */
		render_defer = neospr_active() && mamego_present_indexed && (vidram_r = malloc(VRAM_USED)) != NULL;
#ifndef ESP_PLATFORM
		if (getenv("NEODEFER") && !strcmp(getenv("NEODEFER"), "0")) render_defer = 0;
#endif
#ifdef NEO_NO_DEFER
		render_defer = 0;
#endif
		if (render_defer)
		{
			memcpy(vidram_r, vidram, VRAM_USED);
			memset(vram_dirty, 0, sizeof(vram_dirty));
			printf("neogeo: sprites drawn on the second core\n");
		}
	}
	if (render_defer && mamego_present_indexed)
	{
		/* the renderer still drawing the last frame reads vidram_r and the
		   palette: wait for it, then palette on core 0, copy, hand over */
		mamego_present_indexed(0, 0, 0, 0, 0, 0, 0);
		screenrefresh_(bitmap,&Machine->visible_area,1,0,vidram,neogeo_frame_counter,fix_bank);
		vram_sync();
		render_job.bitmap = bitmap;
		render_job.frame_counter = neogeo_frame_counter;
		render_job.fix_bank = fix_bank;
		mamego_frame_render = neogeo_render_job;
#ifdef NEOPROF
		{ extern volatile int mamego_render_count[3]; mamego_render_count[0]++; }
#endif
		return;
	}
	if (neospr_active())
		neospr_frame();
#endif
	screenrefresh(bitmap,&Machine->visible_area);
}

static int next_update_first_line;

void neogeo_vh_raster_partial_refresh(struct osd_bitmap *bitmap,int current_line)
{
	struct rectangle clip;

	if (current_line < next_update_first_line)
		next_update_first_line = 0;

	clip.min_x = Machine->visible_area.min_x;
	clip.max_x = Machine->visible_area.max_x;
	clip.min_y = next_update_first_line;
	clip.max_y = current_line;
	if (clip.min_y < Machine->visible_area.min_y)
		clip.min_y = Machine->visible_area.min_y;
	if (clip.max_y > Machine->visible_area.max_y)
		clip.max_y = Machine->visible_area.max_y;

	if (clip.max_y >= clip.min_y)
	{
//logerror("refresh %d-%d\n",clip.min_y,clip.max_y);
		screenrefresh(bitmap,&clip);
	}

	next_update_first_line = current_line + 1;
}

void neogeo_vh_raster_screenrefresh(struct osd_bitmap *bitmap,int full_refresh)
{
#ifdef MAMEGO
	if (neospr_active())
		neospr_frame();
#endif
    /* Palette swap occured after last frame but before this one */
    if (palette_swap_pending) swap_palettes();
    palette_recalc();
	/* no need to check the return code since we redraw everything each frame */
}
