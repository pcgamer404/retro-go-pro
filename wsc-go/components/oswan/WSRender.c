/*
$Date: 2009-10-30 05:26:46 +0100 (ven., 30 oct. 2009) $
$Rev: 71 $
*/
#include <stdint.h>
#include <string.h>
#include <stdlib.h>


#include "WS.h"
#include "WSRender.h"
#include "WSSegment.h"
#include "rg_system.h"
#include "rg_utils.h"

#if defined(ESP32) || defined(ESP_PLATFORM)
#include <esp_attr.h>
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

// Spreads the 8 bits of a planar tile row into 8 nibbles, one per pixel.
// This replaces 32 shift/mask/or operations with four table lookups.
static uint32_t wsc_plane_table[256];
static int wsc_plane_table_ready;
// Sprite window mask: 1 inside the window, 0 outside (matches the hardware
// "sprite window" used when DSPCTL bit 3 is set).
static BYTE SprWin[0x100];

static void IRAM_ATTR init_plane_table(void) {
  for (int b = 0; b < 256; b++) {
    uint32_t v = 0;
    for (int k = 0; k < 8; k++) {
      v |= ((b >> (7 - k)) & 1) << (k * 4);
    }
    wsc_plane_table[b] = v;
  }
  wsc_plane_table_ready = 1;
}

#define MAP_TILE 0x01FF
#define MAP_PAL 0x1E00
#define MAP_BANK 0x2000
#define MAP_HREV 0x4000
#define MAP_VREV 0x8000
BYTE *Scr1TMap;
BYTE *Scr2TMap;

#define SPR_TILE 0x01FF
#define SPR_PAL 0x0E00
#define SPR_CLIP 0x1000
#define SPR_LAYR 0x2000
#define SPR_HREV 0x4000
#define SPR_VREV 0x8000
#define SEG_X (LCD_MAIN_W - 32)
#define STRIDE 256

BYTE *SprTTMap;
BYTE *SprETMap;
BYTE *SprTMap = NULL;
WORD *FrameBuffer_real = NULL;
WORD *FrameBuffer = NULL;
WORD Palette[16][16];
WORD MonoColor[8];
int Layer[3] = {1, 1, 1};
int Segment[11];

#ifdef WS_USE_SEGMENT_BUFFER
WORD *SegmentBuffer = NULL;
#endif

void WsSetVidBuf(void *ptr) {
  FrameBuffer = (WORD *)ptr;
}

void AllocateBuffers(void) {
  // SprTMap : 1024 bytes to prevent overflow if SPRCNT > 128
  SprTMap = (BYTE *)rg_alloc(1024 * sizeof(BYTE), MEM_FAST);
  memset(SprTMap, 0, 1024 * sizeof(BYTE));

  if (!FrameBuffer) {
    // FrameBuffer is usually provided by main.c, but fallback just in case.
    FrameBuffer_real =
        (WORD *)rg_alloc((SCREEN_WIDTH * LCD_MAIN_H + 16) * sizeof(WORD), MEM_FAST);
    memset(FrameBuffer_real, 0,
           (SCREEN_WIDTH * LCD_MAIN_H + 16) * sizeof(WORD));
    FrameBuffer = FrameBuffer_real + 8;
  }

#ifdef WS_USE_SEGMENT_BUFFER
  // SegmentBuffer : (LCD_MAIN_H * 4) * (8 * 4) WORDs
  SegmentBuffer = (WORD *)rg_alloc((LCD_MAIN_H * 4) * (8 * 4) * sizeof(WORD), MEM_FAST);
  memset(SegmentBuffer, 0, (LCD_MAIN_H * 4) * (8 * 4) * sizeof(WORD));
#endif
}

void FreeBuffers(void) {
  if (SprTMap) {
    free(SprTMap);
    SprTMap = NULL;
  }
  if (FrameBuffer_real) {
    free(FrameBuffer_real);
    FrameBuffer_real = NULL;
    FrameBuffer = NULL;
  }

#ifdef WS_USE_SEGMENT_BUFFER
  if (SegmentBuffer) {
    free(SegmentBuffer);
    SegmentBuffer = NULL;
  }
#endif
}

