/*
$Date: 2009-10-30 05:26:46 +0100 (ven., 30 oct. 2009) $
$Rev: 71 $
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WS.h"
#include "WSApu.h"
#include "WSFileio.h"
#include "WSHard.h"
#include "WSRender.h"
#include "cpu/necintrf.h"

extern WORD HTimer, VTimer;
extern int RtcCount, RAMEnable, HVMode;
extern WORD MonoColor[8];

#include <rg_system.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

// Last load error, shown by main.c instead of panicking/rebooting.
static char ws_last_error[160] = "Unknown error";
static bool rom_is_xip = false;

// ROMs larger than the free PSRAM are streamed from SD in 64 KB banks. The
// emulator maps segments 2..F to relative bank numbers, so the cache is keyed
// by relative bank number and keeps a small LRU history. A bank that is still
// referenced by a Page[] pointer is never evicted.
#define WS_ROM_BANK_SIZE 0x10000
// Up to 6 MB of bank cache. The allocator still keeps 1 MB of PSRAM free for
// save states and screenshots, and falls back to fewer slots when memory is
// tight, so this is a ceiling rather than a fixed reservation.
#define WS_ROM_CACHE_MAX 96
#define WS_ROM_CACHE_MIN 16

static bool rom_is_streaming = false;
static FILE *WsRomFile = NULL;
static long WsRomDataOffset = 0;
static BYTE *WsRomCache[WS_ROM_CACHE_MAX];
static int WsRomCacheBank[WS_ROM_CACHE_MAX];
static uint32_t WsRomCacheAge[WS_ROM_CACHE_MAX];
static int WsRomSlotRefs[WS_ROM_CACHE_MAX];
static int WsRomPageSlot[16];
static int WsRomCacheSlots = 0;
static uint32_t WsRomCacheClock = 0;
static bool ws_stream_read_error = false;

const char *WsGetLastError(void) { return ws_last_error; }

static int WsFail(const char *message) {
  snprintf(ws_last_error, sizeof(ws_last_error), "%s", message);
  RG_LOGE("[WS] Load error: %s", message);
  return -1;
}

// Saved HTML error pages are a common cause of "ROM" files that are only a
// few KB and produce nonsense footer bytes. Detect them early so the user gets
// a useful message instead of a crash.
static bool WsLooksLikeHtml(const uint8_t *data, size_t size) {
  size_t i = 0;
  size_t n = size < 256 ? size : 256;
  // Skip a UTF-8 BOM and leading whitespace.
  if (n >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
    i = 3;
  while (i < n && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' ||
                   data[i] == '\n'))
    i++;
  return i < n && data[i] == '<';
}

static bool WsFileLooksLikeHtml(FILE *fp, long offset) {
  uint8_t head[256];
  if (fseek(fp, offset, SEEK_SET) != 0)
    return false;
  size_t n = fread(head, 1, sizeof(head), fp);
  return WsLooksLikeHtml(head, n);
}

static int WsGetFooterBanks(BYTE flag) {
  switch (flag) {
  case 1:
    return 4;
  case 2:
    return 8;
  case 3:
    return 16;
  case 4:
    return 32;
  case 5:
    return 48;
  case 6:
    return 64;
  case 7:
    return 96;
  case 8:
    return 128;
  case 9:
    return 256;
  default:
    return 0;
  }
}

static void WsSetupRam(BYTE flag) {
  switch (flag) {
  case 0x01:
    RAMBanks = 1;
    RAMSize = 0x2000;
    CartKind = 0;
    break;
  case 0x02:
    RAMBanks = 1;
    RAMSize = 0x8000;
    CartKind = 0;
    break;
  case 0x03:
    RAMBanks = 2;
    RAMSize = 0x20000;
    CartKind = 0;
    break;
  case 0x04:
    RAMBanks = 4;
    RAMSize = 0x40000;
    CartKind = 0;
    break;
  case 0x05:
    RAMBanks = 8;
    RAMSize = 0x80000;
    CartKind = 0;
    break;
  case 0x10:
    RAMBanks = 1;
    RAMSize = 0x80;
    CartKind = CK_EEP;
    break; /* 128 B */
  case 0x20:
    RAMBanks = 1;
    RAMSize = 0x800;
    CartKind = CK_EEP;
    break; /* 2 KiB  */
  case 0x50:
    RAMBanks = 1;
    RAMSize = 0x400;
    CartKind = CK_EEP;
    break; /* 1 KiB  */
  default:
    /* Force 64KB SRAM if unspecified/unknown (fixes games with bad headers) */
    RAMBanks = 1;
    RAMSize = 0x10000;
    CartKind = 0;
    break;
  }
}

