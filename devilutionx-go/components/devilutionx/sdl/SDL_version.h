#ifndef _SDL_version_h
#define _SDL_version_h

#include "SDL_stdinc.h"

#define SDL_MAJOR_VERSION 1
#define SDL_MINOR_VERSION 2
#define SDL_PATCHLEVEL 15

typedef struct SDL_version {
    Uint8 major;
    Uint8 minor;
    Uint8 patch;
} SDL_version;

#define SDL_VERSION(x) \
{ \
    (x)->major = SDL_MAJOR_VERSION; \
    (x)->minor = SDL_MINOR_VERSION; \
    (x)->patch = SDL_PATCHLEVEL; \
}

#define SDL_VERSIONNUM(X, Y, Z) \
    ((X)*1000 + (Y)*100 + (Z))

#define SDL_COMPILEDVERSION \
    SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL)

#define SDL_VERSION_ATLEAST(X, Y, Z) \
    (SDL_COMPILEDVERSION >= SDL_VERSIONNUM(X, Y, Z))

#ifdef __cplusplus
extern "C" {
#endif

extern DECLSPEC const SDL_version * SDLCALL SDL_Linked_Version(void);

#ifdef __cplusplus
}
#endif

#endif /* _SDL_version_h */
