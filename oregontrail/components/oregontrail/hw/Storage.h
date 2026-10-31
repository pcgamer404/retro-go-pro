#pragma once
#include <stdint.h>
#include <stddef.h>
namespace storage {
bool begin();
bool loadBlob(const char *path, void *out, size_t len);
bool saveBlob(const char *path, const void *data, size_t len);
void remove(const char *path);
}
