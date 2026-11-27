/* harness-only: host "executable memory" below 4 GB so the 32-bit exec address stays valid */
#include "xjit_exec.h"
#include <sys/mman.h>
#include <string.h>
uint32_t g_host_exec_base = 0, g_host_exec_size = 0;
static bool a(xj_exec_t *x, size_t size){
    memset(x,0,sizeof *x); size=(size+4095)&~4095u;
    void *p=mmap(0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    if(p==MAP_FAILED) return false;
    x->data=p; x->exec=(uint32_t)(uintptr_t)p; x->size=size; x->psram=false; g_host_exec_base=x->exec; g_host_exec_size=(uint32_t)size; return true; }
bool xj_exec_alloc_iram(xj_exec_t *x,size_t s){return a(x,s);}
bool xj_exec_alloc_psram(xj_exec_t *x,size_t s){return a(x,s);}
void xj_exec_free(xj_exec_t *x){ if(x->data) munmap(x->data,x->size); memset(x,0,sizeof *x);}
int xj_exec_sync(xj_exec_t *x,size_t o,size_t l){(void)x;(void)o;(void)l;return 0;}
