/* This file is part of Snes9x. See LICENSE file. */

#include "snes9x.h"

#include "memmap.h"
#include "ppu.h"
#include "display.h"
#include "gfx.h"
#include "tile.h"

static const uint32_t HeadMask[4] =
{
#ifdef MSB_FIRST
   0xffffffff, 0x00ffffff, 0x0000ffff, 0x000000ff
#else
   0xffffffff, 0xffffff00, 0xffff0000, 0xff000000
#endif
};

static const uint32_t TailMask[5] =
{
#ifdef MSB_FIRST
   0x00000000, 0xff000000, 0xffff0000, 0xffffff00, 0xffffffff
#else
   0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff, 0xffffffff
#endif
};

static const uint32_t odd[4][16] =
{
#ifdef MSB_FIRST
   {0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101, 0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101},
   {0x00000000, 0x00000004, 0x00000400, 0x00000404, 0x00040000, 0x00040004, 0x00040400, 0x00040404, 0x04000000, 0x04000004, 0x04000400, 0x04000404, 0x04040000, 0x04040004, 0x04040400, 0x04040404},
   {0x00000000, 0x00000010, 0x00001000, 0x00001010, 0x00100000, 0x00100010, 0x00101000, 0x00101010, 0x10000000, 0x10000010, 0x10001000, 0x10001010, 0x10100000, 0x10100010, 0x10101000, 0x10101010},
   {0x00000000, 0x00000040, 0x00004000, 0x00004040, 0x00400000, 0x00400040, 0x00404000, 0x00404040, 0x40000000, 0x40000040, 0x40004000, 0x40004040, 0x40400000, 0x40400040, 0x40404000, 0x40404040}
#else
   {0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100, 0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101},
   {0x00000000, 0x04000000, 0x00040000, 0x04040000, 0x00000400, 0x04000400, 0x00040400, 0x04040400, 0x00000004, 0x04000004, 0x00040004, 0x04040004, 0x00000404, 0x04000404, 0x00040404, 0x04040404},
   {0x00000000, 0x10000000, 0x00100000, 0x10100000, 0x00001000, 0x10001000, 0x00101000, 0x10101000, 0x00000010, 0x10000010, 0x00100010, 0x10100010, 0x00001010, 0x10001010, 0x00101010, 0x10101010},
   {0x00000000, 0x40000000, 0x00400000, 0x40400000, 0x00004000, 0x40004000, 0x00404000, 0x40404000, 0x00000040, 0x40000040, 0x00400040, 0x40400040, 0x00004040, 0x40004040, 0x00404040, 0x40404040}
#endif
};

static const uint32_t even[4][16] =
{
#ifdef MSB_FIRST
   {0x00000000, 0x00000002, 0x00000200, 0x00000202, 0x00020000, 0x00020002, 0x00020200, 0x00020202, 0x02000000, 0x02000002, 0x02000200, 0x02000202, 0x02020000, 0x02020002, 0x02020200, 0x02020202},
   {0x00000000, 0x00000008, 0x00000800, 0x00000808, 0x00080000, 0x00080008, 0x00080800, 0x00080808, 0x08000000, 0x08000008, 0x08000800, 0x08000808, 0x08080000, 0x08080008, 0x08080800, 0x08080808},
   {0x00000000, 0x00000020, 0x00002000, 0x00002020, 0x00200000, 0x00200020, 0x00202000, 0x00202020, 0x20000000, 0x20000020, 0x20002000, 0x20002020, 0x20200000, 0x20200020, 0x20202000, 0x20202020},
   {0x00000000, 0x00000080, 0x00008000, 0x00008080, 0x00800000, 0x00800080, 0x00808000, 0x00808080, 0x80000000, 0x80000080, 0x80008000, 0x80008080, 0x80800000, 0x80800080, 0x80808000, 0x80808080}
#else
   {0x00000000, 0x02000000, 0x00020000, 0x02020000, 0x00000200, 0x02000200, 0x00020200, 0x02020200, 0x00000002, 0x02000002, 0x00020002, 0x02020002, 0x00000202, 0x02000202, 0x00020202, 0x02020202},
   {0x00000000, 0x08000000, 0x00080000, 0x08080000, 0x00000800, 0x08000800, 0x00080800, 0x08080800, 0x00000008, 0x08000008, 0x00080008, 0x08080008, 0x00000808, 0x08000808, 0x00080808, 0x08080808},
   {0x00000000, 0x20000000, 0x00200000, 0x20200000, 0x00002000, 0x20002000, 0x00202000, 0x20202000, 0x00000020, 0x20000020, 0x00200020, 0x20200020, 0x00002020, 0x20002020, 0x00202020, 0x20202020},
   {0x00000000, 0x80000000, 0x00800000, 0x80800000, 0x00008000, 0x80008000, 0x00808000, 0x80808000, 0x00000080, 0x80000080, 0x00800080, 0x80800080, 0x00008080, 0x80008080, 0x00808080, 0x80808080}
#endif
};

