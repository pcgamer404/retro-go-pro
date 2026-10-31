#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rg_system.h>
#include "core_frameskip.h"

// Rate used for the one rg_system_init() in main.c. Every core then calls
// rg_system_reinit() with its own rate (Stella 22050, Lynx 32000, 5200 32000, 7800 ~31440).
#define AUDIO_SAMPLE_RATE   (32000)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 50 + 1)

#ifdef __cplusplus
extern "C" {
#endif

void lynx_main(void);
void a26_main(void);
void a52_main(void);
void a78_main(void);

#ifdef __cplusplus
}
#endif
