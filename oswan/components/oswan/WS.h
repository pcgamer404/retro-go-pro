/*
$Date: 2009-10-30 05:26:46 +0100 (ven., 30 oct. 2009) $
$Rev: 71 $
*/

#ifndef WS_H_
#define WS_H_

#include "WSHard.h"
#include <stdbool.h>

struct EEPROM
{
    WORD *data;
    int we;
};

extern int Run;
extern BYTE *Page[0x10];
extern BYTE* IRAM;
extern BYTE *IO;
extern BYTE *MemDummy;
extern BYTE **ROMMap;     // C-ROM�o���N�}�b�v
extern int ROMBanks;            // C-ROM�o���N��
extern BYTE **RAMMap;     // C-RAM�o���N�}�b�v
extern int RAMBanks;            // C-RAM�o���N��
extern int RAMSize;             // C-RAM���e��
extern WORD IEep[64];
extern struct EEPROM sIEep;
extern struct EEPROM sCEep;

#define CK_EEP 1
extern int CartKind;

void WriteIO(DWORD A, BYTE V);
void WsReset (void);
void WsRomPatch(BYTE *buf);
int WsRun(bool drawFrame);
void WsSplash(void);
void WsCpyPdata(BYTE* dst);
void Sleep(int);
void SetHVMode(int Mode);

void WsInit(void);
void WsDeInit(void);

#endif
