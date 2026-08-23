/*
$Date: 2009-10-30 05:26:46 +0100 (ven., 30 oct. 2009) $
$Rev: 71 $
*/

#ifndef WSFILEIO_H_
#define WSFILEIO_H_

#include <stddef.h>
#include <stdint.h>


void WsSetDir(char *path);
int WsCreate(char *CartName);
int WsCreateFromMemory(const uint8_t *romData, size_t romSize); // XIP no-copy
void WsRelease(void);
void WsLoadIEep(void);
void WsSaveIEep(void);
int WsLoadState(const char *filename);
int WsSaveState(const char *filename);

#endif