IRAM_ATTR void SetPalette(int addr) {
  WORD color, r, g, b;

  // RGB444 format
  color = *(WORD *)(IRAM + (addr & 0xFFFE));
  // Expand RGB444 to the full RGB565 range instead of leaving the low bits
  // zero (the old conversion made every color noticeably dark).
  r = (color >> 8) & 0x0F;
  g = (color >> 4) & 0x0F;
  b = color & 0x0F;
  r = (r << 1) | (r >> 3);
  g = (g << 2) | (g >> 2);
  b = (b << 1) | (b >> 3);
  Palette[(addr & 0x1E0) >> 5][(addr & 0x1E) >> 1] =
      (r << 11) | (g << 5) | b;
}

IRAM_ATTR void RefreshLine(int Line) {
  // Keep hot hardware state in locals. Without this the compiler has to reload
  // these globals after nearly every framebuffer write because the stores may
  // alias them.
  const BYTE colctl = COLCTL;
  const BYTE dspctl = DSPCTL;
  const BYTE lcdslp = LCDSLP;
  BYTE *const iram = IRAM; // pointer fixed, data read-only in this function
  WORD (*const palette)[16] = Palette;
  BYTE *const scr1 = Scr1TMap;
  BYTE *const scr2 = Scr2TMap;
  const int layer0 = Layer[0];
  const int layer1 = Layer[1];
  const int layer2 = Layer[2];
  // BG always covers the full visible width. When it is active we don't need
  // the separate clear pass: transparent BG pixels are written as BaseCol.
  const bool bg_active = (dspctl & 0x01) && layer0;
  const bool fg_window = ((dspctl & 0x30) == 0x20) || ((dspctl & 0x30) == 0x30);
  const bool spr_window = (dspctl & 0x08);
  const bool sprite_active = (dspctl & 0x04) && layer2;
  if (!wsc_plane_table_ready) {
    init_plane_table();
  }
  WORD *pSBuf;     // f[^݃obt@
  WORD *pSWrBuf;   // ݈̏ʒup|C^
  BYTE *pZ;        // ̃CNgp|C^
  BYTE ZBuf[0x100]; // FGC[̔񓧖ۑ
  BYTE *pW;        // ̃CNgp|C^
  BYTE WBuf[0x100]; // FGC[̃EBh[ۑ
  int OffsetX;     //
  int OffsetY;     //
  BYTE *pbTMap;    //
  int TMap;        //
  int TMapX;       //
  int TMapXEnd;    //
  BYTE *pbTData;   //
  int PalIndex;    //
  unsigned int i;
  BYTE index[8];
  int j, k; // Sprite coordinates are signed (0xF8..0xFF = -8..-1)
  bool drawing_bg = false;
  WORD BaseCol; //
  pSBuf = FrameBuffer + Line * SCREEN_WIDTH;
  pSWrBuf = pSBuf;

  if (lcdslp & 0x01) {
    if (colctl & 0xE0) {
      BaseCol = palette[(BORDER & 0xF0) >> 4][BORDER & 0x0F];
    } else {
      BaseCol = MonoColor[BORDER & 0x07];
    }
  } else {
    BaseCol = 0;
  }
  uint32_t *pSWrBuf32 = (uint32_t *)pSWrBuf;
  uint32_t BaseCol32 = (BaseCol << 16) | BaseCol;
  if (!bg_active) {
    for (i = 0; i < LCD_MAIN_W / 2; i++) {
      *pSWrBuf32++ = BaseCol32;
    }
  }
  if (!(lcdslp & 0x01))
    return;
  /*********************************************************************/
  if ((dspctl & 0x01) && layer0) // BG layer
  {
    drawing_bg = true;
    OffsetX = SCR1X & 0x07;
    pSWrBuf = pSBuf - OffsetX;
    i = Line + SCR1Y;
    OffsetY = (i & 0x07);

    pbTMap = scr1 + ((i & 0xF8) << 3);
    TMapX = (SCR1X & 0xF8) >> 2;
    TMapXEnd = ((SCR1X + LCD_MAIN_W + 7) >> 2) & 0xFFE;

    for (; TMapX < TMapXEnd;) {
      TMap = *(WORD*)(pbTMap + (TMapX & 0x3F));
      TMapX += 2;

      if (colctl & 0x40) // 16 colors
      {
        if (TMap & MAP_BANK) {
          pbTData = iram + 0x8000;
        } else {
          pbTData = iram + 0x4000;
        }
        pbTData += (TMap & MAP_TILE) << 5;
        if (TMap & MAP_VREV) {
          pbTData += (7 - OffsetY) << 2;
        } else {
          pbTData += OffsetY << 2;
        }
      } else {
        if ((colctl & 0x80) && (TMap & MAP_BANK)) // 2bpp color mode: bank 2
        {
          pbTData = iram + 0x4000;
        } else {
          pbTData = iram + 0x2000;
        }
        pbTData += (TMap & MAP_TILE) << 4;
        if (TMap & MAP_VREV) {
          pbTData += (7 - OffsetY) << 1;
        } else {
          pbTData += OffsetY << 1;
        }
      }

      // Blank planar rows are common for empty tiles. Skip the decode and
      // palette lookups entirely in that case.
      if ((colctl & 0x40) && (*(const uint32_t *)pbTData == 0)) {
        if (drawing_bg) {
          WORD c = BaseCol;
          for (int n = 0; n < 8; n++) {
            *pSWrBuf++ = c;
          }
        } else {
          pSWrBuf += 8;
          pZ += 8;
          if (fg_window) {
            pW += 8;
          }
        }
        continue;
      }

      if (colctl & 0x20) // Packed Mode
      {
        if (colctl & 0x40) // 16 Color
        {
          index[0] = (pbTData[0] & 0xF0) >> 4;
          index[1] = pbTData[0] & 0x0F;
          index[2] = (pbTData[1] & 0xF0) >> 4;
          index[3] = pbTData[1] & 0x0F;
          index[4] = (pbTData[2] & 0xF0) >> 4;
          index[5] = pbTData[2] & 0x0F;
          index[6] = (pbTData[3] & 0xF0) >> 4;
          index[7] = pbTData[3] & 0x0F;
        } else // 4 Color
        {
          index[0] = (pbTData[0] & 0xC0) >> 6;
          index[1] = (pbTData[0] & 0x30) >> 4;
          index[2] = (pbTData[0] & 0x0C) >> 2;
          index[3] = pbTData[0] & 0x03;
          index[4] = (pbTData[1] & 0xC0) >> 6;
          index[5] = (pbTData[1] & 0x30) >> 4;
          index[6] = (pbTData[1] & 0x0C) >> 2;
          index[7] = pbTData[1] & 0x03;
        }
      } else { // Planar Mode
        if (colctl & 0x40) // 16 Color
        {
          uint32_t b0 = pbTData[0], b1 = pbTData[1], b2 = pbTData[2], b3 = pbTData[3];
          uint32_t v = wsc_plane_table[b0] | (wsc_plane_table[b1] << 1) |
                       (wsc_plane_table[b2] << 2) | (wsc_plane_table[b3] << 3);
          index[0] = v & 0xF;
          index[1] = (v >> 4) & 0xF;
          index[2] = (v >> 8) & 0xF;
          index[3] = (v >> 12) & 0xF;
          index[4] = (v >> 16) & 0xF;
          index[5] = (v >> 20) & 0xF;
          index[6] = (v >> 24) & 0xF;
          index[7] = (v >> 28) & 0xF;
        } else // 4 Color
        {
          uint32_t b0 = pbTData[0], b1 = pbTData[1];
          index[0] = ((b0 >> 7) & 1) | (((b1 >> 7) & 1) << 1);
          index[1] = ((b0 >> 6) & 1) | (((b1 >> 6) & 1) << 1);
          index[2] = ((b0 >> 5) & 1) | (((b1 >> 5) & 1) << 1);
          index[3] = ((b0 >> 4) & 1) | (((b1 >> 4) & 1) << 1);
          index[4] = ((b0 >> 3) & 1) | (((b1 >> 3) & 1) << 1);
          index[5] = ((b0 >> 2) & 1) | (((b1 >> 2) & 1) << 1);
          index[6] = ((b0 >> 1) & 1) | (((b1 >> 1) & 1) << 1);
          index[7] = (b0 & 1) | ((b1 & 1) << 1);
        }
      }

      const int is_transparent_0 = (colctl & 0x40) || (TMap & 0x0800);
      WORD *pal = palette[(TMap & MAP_PAL) >> 9];

      if (TMap & MAP_HREV) {
        for (int k = 0; k < 8; k++) {
          int idx = index[7 - k];
          WORD c = pal[idx];
          if (!idx && is_transparent_0) {
            c = BaseCol;
          }
          *pSWrBuf++ = c;
        }
      } else {
        for (int k = 0; k < 8; k++) {
          int idx = index[k];
          WORD c = pal[idx];
          if (!idx && is_transparent_0) {
            c = BaseCol;
          }
          *pSWrBuf++ = c;
        }
      }
    }
  }
  /*********************************************************************/
  if (sprite_active) {
    memset(ZBuf, 0, sizeof(ZBuf));
  }
  if (fg_window) {
    memset(WBuf, 0, sizeof(WBuf));
  }
  if (spr_window) {
    // Sprite window coordinates are inclusive. A row outside the Y range has
    // an empty window (all zero), matching the hardware.
    memset(SprWin, 0, sizeof(SprWin));
    const int spr_x0 = IO[0x0C], spr_y0 = IO[0x0D];
    const int spr_x1 = IO[0x0E], spr_y1 = IO[0x0F];
    if (Line >= spr_y0 && Line <= spr_y1) {
      for (int x = spr_x0; x <= spr_x1 && x < LCD_MAIN_W; x++)
        SprWin[8 + x] = 1;
    }
  }
  if ((dspctl & 0x02) && layer1) // FG layer
  {
    drawing_bg = false;
    if ((dspctl & 0x30) == 0x20) // display only inside window
    {
      for (i = 0, pW = WBuf + 8; i < LCD_MAIN_W; i++) {
        *pW++ = 1;
      }
      if ((Line >= SCR2WT) && (Line <= SCR2WB)) {
        for (i = SCR2WL, pW = WBuf + 8 + i; (i <= SCR2WR) && (i < LCD_MAIN_W);
             i++) {
          *pW++ = 0;
        }
      }
    } else if ((dspctl & 0x30) == 0x30) // display only outside window
    {
      for (i = 0, pW = WBuf + 8; i < LCD_MAIN_W; i++) {
        *pW++ = 0;
      }
      if ((Line >= SCR2WT) && (Line <= SCR2WB)) {
        for (i = SCR2WL, pW = WBuf + 8 + i; (i <= SCR2WR) && (i < LCD_MAIN_W);
             i++) {
          *pW++ = 1;
        }
      }
    }

    OffsetX = SCR2X & 0x07;
    pSWrBuf = pSBuf - OffsetX;
    i = Line + SCR2Y;
    OffsetY = (i & 0x07);

    pbTMap = scr2 + ((i & 0xF8) << 3);
    TMapX = (SCR2X & 0xF8) >> 2;
    TMapXEnd = ((SCR2X + LCD_MAIN_W + 7) >> 2) & 0xFFE;

    pW = WBuf + 8 - OffsetX;
    pZ = ZBuf + 8 - OffsetX;

    for (; TMapX < TMapXEnd;) {
      TMap = *(WORD*)(pbTMap + (TMapX & 0x3F));
      TMapX += 2;

      if (colctl & 0x40) {
        if (TMap & MAP_BANK) {
          pbTData = iram + 0x8000;
        } else {
          pbTData = iram + 0x4000;
        }
        pbTData += (TMap & MAP_TILE) << 5;
        if (TMap & MAP_VREV) {
          pbTData += (7 - OffsetY) << 2;
        } else {
          pbTData += OffsetY << 2;
        }
      } else {
        if ((colctl & 0x80) && (TMap & MAP_BANK)) // 2bpp color mode: bank 2
        {
          pbTData = iram + 0x4000;
        } else {
          pbTData = iram + 0x2000;
        }
        pbTData += (TMap & MAP_TILE) << 4;
        if (TMap & MAP_VREV) {
          pbTData += (7 - OffsetY) << 1;
        } else {
          pbTData += OffsetY << 1;
        }
      }

      // Blank planar rows are common for empty tiles. Skip the decode and
      // palette lookups entirely in that case.
      if ((colctl & 0x40) && (*(const uint32_t *)pbTData == 0)) {
        if (drawing_bg) {
          WORD c = BaseCol;
          for (int n = 0; n < 8; n++) {
            *pSWrBuf++ = c;
          }
        } else {
          pSWrBuf += 8;
          pZ += 8;
          if (fg_window) {
            pW += 8;
          }
        }
        continue;
      }

      if (colctl & 0x20) // Packed Mode
      {
        if (colctl & 0x40) // 16 Color
        {
          index[0] = (pbTData[0] & 0xF0) >> 4;
          index[1] = pbTData[0] & 0x0F;
          index[2] = (pbTData[1] & 0xF0) >> 4;
          index[3] = pbTData[1] & 0x0F;
          index[4] = (pbTData[2] & 0xF0) >> 4;
          index[5] = pbTData[2] & 0x0F;
          index[6] = (pbTData[3] & 0xF0) >> 4;
          index[7] = pbTData[3] & 0x0F;
        } else // 4 Color
        {
          index[0] = (pbTData[0] & 0xC0) >> 6;
          index[1] = (pbTData[0] & 0x30) >> 4;
          index[2] = (pbTData[0] & 0x0C) >> 2;
          index[3] = pbTData[0] & 0x03;
          index[4] = (pbTData[1] & 0xC0) >> 6;
          index[5] = (pbTData[1] & 0x30) >> 4;
          index[6] = (pbTData[1] & 0x0C) >> 2;
          index[7] = pbTData[1] & 0x03;
        }
      } else { // Planar Mode
        if (colctl & 0x40) // 16 Color
        {
          uint32_t b0 = pbTData[0], b1 = pbTData[1], b2 = pbTData[2], b3 = pbTData[3];
          uint32_t v = wsc_plane_table[b0] | (wsc_plane_table[b1] << 1) |
                       (wsc_plane_table[b2] << 2) | (wsc_plane_table[b3] << 3);
          index[0] = v & 0xF;
          index[1] = (v >> 4) & 0xF;
          index[2] = (v >> 8) & 0xF;
          index[3] = (v >> 12) & 0xF;
          index[4] = (v >> 16) & 0xF;
          index[5] = (v >> 20) & 0xF;
          index[6] = (v >> 24) & 0xF;
          index[7] = (v >> 28) & 0xF;
        } else // 4 Color
        {
          uint32_t b0 = pbTData[0], b1 = pbTData[1];
          index[0] = ((b0 >> 7) & 1) | (((b1 >> 7) & 1) << 1);
          index[1] = ((b0 >> 6) & 1) | (((b1 >> 6) & 1) << 1);
          index[2] = ((b0 >> 5) & 1) | (((b1 >> 5) & 1) << 1);
          index[3] = ((b0 >> 4) & 1) | (((b1 >> 4) & 1) << 1);
          index[4] = ((b0 >> 3) & 1) | (((b1 >> 3) & 1) << 1);
          index[5] = ((b0 >> 2) & 1) | (((b1 >> 2) & 1) << 1);
          index[6] = ((b0 >> 1) & 1) | (((b1 >> 1) & 1) << 1);
          index[7] = (b0 & 1) | ((b1 & 1) << 1);
        }
      }

      // Redundant HREV swap removed to fix cancel-out bug

      const int is_transparent_0 = (colctl & 0x40) || (TMap & 0x0800);
      WORD *pal = palette[(TMap & MAP_PAL) >> 9];

      if (fg_window) {
        if (TMap & MAP_HREV) {
          for (int k = 0; k < 8; k++, pW++, pZ++) {
            int idx = index[7 - k];
            if (*pW) {
              pSWrBuf++;
            } else if (idx) {
              *pSWrBuf++ = pal[idx];
              *pZ = 1;
            } else if (!is_transparent_0) {
              *pSWrBuf++ = pal[0];
              *pZ = 1;
            } else {
              pSWrBuf++;
            }
          }
        } else {
          for (int k = 0; k < 8; k++, pW++, pZ++) {
            int idx = index[k];
            if (*pW) {
              pSWrBuf++;
            } else if (idx) {
              *pSWrBuf++ = pal[idx];
              *pZ = 1;
            } else if (!is_transparent_0) {
              *pSWrBuf++ = pal[0];
              *pZ = 1;
            } else {
              pSWrBuf++;
            }
          }
        }
      } else {
        // Window disabled: this is the common case and avoids a load+branch
        // for every single pixel.
        if (TMap & MAP_HREV) {
          for (int k = 0; k < 8; k++, pZ++) {
            int idx = index[7 - k];
            if (idx) {
              *pSWrBuf++ = pal[idx];
              *pZ = 1;
            } else if (!is_transparent_0) {
              *pSWrBuf++ = pal[0];
              *pZ = 1;
            } else {
              pSWrBuf++;
            }
          }
        } else {
          for (int k = 0; k < 8; k++, pZ++) {
            int idx = index[k];
            if (idx) {
              *pSWrBuf++ = pal[idx];
              *pZ = 1;
            } else if (!is_transparent_0) {
              *pSWrBuf++ = pal[0];
              *pZ = 1;
            } else {
              pSWrBuf++;
            }
          }
        }
      }
    }
  }
  /*********************************************************************/
  if (sprite_active) // sprite
  {
    for (pbTMap = SprETMap; pbTMap >= SprTTMap; pbTMap -= 4) //
    {
      TMap = *(WORD*)pbTMap;

      if (pbTMap[2] > 0xF8) {
        j = pbTMap[2] - 0x100;
      } else {
        j = pbTMap[2];
      }
      if (pbTMap[3] > 0xF8) {
        k = pbTMap[3] - 0x100;
      } else {
        k = pbTMap[3];
      }

      if (Line < j)
        continue;
      if (Line >= j + 8)
        continue;
      if (LCD_MAIN_W <= k)
        continue;
      // A sprite at X < -7 is entirely off-screen; drawing it would write
      // before the framebuffer row (and corrupt the previous row).
      if (k < -7)
        continue;

      pSWrBuf = pSBuf + k;

      if (colctl & 0x40) {
        pbTData = iram + 0x4000;
        pbTData += (TMap & SPR_TILE) << 5;
        if (TMap & SPR_VREV) {
          pbTData += (7 - Line + j) << 2;
        } else {
          pbTData += (Line - j) << 2;
        }
      } else {
        pbTData = iram + 0x2000;
        pbTData += (TMap & SPR_TILE) << 4;
        if (TMap & SPR_VREV) {
          pbTData += (7 - Line + j) << 1;
        } else {
          pbTData += (Line - j) << 1;
        }
      }

      if (colctl & 0x20) // Packed Mode
      {
        if (colctl & 0x40) // 16 Color
        {
          index[0] = (pbTData[0] & 0xF0) >> 4;
          index[1] = pbTData[0] & 0x0F;
          index[2] = (pbTData[1] & 0xF0) >> 4;
          index[3] = pbTData[1] & 0x0F;
          index[4] = (pbTData[2] & 0xF0) >> 4;
          index[5] = pbTData[2] & 0x0F;
          index[6] = (pbTData[3] & 0xF0) >> 4;
          index[7] = pbTData[3] & 0x0F;
        } else // 4 Color
        {
          index[0] = (pbTData[0] & 0xC0) >> 6;
          index[1] = (pbTData[0] & 0x30) >> 4;
          index[2] = (pbTData[0] & 0x0C) >> 2;
          index[3] = pbTData[0] & 0x03;
          index[4] = (pbTData[1] & 0xC0) >> 6;
          index[5] = (pbTData[1] & 0x30) >> 4;
          index[6] = (pbTData[1] & 0x0C) >> 2;
          index[7] = pbTData[1] & 0x03;
        }
      } else { // Planar Mode
        if (colctl & 0x40) // 16 Color
        {
          uint32_t b0 = pbTData[0], b1 = pbTData[1], b2 = pbTData[2], b3 = pbTData[3];
          uint32_t v = wsc_plane_table[b0] | (wsc_plane_table[b1] << 1) |
                       (wsc_plane_table[b2] << 2) | (wsc_plane_table[b3] << 3);
          index[0] = v & 0xF;
          index[1] = (v >> 4) & 0xF;
          index[2] = (v >> 8) & 0xF;
          index[3] = (v >> 12) & 0xF;
          index[4] = (v >> 16) & 0xF;
          index[5] = (v >> 20) & 0xF;
          index[6] = (v >> 24) & 0xF;
          index[7] = (v >> 28) & 0xF;
        } else // 4 Color
        {
          uint32_t b0 = pbTData[0], b1 = pbTData[1];
          index[0] = ((b0 >> 7) & 1) | (((b1 >> 7) & 1) << 1);
          index[1] = ((b0 >> 6) & 1) | (((b1 >> 6) & 1) << 1);
          index[2] = ((b0 >> 5) & 1) | (((b1 >> 5) & 1) << 1);
          index[3] = ((b0 >> 4) & 1) | (((b1 >> 4) & 1) << 1);
          index[4] = ((b0 >> 3) & 1) | (((b1 >> 3) & 1) << 1);
          index[5] = ((b0 >> 2) & 1) | (((b1 >> 2) & 1) << 1);
          index[6] = ((b0 >> 1) & 1) | (((b1 >> 1) & 1) << 1);
          index[7] = (b0 & 1) | ((b1 & 1) << 1);
        }
      }

      // Redundant SPR_HREV swap removed to fix cancel-out bug

      pW = SprWin + 8 + k;
      pZ = ZBuf + k + 8;
      PalIndex = ((TMap & SPR_PAL) >> 9) + 8;

      const int is_transparent_0 = (colctl & 0x40) || (TMap & 0x0800);
      const int check_window = (dspctl & 0x08);
      const int sprite_clip = (TMap & SPR_CLIP);
      const int sprite_layer = (TMap & SPR_LAYR);
      WORD *pal = palette[((TMap & SPR_PAL) >> 9) + 8];

      if (TMap & SPR_HREV) {
        for (i = 0; i < 8; i++, pZ++, pW++) {
          int idx = index[7 - i];
          if (check_window) {
            // Bit 12 selects the side of the window: set = draw outside,
            // clear = draw inside (matches Mednafen's sprite window logic).
            if (sprite_clip) { if (*pW) { pSWrBuf++; continue; } }
            else { if (!*pW) { pSWrBuf++; continue; } }
          }
          if (!idx && is_transparent_0) { pSWrBuf++; continue; }
          if ((*pZ) && (!sprite_layer)) { pSWrBuf++; continue; }
          *pSWrBuf++ = pal[idx];
        }
      } else {
        for (i = 0; i < 8; i++, pZ++, pW++) {
          int idx = index[i];
          if (check_window) {
            // Bit 12 selects the side of the window: set = draw outside,
            // clear = draw inside (matches Mednafen's sprite window logic).
            if (sprite_clip) { if (*pW) { pSWrBuf++; continue; } }
            else { if (!*pW) { pSWrBuf++; continue; } }
          }
          if (!idx && is_transparent_0) { pSWrBuf++; continue; }
          if ((*pZ) && (!sprite_layer)) { pSWrBuf++; continue; }
          *pSWrBuf++ = pal[idx];
        }
      }
    }
  }
}