static uint8_t ConvertTile(uint8_t* pCache, uint32_t TileAddr)
{
   SNES_PROF_INC(tile_conv);
   uint8_t* tp = &Memory.VRAM[TileAddr];
   uint32_t* p = (uint32_t*) pCache;
   uint32_t non_zero = 0;
   uint8_t line;
   uint32_t p1;
   uint32_t p2;
   uint8_t pix;

   switch (BG.BitShift)
   {
   case 8:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if((pix = tp[0]))
         {
            p1 |= odd[0][pix >> 4];
            p2 |= odd[0][pix & 0xf];
         }
         if((pix = tp[1]))
         {
            p1 |= even[0][pix >> 4];
            p2 |= even[0][pix & 0xf];
         }
         if((pix = tp[16]))
         {
            p1 |= odd[1][pix >> 4];
            p2 |= odd[1][pix & 0xf];
         }
         if((pix = tp[17]))
         {
            p1 |= even[1][pix >> 4];
            p2 |= even[1][pix & 0xf];
         }
         if((pix = tp[32]))
         {
            p1 |= odd[2][pix >> 4];
            p2 |= odd[2][pix & 0xf];
         }
         if((pix = tp[33]))
         {
            p1 |= even[2][pix >> 4];
            p2 |= even[2][pix & 0xf];
         }
         if((pix = tp[48]))
         {
            p1 |= odd[3][pix >> 4];
            p2 |= odd[3][pix & 0xf];
         }
         if((pix = tp[49]))
         {
            p1 |= even[3][pix >> 4];
            p2 |= even[3][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;
   case 4:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if((pix = tp[0]))
         {
            p1 |= odd[0][pix >> 4];
            p2 |= odd[0][pix & 0xf];
         }
         if((pix = tp[1]))
         {
            p1 |= even[0][pix >> 4];
            p2 |= even[0][pix & 0xf];
         }
         if((pix = tp[16]))
         {
            p1 |= odd[1][pix >> 4];
            p2 |= odd[1][pix & 0xf];
         }
         if((pix = tp[17]))
         {
            p1 |= even[1][pix >> 4];
            p2 |= even[1][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;
   case 2:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if((pix = tp[0]))
         {
            p1 |= odd[0][pix >> 4];
            p2 |= odd[0][pix & 0xf];
         }
         if((pix = tp[1]))
         {
            p1 |= even[0][pix >> 4];
            p2 |= even[0][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;
   }
   return non_zero ? (0x10|BG.Depth) : (BLANK_TILE|BG.Depth);
}

#define PLOT_PIXEL(screen, pixel) (pixel)

static INLINE void WRITE_4PIXELS16(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if defined(__MIPSEL) && defined(__GNUC__) && !defined(NO_ASM)
	uint16_t *Screen = (uint16_t *) GFX.S + Offset;
	uint8_t  *Depth = GFX.DB + Offset;
	uint8_t  Pixel_A, Pixel_B, Pixel_C, Pixel_D;
	uint8_t  Depth_A, Depth_B, Depth_C, Depth_D;
	uint8_t  Cond;
	uint32_t Temp;
	__asm__ __volatile__ (
		".set noreorder                        \n"
		"   lbu   %[In8A], 0(%[In8])           \n"
		"   lbu   %[In8B], 1(%[In8])           \n"
		"   lbu   %[In8C], 2(%[In8])           \n"
		"   lbu   %[In8D], 3(%[In8])           \n"
		"   lbu   %[ZA], 0(%[Z])               \n"
		"   lbu   %[ZB], 1(%[Z])               \n"
		"   lbu   %[ZC], 2(%[Z])               \n"
		"   lbu   %[ZD], 3(%[Z])               \n"
		/* If In8A is non-zero (opaque) and ZCompare > ZA, write the pixel to
		 * the screen from the palette. */
		"   sltiu %[Temp], %[In8A], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZA]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		/* Otherwise skip to the next pixel, B. */
		"   bne   %[Cond], $0, 2f              \n"
		/* Load the address of the palette entry (16-bit) corresponding to
		 * this pixel (partially in the delay slot). */
		"   sll   %[In8A], %[In8A], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8A] \n"
		/* Load the palette entry. While that's being done, store the new
		 * depth for this pixel. Then store to the screen. */
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 0(%[Z])             \n"
		"   sh    %[Temp], 0(%[Out16])         \n"
		/* Now do the same for pixel B. */
		"2: sltiu %[Temp], %[In8B], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZB]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		"   bne   %[Cond], $0, 3f              \n"
		"   sll   %[In8B], %[In8B], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8B] \n"
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 1(%[Z])             \n"
		"   sh    %[Temp], 2(%[Out16])         \n"
		/* Now do the same for pixel C. */
		"3: sltiu %[Temp], %[In8C], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZC]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		"   bne   %[Cond], $0, 4f              \n"
		"   sll   %[In8C], %[In8C], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8C] \n"
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 2(%[Z])             \n"
		"   sh    %[Temp], 4(%[Out16])         \n"
		/* Now do the same for pixel D. */
		"4: sltiu %[Temp], %[In8D], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZD]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		"   bne   %[Cond], $0, 5f              \n"
		"   sll   %[In8D], %[In8D], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8D] \n"
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 3(%[Z])             \n"
		"   sh    %[Temp], 6(%[Out16])         \n"
		"5:                                    \n"
		".set reorder                          \n"
		: /* output */  [In8A] "=&r" (Pixel_A), [In8B] "=&r" (Pixel_B), [In8C] "=&r" (Pixel_C), [In8D] "=&r" (Pixel_D), [ZA] "=&r" (Depth_A), [ZB] "=&r" (Depth_B), [ZC] "=&r" (Depth_C), [ZD] "=&r" (Depth_D), [Cond] "=&r" (Cond), [Temp] "=&r" (Temp)
		: /* input */   [Out16] "r" (Screen), [Z] "r" (Depth), [In8] "r" (Pixels), [Palette] "r" (ScreenColors), [ZCompare] "r" (GFX.Z1), [ZSet] "r" (GFX.Z2)
		: /* clobber */ "memory"
	);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
#endif
}

static INLINE void WRITE_4PIXELS16_FLIPPED(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if defined(__MIPSEL) && defined(__GNUC__) && !defined(NO_ASM)
	uint16_t *Screen = (uint16_t *) GFX.S + Offset;
	uint8_t  *Depth = GFX.DB + Offset;
	uint8_t  Pixel_A, Pixel_B, Pixel_C, Pixel_D;
	uint8_t  Depth_A, Depth_B, Depth_C, Depth_D;
	uint8_t  Cond;
	uint32_t Temp;
	__asm__ __volatile__ (
		".set noreorder                        \n"
		"   lbu   %[In8A], 3(%[In8])           \n"
		"   lbu   %[In8B], 2(%[In8])           \n"
		"   lbu   %[In8C], 1(%[In8])           \n"
		"   lbu   %[In8D], 0(%[In8])           \n"
		"   lbu   %[ZA], 0(%[Z])               \n"
		"   lbu   %[ZB], 1(%[Z])               \n"
		"   lbu   %[ZC], 2(%[Z])               \n"
		"   lbu   %[ZD], 3(%[Z])               \n"
		/* If In8A is non-zero (opaque) and ZCompare > ZA, write the pixel to
		 * the screen from the palette. */
		"   sltiu %[Temp], %[In8A], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZA]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		/* Otherwise skip to the next pixel, B. */
		"   bne   %[Cond], $0, 2f              \n"
		/* Load the address of the palette entry (16-bit) corresponding to
		 * this pixel (partially in the delay slot). */
		"   sll   %[In8A], %[In8A], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8A] \n"
		/* Load the palette entry. While that's being done, store the new
		 * depth for this pixel. Then store to the screen. */
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 0(%[Z])             \n"
		"   sh    %[Temp], 0(%[Out16])         \n"
		/* Now do the same for pixel B. */
		"2: sltiu %[Temp], %[In8B], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZB]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		"   bne   %[Cond], $0, 3f              \n"
		"   sll   %[In8B], %[In8B], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8B] \n"
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 1(%[Z])             \n"
		"   sh    %[Temp], 2(%[Out16])         \n"
		/* Now do the same for pixel C. */
		"3: sltiu %[Temp], %[In8C], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZC]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		"   bne   %[Cond], $0, 4f              \n"
		"   sll   %[In8C], %[In8C], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8C] \n"
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 2(%[Z])             \n"
		"   sh    %[Temp], 4(%[Out16])         \n"
		/* Now do the same for pixel D. */
		"4: sltiu %[Temp], %[In8D], 1          \n"
		"   sltu  %[Cond], %[ZCompare], %[ZD]  \n"
		"   or    %[Cond], %[Cond], %[Temp]    \n"
		"   bne   %[Cond], $0, 5f              \n"
		"   sll   %[In8D], %[In8D], 1          \n"
		"   addu  %[Temp], %[Palette], %[In8D] \n"
		"   lhu   %[Temp], 0(%[Temp])          \n"
		"   sb    %[ZSet], 3(%[Z])             \n"
		"   sh    %[Temp], 6(%[Out16])         \n"
		"5:                                    \n"
		".set reorder                          \n"
		: /* output */  [In8A] "=&r" (Pixel_A), [In8B] "=&r" (Pixel_B), [In8C] "=&r" (Pixel_C), [In8D] "=&r" (Pixel_D), [ZA] "=&r" (Depth_A), [ZB] "=&r" (Depth_B), [ZC] "=&r" (Depth_C), [ZD] "=&r" (Depth_D), [Cond] "=&r" (Cond), [Temp] "=&r" (Temp)
		: /* input */   [Out16] "r" (Screen), [Z] "r" (Depth), [In8] "r" (Pixels), [Palette] "r" (ScreenColors), [ZCompare] "r" (GFX.Z1), [ZSet] "r" (GFX.Z2)
		: /* clobber */ "memory"
	);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
#endif
}

static void WRITE_4PIXELS16_HALFWIDTH(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N += 2)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         Screen [N >> 1] = ScreenColors [Pixel];
         Depth [N >> 1] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPED_HALFWIDTH(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N += 2)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[2 - N]))
      {
         Screen [N >> 1] = ScreenColors [Pixel];
         Depth [N >> 1] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16x2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPEDx2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16x2x2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = Screen [(GFX.RealPitch >> 1) + N * 2] = Screen [(GFX.RealPitch >> 1) + N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = Depth [(GFX.RealPitch >> 1) + N * 2] = Depth [(GFX.RealPitch >> 1) + N * 2 + 1] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPEDx2x2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = Screen [(GFX.RealPitch >> 1) + N * 2] = Screen [(GFX.RealPitch >> 1) + N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = Depth [(GFX.RealPitch >> 1) + N * 2] = Depth [(GFX.RealPitch >> 1) + N * 2 + 1] = GFX.Z2;
      }
   }
}

/* Plain 16-bit writers with every invariant in registers (esp32-emu-turbo,
 * 2026-09-14). WRITE_4PIXELS16 above reads GFX.S / GFX.DB / GFX.Z1 / GFX.Z2
 * per call and, because Depth[] is a uint8_t store that may alias anything,
 * the compiler reloads them and the tile bytes after every pixel. Here the
 * pointers are restrict (tile cache, z-buffer, screen and palette never
 * overlap) and the depths are plain values hoisted by TILE_FAST_VARS(). */
static INLINE void write4_fast(uint16_t *restrict Screen, uint8_t *restrict Depth, const uint8_t *restrict Pixels,
                               const uint16_t *restrict Colors, uint32_t Z1, uint32_t Z2)
{
   uint32_t p;
   if (Z1 > Depth[0] && (p = Pixels[0])) { Screen[0] = Colors[p]; Depth[0] = Z2; }
   if (Z1 > Depth[1] && (p = Pixels[1])) { Screen[1] = Colors[p]; Depth[1] = Z2; }
   if (Z1 > Depth[2] && (p = Pixels[2])) { Screen[2] = Colors[p]; Depth[2] = Z2; }
   if (Z1 > Depth[3] && (p = Pixels[3])) { Screen[3] = Colors[p]; Depth[3] = Z2; }
}

static INLINE void write4_fast_flipped(uint16_t *restrict Screen, uint8_t *restrict Depth, const uint8_t *restrict Pixels,
                                       const uint16_t *restrict Colors, uint32_t Z1, uint32_t Z2)
{
   uint32_t p;
   if (Z1 > Depth[0] && (p = Pixels[3])) { Screen[0] = Colors[p]; Depth[0] = Z2; }
   if (Z1 > Depth[1] && (p = Pixels[2])) { Screen[1] = Colors[p]; Depth[1] = Z2; }
   if (Z1 > Depth[2] && (p = Pixels[1])) { Screen[2] = Colors[p]; Depth[2] = Z2; }
   if (Z1 > Depth[3] && (p = Pixels[0])) { Screen[3] = Colors[p]; Depth[3] = Z2; }
}

#define TILE_FAST_VARS() \
   uint16_t *const S = (uint16_t *) GFX.S; \
   uint8_t *const DB = GFX.DB; \
   const uint32_t Z1 = GFX.Z1, Z2 = GFX.Z2
#define W4_FAST(Offset, Pixels, Colors)         write4_fast(S + (Offset), DB + (Offset), Pixels, Colors, Z1, Z2)
#define W4_FAST_FLIPPED(Offset, Pixels, Colors) write4_fast_flipped(S + (Offset), DB + (Offset), Pixels, Colors, Z1, Z2)

void DrawTile16(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_FAST_VARS();
   RENDER_TILE(W4_FAST, W4_FAST_FLIPPED, 4);
}

void DrawClippedTile16(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   TILE_FAST_VARS();
   RENDER_CLIPPED_TILE_CODE(W4_FAST, W4_FAST_FLIPPED, 4);
}

/* Same writers, palette fetched per line for tiles on CGRAM entries 0-15
 * (selected by SelectTileRenderer only while IPPU.PalLineDirty). */
void DrawTile16PalLine(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PALLINE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_PALLINE_CODE();
   TILE_FAST_VARS();
   RENDER_TILE_PL(W4_FAST, W4_FAST_FLIPPED, 4);
}

void DrawClippedTile16PalLine(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PALLINE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_PALLINE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   TILE_FAST_VARS();
   RENDER_CLIPPED_TILE_CODE_PL(W4_FAST, W4_FAST_FLIPPED, 4);
}

void DrawTile16HalfWidth(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16_HALFWIDTH, WRITE_4PIXELS16_FLIPPED_HALFWIDTH, 2);
}

void DrawClippedTile16HalfWidth(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_HALFWIDTH, WRITE_4PIXELS16_FLIPPED_HALFWIDTH, 2);
}

void DrawTile16x2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16x2, WRITE_4PIXELS16_FLIPPEDx2, 8);
}

