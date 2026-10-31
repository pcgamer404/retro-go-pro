/*
$Date: 2009-10-30 05:26:46 +0100 (ven., 30 oct. 2009) $
$Rev: 71 $
*/

#ifndef WSFILEIO_H_
#define WSFILEIO_H_

#include <stddef.h>
#include <stdint.h>


int WsCreateFromMemory(const uint8_t *romData, size_t romSize); // XIP no-copy
int WsCreateFromFile(const char *path); // banked streaming for ROMs that don't fit
void WsSetRomPage(int segment, int bank);
void WsRelease(void);
void WsLoadIEep(void);
void WsSaveIEep(void);
int WsLoadState(const char *filename);
int WsSaveState(const char *filename);

#endif