static int WsAllocCartRam(void) {
  for (int i = 0; i < 256; i++)
    RAMMap[i] = MemDummy;
  for (int i = 0; i < RAMBanks; i++) {
    size_t alloc_sz = (CartKind == CK_EEP) ? ((RAMSize + 15) & ~15) : 0x10000;
    RAMMap[i] = (BYTE *)malloc(alloc_sz);
    if (!RAMMap[i]) {
      for (int k = 0; k < i; k++) {
        if (RAMMap[k] != MemDummy)
          free(RAMMap[k]);
        RAMMap[k] = MemDummy;
      }
      return WsFail("Out of memory for cartridge RAM.");
    }
    memset(RAMMap[i], 0, alloc_sz);
  }
  return 0;
}

static void *WsCacheAlloc(void) {
#if defined(ESP_PLATFORM)
  return heap_caps_malloc(WS_ROM_BANK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(WS_ROM_BANK_SIZE);
#endif
}

static int WsAllocRomCache(void) {
  size_t free_psram = 0;
#if defined(ESP_PLATFORM)
  free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
  // Keep 1 MB free for save-state buffers and other runtime allocations.
  int slots = (free_psram > 1024 * 1024)
                  ? (int)((free_psram - 1024 * 1024) / WS_ROM_BANK_SIZE)
                  : 0;
  if (slots > WS_ROM_CACHE_MAX)
    slots = WS_ROM_CACHE_MAX;
  if (slots < WS_ROM_CACHE_MIN)
    return WsFail("Not enough PSRAM to stream this ROM. Try a smaller ROM.");

  for (; slots >= WS_ROM_CACHE_MIN; slots--) {
    BYTE *tmp[WS_ROM_CACHE_MAX] = {0};
    bool ok = true;
    for (int i = 0; i < slots; i++) {
      tmp[i] = (BYTE *)WsCacheAlloc();
      if (!tmp[i]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      for (int i = 0; i < slots; i++) {
        WsRomCache[i] = tmp[i];
        WsRomCacheBank[i] = -1;
        WsRomCacheAge[i] = 0;
        WsRomSlotRefs[i] = 0;
      }
      WsRomCacheSlots = slots;
      for (int i = 0; i < 16; i++)
        WsRomPageSlot[i] = -1;
      WsRomCacheClock = 0;
      ws_stream_read_error = false;
      return 0;
    }
    for (int i = 0; i < slots; i++) {
      if (tmp[i])
        free(tmp[i]);
    }
  }
  return WsFail("Not enough PSRAM to stream this ROM.");
}

static void WsStreamClose(void) {
  if (WsRomFile) {
    fclose(WsRomFile);
    WsRomFile = NULL;
  }
  for (int i = 0; i < WsRomCacheSlots; i++) {
    if (WsRomCache[i]) {
      free(WsRomCache[i]);
      WsRomCache[i] = NULL;
    }
    WsRomCacheBank[i] = -1;
    WsRomSlotRefs[i] = 0;
  }
  WsRomCacheSlots = 0;
  for (int i = 0; i < 16; i++)
    WsRomPageSlot[i] = -1;
  rom_is_streaming = false;
  WsRomDataOffset = 0;
  ws_stream_read_error = false;
  if (ROMMap) {
    for (int i = 0; i < 256; i++)
      ROMMap[i] = MemDummy;
  }
}

// The WonderSwan masks the 8-bit bank number with (padded_banks - 1), where
// padded_banks is the next power of two >= the cartridge size. The ROM is
// right-aligned in that address space, so banks below the padding read open
// bus. This is why a 4 MB game can write C0=0x00 or C0=0x0C and get the same
// data,
// and why an 8 MB game's C0=0x00..0x07 must map real banks instead of open
// bus. ROMMap and the SD cache are indexed by the resulting physical bank.
static int WsBankToPhysical(int bank) {
  if (ROMBanks <= 0 || ROMBanks > 256 || bank < 0 || bank > 0xFF)
    return -1;
  int padded = 1;
  while (padded < ROMBanks)
    padded <<= 1;
  int padding = padded - ROMBanks;
  int phys = bank & (padded - 1);
  if (phys < padding)
    return -1;
  return phys - padding;
}

void WsSetRomPage(int segment, int bank) {
  if (segment < 0 || segment > 15)
    return;
  if (!ROMMap) {
    Page[segment] = MemDummy;
    return;
  }

  int phys = WsBankToPhysical(bank);

  if (!rom_is_streaming) {
    Page[segment] = (phys >= 0 && ROMMap[phys]) ? ROMMap[phys] : MemDummy;
    return;
  }

  int old_slot = WsRomPageSlot[segment];
  if (old_slot >= 0) {
    if (WsRomSlotRefs[old_slot] > 0)
      WsRomSlotRefs[old_slot]--;
    WsRomPageSlot[segment] = -1;
  }

  if (phys < 0 || !WsRomFile) {
    Page[segment] = MemDummy;
    return;
  }

  int slot = -1;
  for (int i = 0; i < WsRomCacheSlots; i++) {
    if (WsRomCacheBank[i] == phys) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    int lru = -1;
    uint32_t lru_age = 0xFFFFFFFFu;
    for (int i = 0; i < WsRomCacheSlots; i++) {
      if (WsRomCacheBank[i] < 0) {
        lru = i;
        break;
      }
      if (WsRomSlotRefs[i] == 0 && WsRomCacheAge[i] < lru_age) {
        lru_age = WsRomCacheAge[i];
        lru = i;
      }
    }
    if (lru < 0) {
      Page[segment] = MemDummy;
      return;
    }
    slot = lru;

    long offset = WsRomDataOffset + (long)phys * WS_ROM_BANK_SIZE;
    if (fseek(WsRomFile, offset, SEEK_SET) != 0 ||
        fread(WsRomCache[slot], 1, WS_ROM_BANK_SIZE, WsRomFile) !=
            WS_ROM_BANK_SIZE) {
      if (!ws_stream_read_error) {
        ws_stream_read_error = true;
        RG_LOGE("[WS] SD read failed at bank %d; streaming stopped", phys);
      }
      WsRomCacheBank[slot] = -1;
      Page[segment] = MemDummy;
      return;
    }
    WsRomCacheBank[slot] = phys;
  }

  WsRomCacheAge[slot] = ++WsRomCacheClock;
  WsRomSlotRefs[slot]++;
  WsRomPageSlot[segment] = slot;
  Page[segment] = WsRomCache[slot];
}

void WsLoadSRAM(void) {
  if (RAMBanks == 0)
    return;
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, rg_system_get_app()->romPath);
  if (!path)
    return;

  void *data = NULL;
  size_t size = 0;
  if (rg_storage_read_file(path, &data, &size, 0)) {
    size_t total_ram = RAMBanks * 0x10000;
    if (CartKind == CK_EEP)
      total_ram = RAMSize;

    if (size > total_ram)
      size = total_ram;

    // Load into banks
    size_t remaining = size;
    for (int i = 0; i < RAMBanks && remaining > 0; i++) {
      size_t to_copy = (remaining > 0x10000) ? 0x10000 : remaining;
      memcpy(RAMMap[i], (uint8_t *)data + (i * 0x10000), to_copy);
      remaining -= to_copy;
    }
    free(data);
  }
  free(path);
}

void WsSaveSRAM(void) {
  if (RAMBanks == 0)
    return;
  char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, rg_system_get_app()->romPath);
  if (!path)
    return;

  size_t total_size = (CartKind == CK_EEP) ? RAMSize : (RAMBanks * 0x10000);
  uint8_t *buffer = malloc(total_size);
  if (!buffer) {
    free(path);
    return;
  }

  size_t remaining = total_size;
  for (int i = 0; i < RAMBanks && remaining > 0; i++) {
    size_t to_copy = (remaining > 0x10000) ? 0x10000 : remaining;
    memcpy(buffer + (i * 0x10000), RAMMap[i], to_copy);
    remaining -= to_copy;
  }

  rg_storage_write_file(path, buffer, total_size, 0);
  free(buffer);
  free(path);
}

