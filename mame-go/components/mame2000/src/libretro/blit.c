#include "libretro.h"
#include "driver.h"
#include "dirty.h"

/* from video.c */
extern char *dirty_old;
extern char *dirty_new;
extern int gfx_xoffset;
extern int gfx_yoffset;
extern int gfx_display_lines;
extern int gfx_display_columns;
extern int gfx_width;
extern int gfx_height;
extern int skiplines;
extern int skipcolumns;

#define SCREEN16 gp2x_screen15

#include "minimal.h"

uint32_t *palette_16bit_lookup;

/* Inner-loop unroll factor for the per-pixel palette-LUT blits below.
 *
 * The 8bpp and palettized-16bpp blits all share a single hot pattern:
 *   dst[x] = palette[src[x]]
 * which compiles to a strict-dependency load -> gather -> store chain
 * (one byte/word load, one indirect LUT load, one short store) repeated
 * scalar.  Manually unrolling four iterations lets the compiler keep
 * four independent gathers in flight at once -- the LUT is L1-hot
 * (256 entries, 512 bytes for 8bpp / 64 KB for palettized-16) so
 * latency, not throughput, is the bottleneck.  4 was picked over 8/16
 * because (a) the typical visible width is 256-384 pixels so the tail
 * cost matters and 4 keeps it under a cache line, and (b) it doesn't
 * blow up icache footprint on the small-cache embedded targets the
 * codebase otherwise has to fit in.  __restrict__ tells the compiler
 * the source/dest/LUT regions are pairwise non-aliasing so it doesn't
 * have to assume worst-case aliasing through unsigned char *. */
#define BLIT_UNROLL 4

void blitscreen_dirty1_color8(struct osd_bitmap *bitmap)
{
	int x, y;
	int width=(bitmap->line[1] - bitmap->line[0]);
	unsigned char *lb=bitmap->line[skiplines] + skipcolumns;
	unsigned short *address=SCREEN16 + gfx_xoffset + (gfx_yoffset * gfx_width);
	const unsigned short * __restrict__ pal = gp2x_palette;

	for (y = 0; y < gfx_display_lines; y += 16)
	{
		for (x = 0; x < gfx_display_columns; )
		{
			int w = 16;
			if (ISDIRTY(x,y))
			{
				int h;
				unsigned char  * __restrict__ lb0      = lb + x;
				unsigned short * __restrict__ address0 = address + x;
				while (x + w < gfx_display_columns && ISDIRTY(x+w,y))
                    			w += 16;
				if (x + w > gfx_display_columns)
                    			w = gfx_display_columns - x;
				for (h = 0; ((h < 16) && ((y + h) < gfx_display_lines)); h++)
				{
					int wx;
					for (wx = 0; wx + BLIT_UNROLL <= w; wx += BLIT_UNROLL)
					{
						unsigned p0 = lb0[wx + 0];
						unsigned p1 = lb0[wx + 1];
						unsigned p2 = lb0[wx + 2];
						unsigned p3 = lb0[wx + 3];
						address0[wx + 0] = pal[p0];
						address0[wx + 1] = pal[p1];
						address0[wx + 2] = pal[p2];
						address0[wx + 3] = pal[p3];
					}
					for (; wx < w; wx++)
						address0[wx] = pal[lb0[wx]];
					lb0 += width;
					address0 += gfx_width;
				}
			}
			x += w;
        	}
		lb += 16 * width;
		address += 16 * gfx_width;
	}
}

void blitscreen_dirty0_color8(struct osd_bitmap *bitmap)
{
	int x,y;
	int width=(bitmap->line[1] - bitmap->line[0]);
	int columns=gfx_display_columns;
	unsigned char  * __restrict__ lb      = bitmap->line[skiplines] + skipcolumns;
	unsigned short * __restrict__ address = SCREEN16 + gfx_xoffset + (gfx_yoffset * gfx_width);
	const unsigned short * __restrict__ pal = gp2x_palette;

	for (y = 0; y < gfx_display_lines; y++)
	{
		for (x = 0; x + BLIT_UNROLL <= columns; x += BLIT_UNROLL)
		{
			unsigned p0 = lb[x + 0];
			unsigned p1 = lb[x + 1];
			unsigned p2 = lb[x + 2];
			unsigned p3 = lb[x + 3];
			address[x + 0] = pal[p0];
			address[x + 1] = pal[p1];
			address[x + 2] = pal[p2];
			address[x + 3] = pal[p3];
		}
		for (; x < columns; x++)
			address[x] = pal[lb[x]];
		lb+=width;
		address+=gfx_width;
	}
}

