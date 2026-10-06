#pragma once

#include <stdint.h>
#include <stdbool.h>

void sms_cheat_init(void);
void sms_cheat_reset(void);
void sms_cheat_add(const char *name, uint32_t addr, uint8_t val, int compare, bool status);
void sms_cheat_del(uint32_t index);
void sms_cheat_set(uint32_t index, bool status);
bool sms_cheat_get(uint32_t index, char **name, uint32_t *addr, uint8_t *val, int *compare, bool *status);
void sms_cheat_apply(void);
bool sms_cheat_decode_par(const char *code, uint32_t *addr, uint8_t *val);
bool sms_cheat_decode_gg(const char *code, uint32_t *addr, uint8_t *val, int *compare);
