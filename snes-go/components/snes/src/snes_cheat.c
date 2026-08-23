#include "snes_cheat.h"
#include "memmap.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#define MAX_CHEATS 64

typedef struct {
    char name[64];
    uint32_t addr;
    uint8_t val;
    bool status;
} snes_cheat_t;

static snes_cheat_t cheats[MAX_CHEATS];
static int cheat_count = 0;

void snes_cheat_init(void) {
    cheat_count = 0;
    memset(cheats, 0, sizeof(cheats));
}

void snes_cheat_reset(void) {
    cheat_count = 0;
}

bool snes_cheat_add(const char *name, uint32_t addr, uint8_t val, bool status) {
    if (cheat_count >= MAX_CHEATS) return false;
    strncpy(cheats[cheat_count].name, name, 63);
    cheats[cheat_count].addr = addr;
    cheats[cheat_count].val = val;
    cheats[cheat_count].status = status;
    cheat_count++;
    return true;
}

bool snes_cheat_set(uint32_t index, bool status) {
    if (index >= cheat_count) return false;
    cheats[index].status = status;
    return true;
}

bool snes_cheat_del(uint32_t index) {
    if (index >= cheat_count) return false;
    for (int i = index; i < cheat_count - 1; i++) {
        cheats[i] = cheats[i + 1];
    }
    cheat_count--;
    return true;
}

bool snes_cheat_get(uint32_t index, char **name, uint32_t *addr, uint8_t *val, bool *status) {
    if (index >= cheat_count) return false;
    if (name) *name = cheats[index].name;
    if (addr) *addr = cheats[index].addr;
    if (val) *val = cheats[index].val;
    if (status) *status = cheats[index].status;
    return true;
}

void snes_cheat_apply(void) {
    for (int i = 0; i < cheat_count; i++) {
        if (!cheats[i].status) continue;
        S9xSetByte(cheats[i].val, cheats[i].addr);
    }
}

bool snes_cheat_decode_par(const char *code, uint32_t *addr, uint8_t *val) {
    if (!code || !addr || !val) return false;
    char hex[16];
    int hex_idx = 0;
    for (int i = 0; code[i] && hex_idx < 15; i++) {
        if (isxdigit((unsigned char)code[i])) hex[hex_idx++] = code[i];
    }
    hex[hex_idx] = 0;
    if (hex_idx < 8) return false;
    char val_hex[3];
    strcpy(val_hex, &hex[hex_idx - 2]);
    hex[hex_idx - 2] = 0;
    *addr = strtoul(hex, NULL, 16);
    *val = (uint8_t)strtoul(val_hex, NULL, 16);
    return true;
}
