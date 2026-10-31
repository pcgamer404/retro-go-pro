/* retro-home: the platform calls MCUME's 5200 core makes (MCUME's emuapi.h
 * on the ESP32 had them on its own display, keys and SD code); main_a52.c
 * implements them on retro-go. The masks are MCUME's. */
#ifndef EMUAPI_H
#define EMUAPI_H
#include <stdint.h>

#define HAS_SND 1
#define PALETTE_SIZE 256
#define R32(rgb) ((rgb >> 16) & 0xff)
#define G32(rgb) ((rgb >> 8) & 0xff)
#define B32(rgb) (rgb & 0xff)

#define MASK_JOY2_RIGHT 0x0001
#define MASK_JOY2_LEFT  0x0002
#define MASK_JOY2_UP    0x0004
#define MASK_JOY2_DOWN  0x0008
#define MASK_JOY2_BTN   0x0010
#define MASK_KEY_USER1  0x0020
#define MASK_KEY_USER2  0x0040
#define MASK_KEY_USER3  0x0080

#ifdef __cplusplus
extern "C" {
#endif
void emu_printf(char *text);
void emu_printi(int val);
void *emu_Malloc(int size);
void emu_Free(void *pt);
void *emu_TmpMemory(void);
int emu_FileOpen(char *filename);
int emu_FileGetc(void);
int emu_FileSeek(int seek);
void emu_FileClose(void);
int emu_FileSize(char *filename);
int emu_ReadKeys(void);
int emu_GetPad(void);
int emu_ReadAnalogJoyX(int min, int max);
int emu_ReadAnalogJoyY(int min, int max);
void emu_SetPaletteEntry(unsigned char r, unsigned char g, unsigned char b, int index);
void emu_DrawLine(unsigned char *buf, int width, int height, int line);
void emu_DrawVsync(void);
void emu_sndInit(void);
void emu_sndPlaySound(int chan, int volume, int freq);
unsigned long calc_crc32(unsigned char *buf, int len);
#ifdef __cplusplus
}
#endif
#endif
