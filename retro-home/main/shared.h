#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rg_system.h>
#include "core_frameskip.h"

// Rate for the single rg_system_init() in main.c; each core then calls rg_system_reinit() with its own
// (fMSX 32000, Frodo 22050).
#define AUDIO_SAMPLE_RATE   (32000)

#ifdef __cplusplus
extern "C" {
#endif

void msx_main(void);
void c64_main(void);
// Add future home computers here (zx_main, ...)

#ifdef __cplusplus
}
#endif
