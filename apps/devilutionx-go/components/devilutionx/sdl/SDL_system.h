#ifndef SDL_system_h_
#define SDL_system_h_

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "SDL_stdinc.h"
#include "SDL_version.h"

#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

int SDL_Init(Uint32 flags);
void SDL_Quit(void);

void SDL_Delay(Uint32 ms);

struct SDL_mutex;
typedef struct SDL_mutex SDL_mutex;

char *** allocateTwoDimenArrayOnHeapUsingMalloc(int row, int col);

#define SDL_mutexP(mutex) SDL_LockMutex(mutex)
#define SDL_mutexV(mutex) SDL_UnlockMutex(mutex)

void SDL_DestroyMutex(SDL_mutex* mutex);
SDL_mutex* SDL_CreateMutex(void);
int SDL_LockMutex(SDL_mutex* mutex);
int SDL_UnlockMutex(SDL_mutex* mutex);

#define MODE_SPI 1
void SDL_InitSD(void);

void Check(const char *str);

// File operations - safely lock/unlock display during operations...
int __mkdir(const char *path, mode_t mode);
FILE *__fopen( const char *path, const char *mode );
long __ftell( FILE *f );
int __feof ( FILE * stream );
int __fputc ( int character, FILE * stream );
int __fgetc ( FILE * stream );
size_t __fwrite ( const void * ptr, size_t size, size_t count, FILE * stream );
int __fclose ( FILE * stream );
int __fseek( FILE * stream, long int offset, int origin );
size_t __fread( void *buffer, size_t size, size_t num, FILE *stream );
int __stat(const char *path, struct stat *buf);

int __open(const char *path, int oflag, ... );
int __close(int fildes);
ssize_t __read(int fildes, void *buf, size_t nbyte);
ssize_t __write(int fildes, const void *buf, size_t nbyte);
off_t __lseek(int fd, off_t offset, int whence);
int __unlink(const char *pathname);

#ifdef __cplusplus
}
#endif

#endif