static char *IEepPath = "./oswan-od.dat";

void WsRelease(void) {
  int i;

  // Cartridge RAM is always owned by the emulator.
  for (i = 0; i < RAMBanks; i++) {
    if (RAMMap[i] && RAMMap[i] != MemDummy) {
      free(RAMMap[i]);
    }
    RAMMap[i] = MemDummy;
  }

  // ROM data is either XIP (a pointer into the ROM buffer owned by the app) or
  // a banked SD cache. Never free XIP pointers, and let WsStreamClose own the
  // cache slots.
  if (rom_is_streaming) {
    WsStreamClose();
  } else if (!rom_is_xip) {
    for (i = 0; i < 256; i++) {
      if (ROMMap[i] && ROMMap[i] != MemDummy) {
        free(ROMMap[i]);
      }
      ROMMap[i] = MemDummy;
    }
  }
  for (i = 0; i < 256; i++)
    ROMMap[i] = MemDummy;
  rom_is_xip = false;
}

int WsCreateFromMemory(const uint8_t *romData, size_t romSize) {
  BYTE footer[10];
  int i;
  size_t header = 0;

  ws_last_error[0] = 0;
  WsStreamClose();
  rom_is_xip = false;

  if (!romData || romSize < 10)
    return WsFail("Not a WonderSwan ROM (file is too small).");

  // Some dumps have a 512-byte copier header. Strip it before mapping banks.
  if (romSize > 512 && (romSize & 0xFFFF) == 512)
    header = 512;

  const uint8_t *rom = romData + header;
  size_t dataSize = romSize - header;

  memcpy(footer, romData + romSize - 10, 10);

  int footerBanks = WsGetFooterBanks(footer[4]);
  if (footerBanks) {
    size_t expected = (size_t)footerBanks << 16;
    if (dataSize < expected) {
      if (WsLooksLikeHtml(romData, romSize))
        return WsFail("This file is an HTML page, not a WonderSwan ROM.");
      return WsFail("ROM is truncated (file is smaller than its header says).");
    }
    ROMBanks = footerBanks;
  } else {
    if ((dataSize & 0xFFFF) != 0 || dataSize < 0x10000 ||
        dataSize > (16u << 20)) {
      if (WsLooksLikeHtml(romData, romSize))
        return WsFail("This file is an HTML page, not a WonderSwan ROM.");
      return WsFail("Not a valid WonderSwan ROM (bad size/footer).");
    }
    ROMBanks = (int)(dataSize >> 16);
  }
  if (ROMBanks <= 0 || ROMBanks > 256)
    return WsFail("Unsupported ROM size (max 16 MB).");

  WsSetupRam(footer[5]);
  WsRomPatch((BYTE *)footer);

  /* Direct ROM XIP mapping, indexed by physical bank. */
  for (i = 0; i < 256; ++i)
    ROMMap[i] = MemDummy;
  for (i = 0; i < ROMBanks; ++i) {
    size_t ofs = ((size_t)i) << 16; // i * 64K
    ROMMap[i] = (ofs < dataSize) ? (BYTE *)(rom + ofs) : MemDummy;
  }
  rom_is_xip = true;

  if (WsAllocCartRam() != 0)
    return -1;

  WsReset();
  SetHVMode(footer[6] & 1); /* 0: H, 1: V */

  RG_LOGI("[WS] ROM: %u bytes, %d banks, XIP (header=%u)",
          (unsigned)dataSize, ROMBanks, (unsigned)header);
  return 0;
}

