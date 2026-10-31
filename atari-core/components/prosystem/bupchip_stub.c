/* retro-home: the BupChip is not built (it needs the bupboop library and only
 * the .cdf release of "Rikki & Vikki" uses it). A cartridge that asks for it
 * fails to load; nothing else calls these. */
#include "BupChip.h"

unsigned char bupchip_flags;
unsigned char bupchip_volume;
unsigned char bupchip_current_song;
short bupchip_buffer[CORETONE_BUFFER_LEN * 4];

int bupchip_InitFromCDF(const char **cdf, size_t *cdfSize, const char *workingDir)
{
    (void)cdf; (void)cdfSize; (void)workingDir;
    return 0;
}
void bupchip_ProcessAudioCommand(unsigned char data) { (void)data; }
void bupchip_Process(unsigned tick) { (void)tick; }
void bupchip_Release(void) {}
void bupchip_StateLoaded(void) {}