/*
 8 * 144 �̃T�C�Y�� 32 * 576 �ŕ`��
*/

#ifdef WS_USE_SEGMENT_BUFFER
void RenderSegment(void) {
  int bit, x, y, i;
  WORD *p = SegmentBuffer;

  for (i = 0; i < 11; i++) {
    for (y = 0; y < segLine[i]; y++) {
      for (x = 0; x < 4; x++) {
        BYTE ch = seg[i][y * 4 + x];
        for (bit = 0; bit < 8; bit++) {
          if (ch & 0x80) {
            if (Segment[i]) {
              *p++ = 0xFCCC;
            } else {
              *p++ = 0xF222;
            }
          } else {
            *p++ = 0xF000;
          }
          ch <<= 1;
        }
      }
    }
  }
}
#else
// Without SegmentBuffer
void RenderSegment(void) {
  for (int yOut = 0; yOut < LCD_MAIN_H; yOut++) {
    const int yStart = yOut * 4;
    const int yEnd = yStart + 3;

    // Accumulation 32
    unsigned char accum[4] = {0, 0, 0, 0};

    int yBase = 0;
    for (int i = 0; i < 11; i++) {
      const int lines = segLine[i];

      int first = yStart - yBase;
      if (first < 0)
        first = 0;
      int last = yEnd - yBase;
      if (last >= lines)
        last = lines - 1;

      if (first <= last) {
        const unsigned char *s = seg[i] + first * 4; // 4 bytes per sub-line
        for (int y = first; y <= last; y++) {
          accum[0] |= s[0];
          accum[1] |= s[1];
          accum[2] |= s[2];
          accum[3] |= s[3];
          s += 4;
        }
      }
      yBase += lines;
    }

    int anyActive = 0;
    yBase = 0;
    for (int i = 0; i < 11; i++) {
      int first = yStart - yBase;
      if (first < 0)
        first = 0;
      int last = yEnd - yBase;
      if (last >= segLine[i])
        last = segLine[i] - 1;
      if (first <= last && Segment[i]) {
        anyActive = 1;
        break;
      }
      yBase += segLine[i];
    }

    const WORD onCol = anyActive ? 0xFCCC : 0xF222;
    const WORD offCol = 0xF000;

    if (SEG_X >= LCD_MAIN_W)
      continue; // completely offscreen to the right

    int maxWidth = LCD_MAIN_W - SEG_X; // clip to screen
    if (maxWidth > 32)
      maxWidth = 32;

    WORD *dst = FrameBuffer + yOut * SCREEN_WIDTH + SEG_X;

    // 32 pixels MSB first
    int written = 0;
    for (int byte = 0; byte < 4 && written < maxWidth; byte++) {
      unsigned char v = accum[byte];
      for (int bit = 0; bit < 8 && written < maxWidth; bit++, written++) {
        *dst++ = (v & 0x80) ? onCol : offCol;
        v <<= 1;
      }
    }
  }
}
#endif

void RenderSleep(void) {
  int x, y;
  WORD *p;

  // Clear only the visible part of every row; the padding belongs to the
  // previous row's right edge and must not be written here.
  for (y = 0; y < LCD_MAIN_H; y++) {
    p = FrameBuffer + y * SCREEN_WIDTH;
    for (x = 0; x < LCD_MAIN_W; x++) {
      *p++ = 0x4208;
    }
  }
}