int WsCreateFromFile(const char *path) {
  BYTE footer[10];
  int i;
  size_t header = 0;
  long fileSize;
  int footerBanks;

  ws_last_error[0] = 0;
  WsStreamClose();
  rom_is_xip = false;

  if (!path || !*path)
    return WsFail("No ROM file specified.");

  WsRomFile = fopen(path, "rb");
  if (!WsRomFile)
    return WsFail("Could not open the ROM file.");

  if (fseek(WsRomFile, 0, SEEK_END) != 0 ||
      (fileSize = ftell(WsRomFile)) < 10) {
    fclose(WsRomFile);
    WsRomFile = NULL;
    return WsFail("Not a WonderSwan ROM (file is too small).");
  }

  if (fseek(WsRomFile, fileSize - 10, SEEK_SET) != 0 ||
      fread(footer, 1, 10, WsRomFile) != 10) {
    fclose(WsRomFile);
    WsRomFile = NULL;
    return WsFail("Could not read the ROM header.");
  }

  if (fileSize > 512 && (fileSize & 0xFFFF) == 512)
    header = 512;

  size_t dataSize = (size_t)fileSize - header;
  footerBanks = WsGetFooterBanks(footer[4]);

  if (footerBanks) {
    size_t expected = (size_t)footerBanks << 16;
    if (dataSize < expected) {
      fclose(WsRomFile);
      WsRomFile = NULL;
      return WsFail("ROM is truncated (file is smaller than its header says).");
    }
    ROMBanks = footerBanks;
  } else {
    if (WsFileLooksLikeHtml(WsRomFile, (long)header)) {
      fclose(WsRomFile);
      WsRomFile = NULL;
      return WsFail("This file is an HTML page, not a WonderSwan ROM.");
    }
    if ((dataSize & 0xFFFF) != 0 || dataSize < 0x10000 ||
        dataSize > (16u << 20)) {
      fclose(WsRomFile);
      WsRomFile = NULL;
      return WsFail("Not a valid WonderSwan ROM (bad size/footer).");
    }
    ROMBanks = (int)(dataSize >> 16);
  }
  if (ROMBanks <= 0 || ROMBanks > 256) {
    fclose(WsRomFile);
    WsRomFile = NULL;
    return WsFail("Unsupported ROM size (max 16 MB).");
  }

  WsSetupRam(footer[5]);
  WsRomPatch(footer);
  WsRomDataOffset = (long)header;

  if (WsAllocCartRam() != 0) {
    fclose(WsRomFile);
    WsRomFile = NULL;
    return -1;
  }

  if (WsAllocRomCache() != 0) {
    for (i = 0; i < 256; i++) {
      if (RAMMap[i] && RAMMap[i] != MemDummy)
        free(RAMMap[i]);
      RAMMap[i] = MemDummy;
    }
    fclose(WsRomFile);
    WsRomFile = NULL;
    return -1;
  }

  rom_is_streaming = true;
  for (i = 0; i < 256; i++)
    ROMMap[i] = MemDummy;

  WsReset();
  if (Page[0xF] == MemDummy) {
    WsStreamClose();
    for (i = 0; i < 256; i++) {
      if (RAMMap[i] && RAMMap[i] != MemDummy)
        free(RAMMap[i]);
      RAMMap[i] = MemDummy;
    }
    return WsFail("Could not read the ROM from SD.");
  }
  SetHVMode(footer[6] & 1); /* 0: H, 1: V */

  RG_LOGI("[WS] ROM: %u bytes, %d banks, streaming from SD (%d x 64KB cache)",
          (unsigned)dataSize, ROMBanks, WsRomCacheSlots);
  return 0;
}