void DrawClippedTile16x2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16x2, WRITE_4PIXELS16_FLIPPEDx2, 8);
}

void DrawTile16x2x2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16x2x2, WRITE_4PIXELS16_FLIPPEDx2x2, 8);
}

void DrawClippedTile16x2x2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16x2x2, WRITE_4PIXELS16_FLIPPEDx2x2, 8);
}

void DrawLargePixel16(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t pixel;
   uint16_t *sp;
   uint8_t *Depth;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   sp = (uint16_t*) GFX.S + Offset;
   Depth = GFX.DB + Offset;
   RENDER_TILE_LARGE(ScreenColors [pixel], PLOT_PIXEL);
}

void DrawLargePixel16HalfWidth(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t pixel;
   uint16_t *sp;
   uint8_t *Depth;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   sp = (uint16_t*) GFX.S + Offset;
   Depth = GFX.DB + Offset;
   RENDER_TILE_LARGE_HALFWIDTH(ScreenColors [pixel], PLOT_PIXEL);
}

/* Colour-math writers (esp32-emu-turbo, 2026-09-14). Same idea as
 * write4_fast: restrict pointers, depths and the fixed colour in registers.
 * Sub z-buffer value per pixel: 0 = no sub pixel (plain colour), 1 = fixed
 * colour (OPFIX, or the precomputed MC palette in gfx.c SubColMode),
 * >= 2 = sub screen pixel (OPSUB). The Fixed*1_2 writers never add the sub
 * screen (OPSUB = M_PLAIN). */
