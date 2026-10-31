/* retro-home: the Souper cartridge's extra RAM, out of Memory.c so that it
 * stays in external RAM while Memory.c's tables go to internal RAM. */
#include "Memory.h"

uint8_t memory_souper_ram[MEMORY_SOUPER_EXRAM_SIZE] = {0};
