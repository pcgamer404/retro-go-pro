#include "hw/Storage.h"
#include <config.h>
#include <rg_storage.h>
#include <string.h>
#include <stdlib.h>
namespace storage {
bool begin() { return rg_storage_ready(); }
bool loadBlob(const char *path, void *out, size_t len) {
    if (!begin()) return false;
    void *data = nullptr; size_t size = 0;
    if (!rg_storage_read_file(path, &data, &size, 0) || size != len) { free(data); return false; }
    memcpy(out, data, len); free(data); return true;
}
bool saveBlob(const char *path, const void *data, size_t len) {
    if (!begin()) return false;
    const char *slash = strrchr(path, '/');
    if (slash) { char dir[256]; size_t n=(size_t)(slash-path); if(n>=sizeof(dir)) return false; memcpy(dir,path,n); dir[n]=0; rg_storage_mkdir(dir); }
    return rg_storage_write_file(path, data, len, RG_FILE_ATOMIC_WRITE);
}
void remove(const char *path) { rg_storage_delete(path); }
}