#define M_PLAIN(c, s) (c)

#define DEFINE_WRITE4_MATH(NAME, OPSUB, OPFIX, P0, P1, P2, P3) \
static INLINE void NAME(uint16_t *restrict Screen, uint8_t *restrict Depth, const uint8_t *restrict Pixels, \
                        const uint16_t *restrict Colors, const uint8_t *restrict SubDepth, \
                        const uint16_t *restrict SubScreen, const uint16_t *restrict MC, \
                        uint32_t Z1, uint32_t Z2, uint32_t Fixed) \
{ \
   uint32_t p, sd; \
   if (Z1 > Depth[0] && (p = Pixels[P0])) { sd = SubDepth[0]; Screen[0] = sd == 0 ? Colors[p] : sd == 1 ? (MC ? MC[p] : OPFIX(Colors[p], Fixed)) : OPSUB(Colors[p], SubScreen[0]); Depth[0] = Z2; } \
   if (Z1 > Depth[1] && (p = Pixels[P1])) { sd = SubDepth[1]; Screen[1] = sd == 0 ? Colors[p] : sd == 1 ? (MC ? MC[p] : OPFIX(Colors[p], Fixed)) : OPSUB(Colors[p], SubScreen[1]); Depth[1] = Z2; } \
   if (Z1 > Depth[2] && (p = Pixels[P2])) { sd = SubDepth[2]; Screen[2] = sd == 0 ? Colors[p] : sd == 1 ? (MC ? MC[p] : OPFIX(Colors[p], Fixed)) : OPSUB(Colors[p], SubScreen[2]); Depth[2] = Z2; } \
   if (Z1 > Depth[3] && (p = Pixels[P3])) { sd = SubDepth[3]; Screen[3] = sd == 0 ? Colors[p] : sd == 1 ? (MC ? MC[p] : OPFIX(Colors[p], Fixed)) : OPSUB(Colors[p], SubScreen[3]); Depth[3] = Z2; } \
}

