// SOTN for Retro-Go - size of the frame the game just finished.
//
// The PSX display mode changes with the screen: stage 256x240, menus 384x240,
// title/logos 512x240 (debug views 512x480). The video glue must scale THAT
// size to the panel; assuming 256 crops the title and menus. This lives in its
// own file because g_CurrentBuffer is a game type: it needs the game headers
// and must not see retro-go's.

#include "pc.h"
#include "dra.h"

void Sotn_DisplaySize(int* w, int* h) {
    int x = 0, y = 0, ww = 256, hh = 240;
    GpuBuffer* b = g_CurrentBuffer;
    if (b) {
        x = b->disp.disp.x;
        y = b->disp.disp.y;
        ww = b->disp.disp.w;
        hh = b->disp.disp.h;
    }
    if (ww < 64 || ww > 1024) {
        ww = 256;
    }
    if (hh < 64 || hh > 512) {
        hh = 240;
    }
    // never read past the 1024x512 VRAM
    if (x < 0 || x + ww > 1024) {
        ww = (x >= 0 && x < 1024 - 64) ? 1024 - x : 256;
    }
    if (y < 0 || y + hh > 512) {
        hh = (y >= 0 && y < 512 - 64) ? 512 - y : 240;
    }
    *w = ww;
    *h = hh;
}