void WsLoadIEep(void) {
  FILE *fp;

  if ((fp = fopen(IEepPath, "rb")) != NULL) {
    fread(IEep, sizeof(WORD), 64, fp);
    fclose(fp);
  } else {
    WORD *p = IEep + 0x30;
    memset(IEep, 0xFF, 0x60);
    memset(p, 0, 0x20);
    *p++ = 0x211D;
    *p++ = 0x180B;
    *p++ = 0x1C0D;
    *p++ = 0x1D23;
    *p++ = 0x0B1E;
    *p = 0x0016;
  }
}

void WsSaveIEep(void) {
  FILE *fp;

  if ((fp = fopen(IEepPath, "wb")) != NULL) {
    fwrite(IEep, sizeof(WORD), 64, fp);
    fclose(fp);
  }
}

// Number of bytes stored after the per-bank work RAM (palette, timers and APU).
static long WsStateTrailerSize(void) {
  return (long)(sizeof(WORD) * 16 * 16) + // Palette
         (long)(sizeof(WORD) * 2) +       // HTimer, VTimer
         (long)(sizeof(int) * 3) +        // RtcCount, RAMEnable, HVMode
         (long)(sizeof(WORD) * 8) +       // MonoColor
         (long)sizeof(unsigned long) +    // WaveMap
         (long)sizeof(int) +              // VoiceOn
         (long)(sizeof(SOUND) * 4) +      // Ch
         (long)sizeof(SWEEP) +            // Swp
         (long)sizeof(NOISE);             // Noise
}