DEFINE_WRITE4_MATH(write4_add,       COLOR_ADD,    COLOR_ADD,    0, 1, 2, 3)
DEFINE_WRITE4_MATH(write4_add_f,     COLOR_ADD,    COLOR_ADD,    3, 2, 1, 0)
DEFINE_WRITE4_MATH(write4_add1_2,    COLOR_ADD1_2, COLOR_ADD,    0, 1, 2, 3)
DEFINE_WRITE4_MATH(write4_add1_2_f,  COLOR_ADD1_2, COLOR_ADD,    3, 2, 1, 0)
DEFINE_WRITE4_MATH(write4_sub,       COLOR_SUB,    COLOR_SUB,    0, 1, 2, 3)
DEFINE_WRITE4_MATH(write4_sub_f,     COLOR_SUB,    COLOR_SUB,    3, 2, 1, 0)
DEFINE_WRITE4_MATH(write4_sub1_2,    COLOR_SUB1_2, COLOR_SUB,    0, 1, 2, 3)
DEFINE_WRITE4_MATH(write4_sub1_2_f,  COLOR_SUB1_2, COLOR_SUB,    3, 2, 1, 0)
DEFINE_WRITE4_MATH(write4_addf1_2,   M_PLAIN,      COLOR_ADD1_2, 0, 1, 2, 3)
DEFINE_WRITE4_MATH(write4_addf1_2_f, M_PLAIN,      COLOR_ADD1_2, 3, 2, 1, 0)
DEFINE_WRITE4_MATH(write4_subf1_2,   M_PLAIN,      COLOR_SUB1_2, 0, 1, 2, 3)
DEFINE_WRITE4_MATH(write4_subf1_2_f, M_PLAIN,      COLOR_SUB1_2, 3, 2, 1, 0)

