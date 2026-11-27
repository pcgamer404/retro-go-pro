/**
 * @file board_config.h
 * @brief Configuration for Command & Conquer on Retro-Go (ESP32-S3).
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define LCD_WIDTH           320
#define LCD_HEIGHT          240

#define SD_MOUNT_POINT      "/sd"

#ifdef CNC_GAME_RA
#define CNC_DEFAULT_DATA_DIR    SD_MOUNT_POINT "/roms/ra"
#define CNC_CHECK_FILE          "REDALERT.MIX"
#define CNC_PROGRAM_NAME        "vanillara"
#else
#define CNC_DEFAULT_DATA_DIR    SD_MOUNT_POINT "/roms/cnc"
#define CNC_CHECK_FILE          "CONQUER.MIX"
#define CNC_PROGRAM_NAME        "vanillatd"
#endif

#ifdef __cplusplus
extern "C" {
#endif
const char* cnc_get_data_dir(void);
void cnc_set_data_dir(const char* dir);
#ifdef __cplusplus
}
#endif

#define CNC_DATA_DIR cnc_get_data_dir()

#define AUDIO_SAMPLE_RATE   22050

#endif // BOARD_CONFIG_H
