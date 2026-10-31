/* retro-home: the Atari 5200 BIOS (2 KB at 0xF800) is not part of the source:
 * the app reads it from the card, /retro-go/bios/5200.rom, into this array. */
#ifndef ATARI5200_ROM_H
#define ATARI5200_ROM_H
extern unsigned char BIOSData[2048];
#endif