/* Colour math always targets the main screen / main z-buffer. In SubColMode
 * the sub z-buffer is the per-column GFX.SubCol table (index = x). */
#define TILE_MATH_VARS() \
   uint16_t *const S = (uint16_t *) GFX.S; \
   uint8_t *const DB = GFX.ZBuffer; \
   const uint32_t Z1 = GFX.Z1, Z2 = GFX.Z2, Fixed = GFX.FixedColour; \
   const int32_t Delta = GFX.Delta; \
   const uint8_t *const SD = GFX.SubColMode ? GFX.SubCol : GFX.SubZBuffer; \
   const uint32_t SubMask = GFX.SubColMode ? 0xff : 0xffffffffu; \
   const uint16_t *const MC = GFX.SubColMode ? GFX.MathColors + (ScreenColors - IPPU.ScreenColors) : NULL
#define W4_MATH(FN, Offset, Pixels, Colors) \
   FN(S + (Offset), DB + (Offset), Pixels, Colors, SD + ((Offset) & SubMask), S + Delta + (Offset), MC, Z1, Z2, Fixed)
#define W4_ADD(O, P, C)         W4_MATH(write4_add, O, P, C)
#define W4_ADD_F(O, P, C)       W4_MATH(write4_add_f, O, P, C)
#define W4_ADD1_2(O, P, C)      W4_MATH(write4_add1_2, O, P, C)
#define W4_ADD1_2_F(O, P, C)    W4_MATH(write4_add1_2_f, O, P, C)
#define W4_SUB(O, P, C)         W4_MATH(write4_sub, O, P, C)
#define W4_SUB_F(O, P, C)       W4_MATH(write4_sub_f, O, P, C)
#define W4_SUB1_2(O, P, C)      W4_MATH(write4_sub1_2, O, P, C)
#define W4_SUB1_2_F(O, P, C)    W4_MATH(write4_sub1_2_f, O, P, C)
#define W4_ADDF1_2(O, P, C)     W4_MATH(write4_addf1_2, O, P, C)
#define W4_ADDF1_2_F(O, P, C)   W4_MATH(write4_addf1_2_f, O, P, C)
#define W4_SUBF1_2(O, P, C)     W4_MATH(write4_subf1_2, O, P, C)
#define W4_SUBF1_2_F(O, P, C)   W4_MATH(write4_subf1_2_f, O, P, C)

