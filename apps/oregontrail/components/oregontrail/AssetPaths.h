#pragma once
#include <stddef.h>
const char *ot_data_root();
void ot_set_data_root(const char *path);
void ot_build_path(char *out, size_t outSize, const char *relative);
