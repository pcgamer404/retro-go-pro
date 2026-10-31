/* retro-home: the few libretro-common file calls ProSystem makes (the BIOS and
 * the files a .cdf names), on stdio. */
#ifndef PROSYSTEM_FILE_STREAM_SHIM_H
#define PROSYSTEM_FILE_STREAM_SHIM_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef FILE RFILE;
#define RETRO_VFS_FILE_ACCESS_READ 0
#define RETRO_VFS_FILE_ACCESS_HINT_NONE 0

static inline RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
    (void)mode; (void)hints;
    return fopen(path, "rb");
}
static inline int64_t filestream_get_size(RFILE *f)
{
    long at = ftell(f), size;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, at, SEEK_SET);
    return size;
}
static inline int filestream_error(RFILE *f) { return ferror(f); }
static inline int filestream_close(RFILE *f) { return fclose(f); }
static inline int64_t rfread(void *buffer, size_t elem_size, size_t elem_count, RFILE *f)
{
    return (int64_t)fread(buffer, elem_size, elem_count, f);
}
static inline int filestream_read_file(const char *path, void **buf, int64_t *len)
{
    FILE *f = fopen(path, "rb");
    *buf = NULL;
    *len = 0;
    if (!f) return 0;
    int64_t size = filestream_get_size(f);
    *buf = malloc(size + 1);
    if (*buf && fread(*buf, 1, size, f) == (size_t)size) *len = size;
    fclose(f);
    return *len > 0;
}
#endif