#define WS_STATE_APU_SIZE                                                      \
  ((long)(sizeof(unsigned long) + sizeof(int) + sizeof(SOUND) * 4 +            \
          sizeof(SWEEP) + sizeof(NOISE)))

// Save states written by older build don't record how much work RAM the
// cartridge had. The old code always wrote the RAMSize of the build that
// created the file, so a state made when this ROM reported 64KB won't line up
// with the current 32KB layout. Accept the RAM sizes the emulator can map.
static bool WsStateRamSizePlausible(long bytes) {
  switch (bytes) {
  case 0x0:
  case 0x80:
  case 0x400:
  case 0x800:
  case 0x2000:
  case 0x8000:
  case 0x10000:
    return true;
  default:
    return false;
  }
}

#define MacroLoadNecRegisterFromFile(F, R)                                     \
  fread(&value, sizeof(unsigned int), 1, fp);                                  \
  nec_set_reg(R, value);
int WsLoadState(const char *filename) {
  FILE *fp;
  unsigned int value;
  int i;
  long file_size = -1;
  long ram_per_bank = -1;
  bool has_apu = true;

  if ((fp = fopen(filename, "rb")) == NULL) {
    return 0;
  }

  // Determine the per-bank RAM payload from the file length. This recovers
  // states saved with a different RAMSize than the current cartridge.
  if (fseek(fp, 0, SEEK_END) == 0) {
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
  }
  if (file_size > 0 && RAMBanks > 0) {
    long prefix = (long)(18 * sizeof(unsigned int)) + 0x10000 + 0x100;
    long trailer = WsStateTrailerSize();
    long payload = file_size - prefix - trailer;
    if (payload >= 0 && (payload % RAMBanks) == 0 &&
        WsStateRamSizePlausible(payload / RAMBanks)) {
      ram_per_bank = payload / RAMBanks;
    } else {
      // Some very old states did not store the APU block at all.
      long payload2 = file_size - prefix - (trailer - WS_STATE_APU_SIZE);
      if (payload2 >= 0 && (payload2 % RAMBanks) == 0 &&
          WsStateRamSizePlausible(payload2 / RAMBanks)) {
        ram_per_bank = payload2 / RAMBanks;
        has_apu = false;
      }
    }
  }
  if (ram_per_bank < 0) {
    // Unknown layout; fall back to the current cartridge configuration.
    ram_per_bank = (RAMSize < 0x10000) ? (long)RAMSize : 0x10000L;
    has_apu = true;
  }
  RG_LOGD("[WS] State load: file=%ld bytes, RAM=%ld bytes/bank x%d "
          "(current layout=%ld), APU=%d",
          file_size, ram_per_bank, RAMBanks,
          (long)(RAMSize < 0x10000 ? RAMSize : 0x10000), has_apu);

  MacroLoadNecRegisterFromFile(fp, NEC_IP);
  MacroLoadNecRegisterFromFile(fp, NEC_AW);
  MacroLoadNecRegisterFromFile(fp, NEC_BW);
  MacroLoadNecRegisterFromFile(fp, NEC_CW);
  MacroLoadNecRegisterFromFile(fp, NEC_DW);
  MacroLoadNecRegisterFromFile(fp, NEC_CS);
  MacroLoadNecRegisterFromFile(fp, NEC_DS);
  MacroLoadNecRegisterFromFile(fp, NEC_ES);
  MacroLoadNecRegisterFromFile(fp, NEC_SS);
  MacroLoadNecRegisterFromFile(fp, NEC_IX);
  MacroLoadNecRegisterFromFile(fp, NEC_IY);
  MacroLoadNecRegisterFromFile(fp, NEC_BP);
  MacroLoadNecRegisterFromFile(fp, NEC_SP);
  MacroLoadNecRegisterFromFile(fp, NEC_FLAGS);
  MacroLoadNecRegisterFromFile(fp, NEC_VECTOR);
  MacroLoadNecRegisterFromFile(fp, NEC_PENDING);
  MacroLoadNecRegisterFromFile(fp, NEC_NMI_STATE);
  MacroLoadNecRegisterFromFile(fp, NEC_IRQ_STATE);
  fread(IRAM, sizeof(BYTE), 0x10000, fp);
  fread(IO, sizeof(BYTE), 0x100, fp);
  for (i = 0; i < RAMBanks; i++) {
    // EEPROM carts allocate only the (rounded) RAMSize; all other carts get a
    // full 64KB work-RAM bank.
    const size_t bank_alloc =
        (CartKind == CK_EEP) ? (((size_t)RAMSize + 15) & ~(size_t)15) : 0x10000;
    BYTE *bank = RAMMap[i];
    if (!bank || bank == MemDummy || bank_alloc == 0) {
      // No memory to load into; skip the payload so later sections stay aligned.
      if (ram_per_bank > 0)
        fseek(fp, ram_per_bank, SEEK_CUR);
      continue;
    }
    if (ram_per_bank >= (long)bank_alloc) {
      fread(bank, 1, bank_alloc, fp);
      if (ram_per_bank > (long)bank_alloc)
        fseek(fp, ram_per_bank - (long)bank_alloc, SEEK_CUR);
    } else {
      if (ram_per_bank > 0)
        fread(bank, 1, (size_t)ram_per_bank, fp);
      memset(bank + (ram_per_bank > 0 ? ram_per_bank : 0), 0,
             bank_alloc - (ram_per_bank > 0 ? (size_t)ram_per_bank : 0));
    }
  }
  fread(Palette, sizeof(WORD), 16 * 16, fp);
  // Missing HW state
  fread(&HTimer, sizeof(WORD), 1, fp);
  fread(&VTimer, sizeof(WORD), 1, fp);
  fread(&RtcCount, sizeof(int), 1, fp);
  fread(&RAMEnable, sizeof(int), 1, fp);
  fread(&HVMode, sizeof(int), 1, fp);
  fread(MonoColor, sizeof(WORD), 8, fp);
  // APU state (absent from some very old states)
  if (has_apu) {
    fread(&WaveMap, sizeof(unsigned long), 1, fp);
    fread(&VoiceOn, sizeof(int), 1, fp);
    fread(Ch, sizeof(SOUND), 4, fp);
    fread(&Swp, sizeof(SWEEP), 1, fp);
    fread(&Noise, sizeof(NOISE), 1, fp);
  }
  // A short/truncated state would have misaligned every section above.
  const bool truncated = ferror(fp) || feof(fp);
  fclose(fp);
  if (truncated) {
    RG_LOGW("[WS] State load failed: file is truncated or has an unknown layout");
    return 0;
  }

  WriteIO(0x07, IO[0x07]); // tilemap pointers
  WriteIO(0x15, IO[0x15]); // segments
  WriteIO(0xC0, IO[0xC0]); // bank 0x4-0xF
  WriteIO(0xC1, IO[0xC1]); // bank 0x1
  WriteIO(0xC2, IO[0xC2]); // bank 0x2
  WriteIO(0xC3, IO[0xC3]); // bank 0x3
  for (i = 0x80; i <= 0x90; i++) {
    WriteIO(i, IO[i]);
  }

  // The state was captured mid-frame. Restart the renderer at the top of the
  // frame so the first frame after loading is always drawn from scratch
  // instead of mixing the pre-load image with the restored state.
  IO[0x02] = 0; // RSTRL
  // Never resume with the emulated LCD asleep; otherwise the renderer would
  // silently skip frames and the old image would remain on screen.
  IO[0x14] |= 0x01; // LCDSLP
  nec_poll_reset();
  RG_LOGD("[WS] State applied: DSPCTL=%02X COLCTL=%02X LCDSLP=%02X "
          "RAMEnable=%d HVMode=%d",
          DSPCTL, COLCTL, LCDSLP, RAMEnable, HVMode);
  return 1;
}

