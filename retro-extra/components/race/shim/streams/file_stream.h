/* libretro file_stream API on top of stdio, for the RACE core. */
#ifndef RACE_FILE_STREAM_SHIM_H
#define RACE_FILE_STREAM_SHIM_H
#include <stdio.h>
#include <stdint.h>
typedef FILE RFILE;
#define RETRO_VFS_FILE_ACCESS_READ            1
#define RETRO_VFS_FILE_ACCESS_WRITE           2
#define RETRO_VFS_FILE_ACCESS_READ_WRITE      3
#define RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING 4
#define RETRO_VFS_FILE_ACCESS_HINT_NONE       0
static inline RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
    (void)hints;
    return fopen(path, (mode & RETRO_VFS_FILE_ACCESS_WRITE) ? "wb" : "rb");
}
static inline int64_t filestream_read(RFILE *f, void *buf, int64_t len) { return fread(buf, 1, len, f); }
static inline int64_t filestream_write(RFILE *f, const void *buf, int64_t len) { return fwrite(buf, 1, len, f); }
static inline int filestream_close(RFILE *f) { return fclose(f); }
static inline int64_t filestream_get_size(RFILE *f) { long p = ftell(f); fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, p, SEEK_SET); return s; }
#endif