/* SubColMode with a colour window: the sub z is 0/1 by column and the
 * colour-math result for 1 is the MC palette, so a pixel is a palette
 * select. One pair covers every operation (the op lives in MC). */
#define DEFINE_WRITE4_SUBCOL(NAME, P0, P1, P2, P3) \
static INLINE void NAME(uint16_t *restrict Screen, uint8_t *restrict Depth, const uint8_t *restrict Pixels, \
                        const uint16_t *restrict Colors, const uint16_t *restrict MC, \
                        const uint8_t *restrict SubCol, uint32_t Z1, uint32_t Z2) \
{ \
   uint32_t p; \
   if (Z1 > Depth[0] && (p = Pixels[P0])) { Screen[0] = (SubCol[0] ? MC : Colors)[p]; Depth[0] = Z2; } \
   if (Z1 > Depth[1] && (p = Pixels[P1])) { Screen[1] = (SubCol[1] ? MC : Colors)[p]; Depth[1] = Z2; } \
   if (Z1 > Depth[2] && (p = Pixels[P2])) { Screen[2] = (SubCol[2] ? MC : Colors)[p]; Depth[2] = Z2; } \
   if (Z1 > Depth[3] && (p = Pixels[P3])) { Screen[3] = (SubCol[3] ? MC : Colors)[p]; Depth[3] = Z2; } \
}
DEFINE_WRITE4_SUBCOL(write4_subcol,   0, 1, 2, 3)
DEFINE_WRITE4_SUBCOL(write4_subcol_f, 3, 2, 1, 0)

#define TILE_SUBCOL_VARS() \
   uint16_t *const S = (uint16_t *) GFX.S; \
   uint8_t *const DB = GFX.ZBuffer; \
   const uint32_t Z1 = GFX.Z1, Z2 = GFX.Z2; \
   const uint8_t *const SC = GFX.SubCol; \
   const uint16_t *const MC = GFX.MathColors + (ScreenColors - IPPU.ScreenColors)
