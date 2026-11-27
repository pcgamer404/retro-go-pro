/* Host harness for dynarec v2: real dynarec.c + stub VM services exposed through a flat C API. */
#include "dynarec.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

const char *S_clinit = "<clinit>";
static int g_flush_msgs = 0;
static char g_last_log[256];
void nj_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_last_log, sizeof g_last_log, fmt, ap); va_end(ap);
    if (strstr(g_last_log, "Flushing")) g_flush_msgs++;
}
Object *resolve_string(Thread *t, Class *c, uint16_t i) { (void)t;(void)c;(void)i; return NULL; }
Class  *resolve_class(Thread *t, Class *c, uint16_t i)  { (void)t;(void)c;(void)i; return NULL; }
Field  *resolve_field(Thread *t, Class *c, uint16_t i)  { (void)t;(void)c;(void)i; return NULL; }
Object *class_mirror(Class *c) { (void)c; return NULL; }
bool monitor_exit(Thread *t, Object *o) { (void)t; (void)o; return true; }
void *nj_pal_malloc(size_t n) { return calloc(1, n); }
void nj_pal_free(void *p) { free(p); }

#define MAXM 4096
static Class  g_cls;
static Method g_m[MAXM];
static int    g_n = 0;
static Thread g_t;
static CPEntry g_cp[64];
static Field  g_fields[8];
static Class  g_fcls;

void class_foreach(void (*fn)(Class *, void *), void *ctx) { fn(&g_cls, ctx); }

int h_init(void) {
    memset(&g_cls, 0, sizeof g_cls);
    g_cls.name = "Test"; g_cls.methods = g_m; g_cls.cp = g_cp; g_cls.cp_count = 64;
    memset(&g_fcls, 0, sizeof g_fcls);
    g_fcls.state = CLS_INITIALIZED;
    g_fcls.statics = (Slot *)(uintptr_t)0x20006000u;      /* lives in the emulator's address space */
    dynarec_init();
    return 0;
}
int h_add(const uint8_t *code, int len, int max_locals, int max_stack, int ret_slots) {
    Method *m = &g_m[g_n];
    memset(m, 0, sizeof *m);
    m->cls = &g_cls; m->name = "m"; m->desc = "()V";
    m->code = (uint8_t *)malloc(len + 8); memcpy(m->code, code, len); m->code_len = len;
    m->max_locals = max_locals; m->max_stack = max_stack; m->ret_slots = ret_slots;
    g_cls.n_methods = ++g_n;
    return g_n - 1;
}
int h_compile(int i) { return dynarec_compile_method(&g_m[i], &g_t) ? 1 : 0; }
int h_has_code(int i) { return g_m[i].jit_code != NULL; }
int h_unsupported(int i) { return (g_m[i].access & ACC_JIT_UNSUPPORTED) != 0; }
int h_osr(int i) { return (g_m[i].access & ACC_JIT_OSR) != 0; }
int h_fails(int i) { return g_m[i].jit_fails; }
uint32_t h_codeptr(int i) { return (uint32_t)(uintptr_t)g_m[i].code; }
extern uint32_t g_host_exec_base, g_host_exec_size;
uint32_t h_cache_base(void) { return g_host_exec_base; }
uint32_t h_cache_size(void) { return g_host_exec_size; }
uint32_t h_entry(int i) { return (uint32_t)(uintptr_t)g_m[i].jit_code; }
uint32_t h_jit_size(int i) { return g_m[i].jit_size; }
int h_off_frame_pc(void) { return offsetof(Frame, pc); }
int h_off_frame_sp(void) { return offsetof(Frame, sp); }
int h_off_frame_locals(void) { return offsetof(Frame, locals); }
int h_off_thread_budget(void) { return offsetof(Thread, budget); }
int h_off_class_super(void) { return offsetof(Class, super); }
int h_off_class_component(void) { return offsetof(Class, component); }
int h_flush_msgs_count(void) { return g_flush_msgs; }
const char *h_last_log(void) { return g_last_log; }
void h_set_cp_int(int idx, int v) { g_cp[idx].tag = CONSTANT_Integer; g_cp[idx].resolved = 1; g_cp[idx].v.i = v; }
void h_set_cp_long(int idx, uint32_t hi, uint32_t lo) {          /* cp[idx].v.u = hi, cp[idx+1].v.u = lo */
    g_cp[idx].tag = CONSTANT_Long; g_cp[idx].v.u = hi; g_cp[idx + 1].v.u = lo;
}
/* static field entry: Fieldref resolved to slot `offset` of a class whose statics live at 0x20006000 */
void h_set_cp_static(int idx, int fidx, int offset, int wide) {
    g_fields[fidx].cls = &g_fcls; g_fields[fidx].offset = offset; g_fields[fidx].desc = wide ? "J" : "I";
    g_cp[idx].tag = CONSTANT_Fieldref; g_cp[idx].resolved = 1; g_cp[idx].v.field = &g_fields[fidx];
}
void h_set_fstate(int s) { g_fcls.state = s; }
void h_set_exec_count(int i, int n) { g_m[i].exec_count = n; }
void h_reset(void) { dynarec_flush_cache(); g_n = 0; g_cls.n_methods = 0; }
int h_reset_methods_only(void) { g_n = 0; g_cls.n_methods = 0; return 0; }