#define MacroStoreNecRegisterToFile(F, R)                                      \
  value = nec_get_reg(R);                                                      \
  fwrite(&value, sizeof(unsigned int), 1, fp);
int WsSaveState(const char *filename) {
  FILE *fp;
  unsigned int value;
  int i;

  if ((fp = fopen(filename, "wb")) == NULL) {
    return 0;
  }
  MacroStoreNecRegisterToFile(fp, NEC_IP);
  MacroStoreNecRegisterToFile(fp, NEC_AW);
  MacroStoreNecRegisterToFile(fp, NEC_BW);
  MacroStoreNecRegisterToFile(fp, NEC_CW);
  MacroStoreNecRegisterToFile(fp, NEC_DW);
  MacroStoreNecRegisterToFile(fp, NEC_CS);
  MacroStoreNecRegisterToFile(fp, NEC_DS);
  MacroStoreNecRegisterToFile(fp, NEC_ES);
  MacroStoreNecRegisterToFile(fp, NEC_SS);
  MacroStoreNecRegisterToFile(fp, NEC_IX);
  MacroStoreNecRegisterToFile(fp, NEC_IY);
  MacroStoreNecRegisterToFile(fp, NEC_BP);
  MacroStoreNecRegisterToFile(fp, NEC_SP);
  MacroStoreNecRegisterToFile(fp, NEC_FLAGS);
  MacroStoreNecRegisterToFile(fp, NEC_VECTOR);
  MacroStoreNecRegisterToFile(fp, NEC_PENDING);
  MacroStoreNecRegisterToFile(fp, NEC_NMI_STATE);
  MacroStoreNecRegisterToFile(fp, NEC_IRQ_STATE);
  fwrite(IRAM, sizeof(BYTE), 0x10000, fp);
  fwrite(IO, sizeof(BYTE), 0x100, fp);
  for (i = 0; i < RAMBanks; i++) {
    if (RAMSize < 0x10000) {
      fwrite(RAMMap[i], 1, RAMSize, fp);
    } else {
      fwrite(RAMMap[i], 1, 0x10000, fp);
    }
  }
  fwrite(Palette, sizeof(WORD), 16 * 16, fp);
  // Missing HW state
  fwrite(&HTimer, sizeof(WORD), 1, fp);
  fwrite(&VTimer, sizeof(WORD), 1, fp);
  fwrite(&RtcCount, sizeof(int), 1, fp);
  fwrite(&RAMEnable, sizeof(int), 1, fp);
  fwrite(&HVMode, sizeof(int), 1, fp);
  fwrite(MonoColor, sizeof(WORD), 8, fp);
  // APU state
  fwrite(&WaveMap, sizeof(unsigned long), 1, fp);
  fwrite(&VoiceOn, sizeof(int), 1, fp);
  fwrite(Ch, sizeof(SOUND), 4, fp);
  fwrite(&Swp, sizeof(SWEEP), 1, fp);
  fwrite(&Noise, sizeof(NOISE), 1, fp);
  const long file_size = ftell(fp);
  const bool failed = ferror(fp) != 0;
  fclose(fp);
  if (failed) {
    RG_LOGE("[WS] State save failed!");
    return 0;
  }
  RG_LOGD("[WS] State save: file=%ld bytes, RAM=%ld bytes/bank x%d", file_size,
          (long)(RAMSize < 0x10000 ? RAMSize : 0x10000), RAMBanks);
  return 1;
}
