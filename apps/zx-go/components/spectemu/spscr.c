/* 
 * Copyright (C) 1996-1998 Szeredi Miklos
 * Email: mszeredi@inf.bme.hu
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. See the file COPYING. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "spscr_p.h"
#include "spscr.h"

#include "spperif.h"
#include "z80.h"


#include <stdlib.h>
#include <stdio.h>

int color_type = 0;

#define N0 0x04
#define N1 0x34

#define B0 0x08
#define B1 0x3F

struct rgb *spscr_crgb;

static struct rgb norm_colors[COLORNUM]={
  {0,0,0},{N0,N0,N1},{N1,N0,N0},{N1,N0,N1},
  {N0,N1,N0},{N0,N1,N1},{N1,N1,N0},{N1,N1,N1},

  {0x15,0x15,0x15},{B0,B0,B1},{B1,B0,B0},{B1,B0,B1},
  {B0,B1,B0},{B0,B1,B1},{B1,B1,B0},{B1,B1,B1}
};

static struct rgb gray_colors[COLORNUM]={
  {0,0,0},{20,20,20},{26,26,26},{32,32,32},
  {38,38,38},{44,44,44},{50,50,50},{56,56,56},

  {16,16,16},{23,23,23},{30,30,30},{36,36,36},
  {43,43,43},{50,50,50},{56,56,56},{63,63,63}
};

struct rgb custom_colors[COLORNUM]={
  {0,0,0},{N0,N0,N1},{N1,N0,N0},{N1,N0,N1},
  {N0,N1,N0},{N0,N1,N1},{N1,N1,N0},{N1,N1,N1},

  {0x15,0x15,0x15},{B0,B0,B1},{B1,B0,B0},{B1,B0,B1},
  {B0,B1,B0},{B0,B1,B1},{B1,B1,B0},{B1,B1,B1}
};


#define TABOFFS 2

volatile int screen_visible = 1;
volatile int accept_keys = 1;

int lastborder=100,skip=0;

extern int buffpos;  // audio buffer position
extern int keyboard; // on-screen keyboard active?

// Clean update_screen_line stub - frame rendering is performed by spectemu_render_screen in RGB565 directly into Retro-Go surface
byte *update_screen_line(byte *scrp, int coli, int scri, int border,
			 qbyte *cmarkp)
{
  (void)coli;
  (void)scri;
  (void)border;
  (void)cmarkp;
  return scrp;
}
//-----------------------------------------------------------------------------------------------------


void translate_screen(void)
{
  int border, scline;
  byte *scrptr;
  qbyte cmark = 0;

  scrptr = (byte *) SPNM(image);

  border = DANM(ula_outport) & 0x07;
  if(border != SPNM(lastborder)) {
    SPNM(border_update) = 2;
    SPNM(lastborder) = border;
  }

  for(scline = 0; scline < (TMNUM / 2); scline++) 
    scrptr = update_screen_line(scrptr, SPNM(coli)[scline], SPNM(scri)[scline],
				border, &cmark);

}

void flash_change(void)
{
  int i,j;
  byte *scp;
  qbyte *mcp;
  register unsigned int val;

  mcp = sp_scr_mark + 0x2C0;
  scp = z80_proc.mem + 0x5800;

  for(i = 24; i; mcp++, i--) {
    val = 0;
    for(j = 32; j; scp++, j--) {
      val >>= 1;
      if(*scp & 0x80) val |= (1 << 31);
    }
    *mcp |= val;
  }
}

void spscr_init_line_pointers(int lines)
{
  int i;
  int bs;
  int y;
  int scline;
  
  bs = (lines - 192) / 2;

  for(i = 0; i < PORT_TIME_NUM; i++) {

    sp_scri[i] = -2;

    if(i < ODDHF) scline = i;
    else scline = i - ODDHF;
    
    if(scline >= 64-bs && scline < 256+bs) {
      if(scline >= 64 && scline < 256) {
	y = scline - 64;
	sp_coli[i] = y;
	sp_scri[i] = 0x200 +
	  (y & 0xC0) + ((y & 0x07) << 3) + ((y & 0x38) >> 3);
      }
      else sp_scri[i] = -1;
    }
  }
}

void spscr_init_colors(void)
{
  spscr_crgb = norm_colors;
  
  switch(color_type) {
  case 0:
    spscr_crgb = norm_colors;
    break;
    
  case 1:
    spscr_crgb = gray_colors;
    break;
    
  case 2:
    spscr_crgb = custom_colors;
    break;
  }
}
