#include "sms_cheat.h"
#include "shared.h"
#include <ctype.h>
#include <stdlib.h>

#define MAX_CHEATS 64

typedef struct {
    char name[64];
    uint32_t addr;
    uint8_t val;
    int compare;
    bool status;
} sms_cheat_t;

static sms_cheat_t cheats[MAX_CHEATS];
static int cheat_count = 0;

void sms_cheat_init(void) {
    memset(cheats, 0, sizeof(cheats));
    cheat_count = 0;
}

void sms_cheat_reset(void) {
    cheat_count = 0;
}

void sms_cheat_add(const char *name, uint32_t addr, uint8_t val, int compare, bool status) {
    if (cheat_count >= MAX_CHEATS) return;
    
    strncpy(cheats[cheat_count].name, name, 63);
    cheats[cheat_count].addr = addr;
    cheats[cheat_count].val = val;
    cheats[cheat_count].compare = compare;
    cheats[cheat_count].status = status;
    cheat_count++;
}

void sms_cheat_del(uint32_t index) {
    if (index >= cheat_count) return;
    for (int i = index; i < cheat_count - 1; i++) {
        cheats[i] = cheats[i+1];
    }
    cheat_count--;
}

void sms_cheat_set(uint32_t index, bool status) {
    if (index < cheat_count) {
        cheats[index].status = status;
    }
}

bool sms_cheat_get(uint32_t index, char **name, uint32_t *addr, uint8_t *val, int *compare, bool *status) {
    if (index >= cheat_count) return false;
    if (name) *name = cheats[index].name;
    if (addr) *addr = cheats[index].addr;
    if (val) *val = cheats[index].val;
    if (compare) *compare = cheats[index].compare;
    if (status) *status = cheats[index].status;
    return true;
}

void sms_cheat_apply(void) {
    for (int i = 0; i < cheat_count; i++) {
        if (!cheats[i].status) continue;

        uint32_t addr = cheats[i].addr & 0xFFFF;
        uint8_t val = cheats[i].val;
        int compare = cheats[i].compare;

        // 1. Direct RAM patching (SMS RAM is 8KB mirrored at 0xC000-0xFFFF)
        if (addr >= 0xC000) {
            sms.wram[addr & 0x1FFF] = val;
        }
        
        // 2. Page Patching (ROM or specialized RAM)
        uint8_t page = addr >> 10;
        uint8_t offset = addr & 0x03FF;
        if (cpu_readmap[page] && cpu_readmap[page] != (unsigned char *)dummy_memory) {
            // Only patch if it's a GameShark code (compare == -1) 
            // OR if the compare value matches the original ROM data
            if (compare == -1 || cpu_readmap[page][offset] == (uint8_t)compare) {
                cpu_readmap[page][offset] = val;
            }
        }
    }
}

bool sms_cheat_decode_par(const char *code, uint32_t *addr, uint8_t *val) {
    if (!code || !addr || !val) return false;

    char clean[16];
    int j = 0;
    for (int i = 0; code[i] && j < 15; i++) {
        if (isxdigit((int)code[i])) clean[j++] = code[i];
    }
    clean[j] = 0;

    if (j == 6) { // XXXXXX -> XXXX:YY
        uint32_t raw = strtoul(clean, NULL, 16);
        *addr = (raw >> 8) & 0xFFFF;
        *val = raw & 0xFF;
        return true;
    } else if (j == 8) { // XXXXXXXX -> ???
        uint32_t raw = strtoul(clean, NULL, 16);
        *addr = (raw >> 8) & 0xFFFFFF;
        *val = raw & 0xFF;
        return true;
    }

    return false;
}

static int hex2int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

bool sms_cheat_decode_gg(const char *code, uint32_t *addr, uint8_t *val, int *compare) {
    char clean[16];
    int len = 0;
    for (int i = 0; code[i]; i++) {
        if (isxdigit((int)code[i])) clean[len++] = code[i];
    }
    
    if (len != 6 && len != 9) return false;

    int n[9] = {0};
    for (int i = 0; i < len; i++) {
        n[i] = hex2int(clean[i]);
    }

    // Standard Game Boy / Game Gear Game Genie Decoding (ABC-DEF-GHI)
    // Value = AB
    *val = (n[0] << 4) | n[1];
    // Address = (F^0xF)CDE (Digit 6, 3, 4, 5)
    *addr = ((n[5] ^ 0x0F) << 12) | (n[2] << 8) | (n[3] << 4) | n[4];

    if (len == 9) {
        // Compare = GI (Digit 7, 9) -> Rotate Right 2 -> XOR 0xBA
        int comp = (n[6] << 4) | n[8];
        comp = ((comp >> 2) | (comp << 6)) & 0xFF;
        comp ^= 0xBA;
        *compare = comp;
    } else {
        *compare = -1; // No compare for 6-digit codes
    }

    return true;
}
