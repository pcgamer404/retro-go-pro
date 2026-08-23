#pragma once

#include <stdint.h>
#include <stdbool.h>

void snes_cheat_init(void);
void snes_cheat_reset(void);
bool snes_cheat_add(const char *name, uint32_t addr, uint8_t val, bool status);
bool snes_cheat_set(uint32_t index, bool status);
bool snes_cheat_del(uint32_t index);
bool snes_cheat_get(uint32_t index, char **name, uint32_t *addr, uint8_t *val, bool *status);
void snes_cheat_apply(void);
bool snes_cheat_decode_par(const char *code, uint32_t *addr, uint8_t *val);