void blitscreen_dirty1_palettized16(struct osd_bitmap *bitmap)
{
	int x, y;
	int width=(bitmap->line[1] - bitmap->line[0])>>1;
	unsigned short *lb=((unsigned short*)(bitmap->line[skiplines])) + skipcolumns;
	unsigned short *address=SCREEN16 + gfx_xoffset + (gfx_yoffset * gfx_width);
	const uint32_t * __restrict__ pal = palette_16bit_lookup;

	for (y = 0; y < gfx_display_lines; y += 16)
	{
		for (x = 0; x < gfx_display_columns; )
		{
			int w = 16;
			if (ISDIRTY(x,y))
			{
				int h;
				unsigned short * __restrict__ lb0      = lb + x;
				unsigned short * __restrict__ address0 = address + x;
				while (x + w < gfx_display_columns && ISDIRTY(x+w,y))
                    			w += 16;
				if (x + w > gfx_display_columns)
                    			w = gfx_display_columns - x;
				for (h = 0; ((h < 16) && ((y + h) < gfx_display_lines)); h++)
				{
					int wx;
					for (wx = 0; wx + BLIT_UNROLL <= w; wx += BLIT_UNROLL)
					{
						unsigned p0 = lb0[wx + 0];
						unsigned p1 = lb0[wx + 1];
						unsigned p2 = lb0[wx + 2];
						unsigned p3 = lb0[wx + 3];
						address0[wx + 0] = (unsigned short) pal[p0];
						address0[wx + 1] = (unsigned short) pal[p1];
						address0[wx + 2] = (unsigned short) pal[p2];
						address0[wx + 3] = (unsigned short) pal[p3];
					}
					for (; wx < w; wx++)
						address0[wx] = (unsigned short) pal[lb0[wx]];
					lb0 += width;
					address0 += gfx_width;
				}
			}
			x += w;
        	}
		lb += 16 * width;
		address += 16 * gfx_width;
	}
}

void blitscreen_dirty0_palettized16(struct osd_bitmap *bitmap)
{
	int x,y;
	int width=(bitmap->line[1] - bitmap->line[0])>>1;
	int columns=gfx_display_columns;
	unsigned short * __restrict__ lb      = ((unsigned short*)(bitmap->line[skiplines])) + skipcolumns;
	unsigned short * __restrict__ address = SCREEN16 + gfx_xoffset + (gfx_yoffset * gfx_width);
	const uint32_t * __restrict__ pal = palette_16bit_lookup;

	for (y = 0; y < gfx_display_lines; y++)
	{
		for (x = 0; x + BLIT_UNROLL <= columns; x += BLIT_UNROLL)
		{
			unsigned p0 = lb[x + 0];
			unsigned p1 = lb[x + 1];
			unsigned p2 = lb[x + 2];
			unsigned p3 = lb[x + 3];
			address[x + 0] = (unsigned short) pal[p0];
			address[x + 1] = (unsigned short) pal[p1];
			address[x + 2] = (unsigned short) pal[p2];
			address[x + 3] = (unsigned short) pal[p3];
		}
		for (; x < columns; x++)
			address[x] = (unsigned short) pal[lb[x]];
		lb+=width;
		address+=gfx_width;
	}
}

void blitscreen_dirty1_color16(struct osd_bitmap *bitmap)
{
	int x, y;
	int width=(bitmap->line[1] - bitmap->line[0])>>1;
	unsigned short *lb=((unsigned short*)(bitmap->line[skiplines])) + skipcolumns;
	unsigned short *address=SCREEN16 + gfx_xoffset + (gfx_yoffset * gfx_width);

	for (y = 0; y < gfx_display_lines; y += 16)
	{
		for (x = 0; x < gfx_display_columns; )
		{
			int w = 16;
			if (ISDIRTY(x,y))
			{
				int h;
				unsigned short *lb0 = lb + x;
				unsigned short *address0 = address + x;
				while (x + w < gfx_display_columns && ISDIRTY(x+w,y))
                    			w += 16;
				if (x + w > gfx_display_columns)
                    			w = gfx_display_columns - x;
				for (h = 0; ((h < 16) && ((y + h) < gfx_display_lines)); h++)
				{
					memcpy(address0, lb0, w * sizeof(unsigned short));
					lb0 += width;
					address0 += gfx_width;
				}
			}
			x += w;
        	}
		lb += 16 * width;
		address += 16 * gfx_width;
	}
}

void blitscreen_dirty0_color16(struct osd_bitmap *bitmap)
{
	int y;
	int width=(bitmap->line[1] - bitmap->line[0])>>1;
	int columns=gfx_display_columns;
	unsigned short *lb = ((unsigned short*)(bitmap->line[skiplines])) + skipcolumns;
	unsigned short *address = SCREEN16 + gfx_xoffset + (gfx_yoffset * gfx_width);

	for (y = 0; y < gfx_display_lines; y++)
	{
		memcpy(address, lb, columns * sizeof(unsigned short));
		lb+=width;
		address+=gfx_width;
	}
}
