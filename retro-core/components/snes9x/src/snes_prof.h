/* Renderer counters for the esp32-emu-turbo Phase 4 work (step 4.0).
 *
 * Compiled in only with SNES_PROF=1 (see retro-core/CMakeLists.txt). Unlike
 * RG_ENABLE_PROFILING this adds no -finstrument-functions, so the numbers
 * are those of the production code plus a few counters and timer reads.
 * All fields are per-second accumulators, reset by main_snes.c after it
 * prints them.
 */
#ifndef _SNES_PROF_H_
#define _SNES_PROF_H_

#include <stdint.h>

#if SNES_PROF

typedef struct
{
   uint32_t strips;       /* S9xUpdateScreen calls (strips drawn) */
   uint32_t strip_lines;  /* scanlines drawn */
   uint32_t sub_passes;   /* RenderScreen(sub) calls (colour math on) */
   uint32_t tiles;        /* tile draws entered (TILE_PREAMBLE_CODE) */
   uint32_t tiles_blank;  /* of which rejected as BLANK_TILE */
   uint32_t tile_conv;    /* ConvertTile calls (tile cache misses) */
   uint32_t obj_setup;    /* S9xSetupOBJ calls */
   uint32_t mode_hist[8]; /* strips per BG mode */
   int64_t t_update;      /* whole S9xUpdateScreen */
   int64_t t_clear;       /* z-buffer / sub-screen clears */
   int64_t t_sub;         /* RenderScreen(sub) */
   int64_t t_main;        /* RenderScreen(main) */
   int64_t t_combine;     /* colour-math combine / backdrop fill after main */
   int64_t t_obj;         /* DrawOBJS (main + sub) */
   int64_t t_bg[4];       /* DrawBackground per BG (main + sub) */
   int64_t t_objsetup;    /* S9xSetupOBJ */
   uint32_t cur_layer;    /* 0-3 = BG being drawn, 4 = OBJ (set by the callers) */
   uint32_t l_tiles[5];   /* non-blank tile draws per layer */
   uint32_t l_clipped[5]; /* of which through DrawClippedTile* (edge / window) */
   uint32_t l_lines[5];   /* sum of LineCount: 8 per full-height tile band */
   uint32_t m7_lines;     /* Mode 7 scanlines rendered */
   int64_t t_m7;          /* Mode 7 background drawers */
   uint32_t m7_variant[5];
   uint32_t m7_runs;       /* Mode 7 spans drawn (clip regions x colour-window runs x lines) */
   uint32_t last_reg;      /* PPU register being written (S9xSetPPU), 0x80 = DMA */
   uint32_t flush_hist[0x40]; /* S9xUpdateScreen calls per triggering register (0x2100 + i) */
   uint32_t cg_hist[32];   /* CGRAM entries changed mid-strip: 0-15 = entry, 16+ = group of 16 */
   uint32_t cg_zero;       /* of which entry 0 (backdrop) */
   uint32_t subempty_why;  /* OR of: 1 sub active, 2 colour window, 4 pseudo, 8 direct, 16 = fast path taken (sum over strips) */ /* strips: plain, add, add1/2, sub, sub1/2 */
} snes_prof_t;

extern snes_prof_t snes_prof;

#define SNES_PROF_INC(field)      (snes_prof.field++)
#define SNES_PROF_SET(field, v)   (snes_prof.field = (v))
#define SNES_PROF_ADDN(field, n)  (snes_prof.field += (n))
#define SNES_PROF_T0(var)         int64_t var = rg_system_timer()
#define SNES_PROF_ACC(field, var) (snes_prof.field += rg_system_timer() - (var))

#else

#define SNES_PROF_INC(field)      ((void)0)
#define SNES_PROF_SET(field, v)   ((void)0)
#define SNES_PROF_ADDN(field, n)  ((void)0)
#define SNES_PROF_T0(var)         ((void)0)
#define SNES_PROF_ACC(field, var) ((void)0)

#endif

#endif