#define W4_SUBCOL(O, P, C)   write4_subcol(S + (O), DB + (O), P, C, MC, SC + ((O) & 0xff), Z1, Z2)
#define W4_SUBCOL_F(O, P, C) write4_subcol_f(S + (O), DB + (O), P, C, MC, SC + ((O) & 0xff), Z1, Z2)

void DrawTile16SubCol(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_SUBCOL_VARS();
   RENDER_TILE(W4_SUBCOL, W4_SUBCOL_F, 4);
}

void DrawClippedTile16SubCol(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   TILE_SUBCOL_VARS();
   RENDER_CLIPPED_TILE_CODE(W4_SUBCOL, W4_SUBCOL_F, 4);
}

#define DEFINE_MATH_TILE(NAME, NORMAL, FLIPPED) \
void NAME(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount) \
{ \
   uint8_t* bp; \
   TILE_PREAMBLE_VARS(); \
   TILE_PREAMBLE_CODE(); \
   TILE_MATH_VARS(); \
   RENDER_TILE(NORMAL, FLIPPED, 4); \
}
#define DEFINE_MATH_CLIPPED_TILE(NAME, NORMAL, FLIPPED) \
void NAME(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount) \
{ \
   uint8_t* bp; \
   TILE_PREAMBLE_VARS(); \
   TILE_CLIP_PREAMBLE_VARS(); \
   RENDER_CLIPPED_TILE_VARS(); \
   TILE_PREAMBLE_CODE(); \
   TILE_CLIP_PREAMBLE_CODE(); \
   TILE_MATH_VARS(); \
   RENDER_CLIPPED_TILE_CODE(NORMAL, FLIPPED, 4); \
}

DEFINE_MATH_TILE(DrawTile16Add,                  W4_ADD,      W4_ADD_F)
DEFINE_MATH_CLIPPED_TILE(DrawClippedTile16Add,   W4_ADD,      W4_ADD_F)
DEFINE_MATH_TILE(DrawTile16Add1_2,               W4_ADD1_2,   W4_ADD1_2_F)
DEFINE_MATH_CLIPPED_TILE(DrawClippedTile16Add1_2, W4_ADD1_2,  W4_ADD1_2_F)
DEFINE_MATH_TILE(DrawTile16Sub,                  W4_SUB,      W4_SUB_F)
DEFINE_MATH_CLIPPED_TILE(DrawClippedTile16Sub,   W4_SUB,      W4_SUB_F)
DEFINE_MATH_TILE(DrawTile16Sub1_2,               W4_SUB1_2,   W4_SUB1_2_F)
DEFINE_MATH_CLIPPED_TILE(DrawClippedTile16Sub1_2, W4_SUB1_2,  W4_SUB1_2_F)
DEFINE_MATH_TILE(DrawTile16FixedAdd1_2,          W4_ADDF1_2,  W4_ADDF1_2_F)
DEFINE_MATH_CLIPPED_TILE(DrawClippedTile16FixedAdd1_2, W4_ADDF1_2, W4_ADDF1_2_F)
DEFINE_MATH_TILE(DrawTile16FixedSub1_2,          W4_SUBF1_2,  W4_SUBF1_2_F)
DEFINE_MATH_CLIPPED_TILE(DrawClippedTile16FixedSub1_2, W4_SUBF1_2, W4_SUBF1_2_F)

void DrawLargePixel16Add(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_ADD_PIXEL(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_ADD (p, *(s + GFX.Delta)) : \
                COLOR_ADD (p, GFX.FixedColour)) : p)

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_ADD_PIXEL);
}

void DrawLargePixel16Add1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_ADD_PIXEL1_2(s, p) \
((uint16_t) (Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_ADD1_2 (p, *(s + GFX.Delta)) : \
                COLOR_ADD (p, GFX.FixedColour)) : p))

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_ADD_PIXEL1_2);
}

void DrawLargePixel16Sub(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_SUB_PIXEL(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_SUB (p, *(s + GFX.Delta)) : \
                COLOR_SUB (p, GFX.FixedColour)) : p)

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_SUB_PIXEL);
}

void DrawLargePixel16Sub1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_SUB_PIXEL1_2(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_SUB1_2 (p, *(s + GFX.Delta)) : \
                COLOR_SUB (p, GFX.FixedColour)) : p)

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_SUB_PIXEL1_2);
}
