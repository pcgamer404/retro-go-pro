#include "SDL_system.h"

struct SDL_mutex
{
    pthread_mutex_t id;
#if FAKE_RECURSIVE_MUTEX
    int recursive;
    pthread_t owner;
#endif
};

void SDL_ClearError(void)
{

}

void SDL_Delay(Uint32 ms)
{
    const TickType_t xDelay = ms / portTICK_PERIOD_MS;
    vTaskDelay( xDelay );
}

char *SDL_GetError(void)
{
    return (char *)"";
}

int SDL_Init(Uint32 flags)
{
    if(flags == SDL_INIT_VIDEO)
        SDL_InitSubSystem(flags);
    return 0;
}

void SDL_Quit(void)
{

}

void SDL_InitSD(void)
{
    // retro-go already mounts storage during rg_system_init(), so this
    // is a no-op. Kept only because file.c calls it before first file
    // access.
}

const SDL_version* SDL_Linked_Version()
{
    SDL_version *vers = malloc(sizeof(SDL_version));
    vers->major = SDL_MAJOR_VERSION;                 
    vers->minor = SDL_MINOR_VERSION;                 
    vers->patch = SDL_PATCHLEVEL;      
    return vers;
}

char *** allocateTwoDimenArrayOnHeapUsingMalloc(int row, int col)
{
	char ***ptr = malloc(row * sizeof(*ptr) + row * (col * sizeof **ptr) );

	int * const data = ptr + row;
	for(int i = 0; i < row; i++)
		ptr[i] = data + i * col;

	return ptr;
}

void SDL_DestroyMutex(SDL_mutex* mutex)
{

}

SDL_mutex* SDL_CreateMutex(void)
{
    SDL_mutex* mut = NULL;
    return mut;
}

int SDL_LockMutex(SDL_mutex* mutex)
{
    return 0;
}

int SDL_UnlockMutex(SDL_mutex* mutex)
{
    return 0;
}