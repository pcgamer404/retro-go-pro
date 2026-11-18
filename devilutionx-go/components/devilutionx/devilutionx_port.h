#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Log a lightweight check of the original game data on the SD card. */
bool devilutionx_check_game_data(void);

/** Logs the current integration checkpoint. */
void devilutionx_component_checkpoint(void);

#ifdef __cplusplus
}
#endif
