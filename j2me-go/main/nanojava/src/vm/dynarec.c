/*
 * dynarec.c - Dynamic Recompiler (JIT v3.1 Unified) for NanoJava on ESP32-S3 (Xtensa LX7).
 *
 * Register Map inside JIT Window (entry a1, 32):
 *  a2  = Thread *t (in) / status return code (out)
 *  a3  = Frame  *f (in)
 *  a4  = Slot   *locals (f->locals)
 *  a5  = Slot   *sp     (memory stack pointer, below cached TOS registers)
 *  a6  = scratch 1 (REG_S1)
 *  a7  = scratch 2 (REG_S2)
 *  a8  = scratch 3 (REG_S3)
 *  a9  = scratch 4 (REG_S4)
 *  a10 = m->code base pointer / callx8 arg0
 *  a11 = REG_TOS0 (top operand stack slot when tos_depth >= 1) / callx8 arg1
 *  a12 = REG_TOS1 (second operand stack slot when tos_depth == 2) / callx8 arg2
 */
#include "dynarec.h"
#include "xjit_exec.h"
#include "xjit_emit.h"
#include "../pal/pal.h"
#include "../pal/alloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_cache.h"
#endif

#define MAX_LITS          2048
#define MAX_RESUME_POINTS 1024
#define MAX_FIXUPS        2048
#define MAX_CODE_SIZE     16384

typedef struct {
    uint32_t pc;
    int      patch_pos;
} ResumePoint;

typedef struct {
    int      patch_pos;
    uint32_t target_pc;
} BranchFixup;

static xj_exec_t    s_jit_mem;
static size_t       s_jit_cache_used = 0;
static bool         s_jit_cache_full = false;
static uint32_t     s_methods_compiled = 0;
static uint32_t     s_jit_invocations = 0;
static uint32_t     s_jit_deopts = 0;
static uint32_t     s_jit_bytes_emitted = 0;

static uint32_t     s_jit_compile_attempts = 0;
static uint32_t     s_jit_compile_rejects = 0;
static const char  *s_reject_reason = NULL;
static uint32_t     s_reject_pc = 0;
static uint8_t      s_reject_op = 0;

static uint32_t    *s_lits = NULL;
static int         *s_lit_offsets = NULL;
static int          s_n_lits = 0;

static ResumePoint *s_resumes = NULL;
static int          s_n_resumes = 0;

static int32_t     *s_pc_to_jit = NULL;
static BranchFixup *s_fixups = NULL;
static uint8_t     *s_is_target = NULL;

/* ------------------------------------------------------------------------ */
/* C Runtime Trampolines called directly from JIT via callx8                */
/* ------------------------------------------------------------------------ */
static int32_t jit_rt_fdiv(int32_t a_bits, int32_t b_bits)
{
    float a, b, r;
    memcpy(&a, &a_bits, 4);
    memcpy(&b, &b_bits, 4);
    r = a / b;
    int32_t out;
    memcpy(&out, &r, 4);
    return out;
}

static int32_t jit_rt_frem(int32_t a_bits, int32_t b_bits)
{
    float a, b, r;
    memcpy(&a, &a_bits, 4);
    memcpy(&b, &b_bits, 4);
    r = fmodf(a, b);
    int32_t out;
    memcpy(&out, &r, 4);
    return out;
}

static Object *jit_rt_new(Thread *t, Class *c)
{
    if (!c || c->state != CLS_INITIALIZED) return NULL;
    return vm_new_object(t, c);
}

static Array *jit_rt_newarray(Thread *t, int32_t atype, int32_t count)
{
    static const char types[12] = {0, 0, 0, 0, 'Z', 'C', 'F', 'D', 'B', 'S', 'I', 'J'};
    if (atype < 4 || atype > 11 || count < 0) return NULL;
    return vm_new_prim_array(t, types[atype], count);
}

static Array *jit_rt_anewarray(Thread *t, Class *elem_cls, int32_t count)
{
    if (!elem_cls || count < 0) return NULL;
    return vm_new_array(t, class_array_of(elem_cls), count);
}

static int32_t jit_rt_instanceof(Object *o, Class *c)
{
    return (o && c && class_assignable(o->cls, c)) ? 1 : 0;
}

static int32_t jit_rt_checkcast(Object *o, Class *c)
{
    return (!o || (c && class_assignable(o->cls, c))) ? 1 : 0;
}

void dynarec_init(void)
{
    if (s_jit_mem.data)
        return;

    if (JIT_CODE_CACHE_SIZE <= 64 * 1024) {
        xj_exec_alloc_iram(&s_jit_mem, JIT_CODE_CACHE_SIZE);
    }

    if (!s_jit_mem.data) {
        static const size_t cache_sizes[] = {
            JIT_CODE_CACHE_SIZE,
            512 * 1024,
            384 * 1024,
            256 * 1024,
            128 * 1024
        };
        for (size_t i = 0; i < sizeof(cache_sizes) / sizeof(cache_sizes[0]); i++) {
            if (cache_sizes[i] <= JIT_CODE_CACHE_SIZE && xj_exec_alloc_psram(&s_jit_mem, cache_sizes[i])) {
                break;
            }
        }
    }

    if (!s_jit_mem.data) {
        nj_log("[dynarec] ERROR: Failed to allocate JIT code cache!");
        return;
    }

    if (!s_lits)        s_lits        = (uint32_t *)nj_pal_malloc(MAX_LITS * sizeof(uint32_t));
    if (!s_lit_offsets) s_lit_offsets = (int *)nj_pal_malloc(MAX_LITS * sizeof(int));
    if (!s_resumes)     s_resumes     = (ResumePoint *)nj_pal_malloc(MAX_RESUME_POINTS * sizeof(ResumePoint));
    if (!s_pc_to_jit)   s_pc_to_jit   = (int32_t *)nj_pal_malloc((MAX_CODE_SIZE + 1) * sizeof(int32_t));
    if (!s_fixups)      s_fixups      = (BranchFixup *)nj_pal_malloc(MAX_FIXUPS * sizeof(BranchFixup));
    if (!s_is_target)   s_is_target   = (uint8_t *)nj_pal_malloc(MAX_CODE_SIZE + 1);

    s_jit_cache_used = 0;
    s_jit_cache_full = false;
    nj_log("[dynarec] JIT v3.1 (Unified TOS-Cached + 3D) allocated: %u KB (%s) data=%p, exec=0x%08x",
           (unsigned)(s_jit_mem.size / 1024),
           s_jit_mem.psram ? "PSRAM" : "IRAM",
           s_jit_mem.data,
           (unsigned)s_jit_mem.exec);
}

/* ------------------------------------------------------------------------ */
/* Xtensa LX7 Register Map & TOS (Top-of-Stack) Register Cache              */
/* ------------------------------------------------------------------------ */
#define REG_T       2
#define REG_F       3
#define REG_LV      4
#define REG_SP      5
#define REG_S1      6
#define REG_S2      7
#define REG_S3      8
#define REG_S4      9
#define REG_CODE    10
#define REG_TOS0    11
#define REG_TOS1    12

static inline void xj_32(xj_emit_t *e, uint32_t w)
{
    xj_16(e, w & 0xFFFF);
    xj_16(e, (w >> 16) & 0xFFFF);
}

/* Single-precision FPU emitters (Xtensa LX7 Coprocessor 0) */
static inline void xj_wfr(xj_emit_t *e, int fr, int as)
{
    xj_24(e, (0xFAu << 16) | ((fr & 15) << 12) | ((as & 15) << 8) | 0x50);
}
static inline void xj_rfr(xj_emit_t *e, int ar, int fs)
{
    xj_24(e, (0xFAu << 16) | ((ar & 15) << 12) | ((fs & 15) << 8) | 0x40);
}
static inline void xj_olt_s(xj_emit_t *e, int br, int fs, int ft)
{
    xj_24(e, (4u << 20) | (0xBu << 16) | ((br & 15) << 12) | ((fs & 15) << 8) | ((ft & 15) << 4));
}
static inline void xj_oeq_s(xj_emit_t *e, int br, int fs, int ft)
{
    xj_24(e, (2u << 20) | (0xBu << 16) | ((br & 15) << 12) | ((fs & 15) << 8) | ((ft & 15) << 4));
}
static inline void xj_movt(xj_emit_t *e, int ar, int as, int br)
{
    xj_24(e, (0xD3u << 16) | ((ar & 15) << 12) | ((as & 15) << 8) | ((br & 15) << 4));
}
static inline void xj_add_s(xj_emit_t *e, int fr, int fs, int ft)
{
    xj_24(e, (0x0Au << 16) | ((fr & 15) << 12) | ((fs & 15) << 8) | ((ft & 15) << 4));
}
static inline void xj_sub_s(xj_emit_t *e, int fr, int fs, int ft)
{
    xj_24(e, (0x1Au << 16) | ((fr & 15) << 12) | ((fs & 15) << 8) | ((ft & 15) << 4));
}
static inline void xj_mul_s(xj_emit_t *e, int fr, int fs, int ft)
{
    xj_24(e, (0x2Au << 16) | ((fr & 15) << 12) | ((fs & 15) << 8) | ((ft & 15) << 4));
}
static inline void xj_neg_s(xj_emit_t *e, int fr, int fs)
{
    xj_24(e, (0xFAu << 16) | ((fr & 15) << 12) | ((fs & 15) << 8) | 0x60);
}
static inline void xj_float_s(xj_emit_t *e, int fr, int as)
{
    xj_24(e, (0xCAu << 16) | ((fr & 15) << 12) | ((as & 15) << 8));
}
static inline void xj_trunc_s(xj_emit_t *e, int ar, int fs)
{
    xj_24(e, (0x9Au << 16) | ((ar & 15) << 12) | ((fs & 15) << 8));
}

/* TOS Stack Cache Operations:
 *   tos_depth == 0: all operand words are in memory below REG_SP.
 *   tos_depth == 1: top word is in REG_TOS0 (a11); REG_SP points to its memory slot.
 *   tos_depth == 2: top word is in REG_TOS0 (a11), second word in REG_TOS1 (a12);
 *                   REG_SP points to REG_TOS1's memory slot.
 */
static inline void jit_push(xj_emit_t *e, int src_reg)
{
    xj_s32i(e, src_reg, REG_SP, 0);
    xj_addi(e, REG_SP, REG_SP, 4);
}

static inline void tos_spill(xj_emit_t *e, int *tos)
{
    if (*tos == 1) {
        xj_s32i(e, REG_TOS0, REG_SP, 0);
        xj_addi(e, REG_SP, REG_SP, 4);
        *tos = 0;
    } else if (*tos == 2) {
        xj_s32i(e, REG_TOS1, REG_SP, 0);
        xj_s32i(e, REG_TOS0, REG_SP, 4);
        xj_addi(e, REG_SP, REG_SP, 8);
        *tos = 0;
    }
}

static inline void tos_push(xj_emit_t *e, int *tos, int src_reg)
{
    if (*tos == 0) {
        if (src_reg != REG_TOS0) xj_mov_n(e, REG_TOS0, src_reg);
        *tos = 1;
    } else if (*tos == 1) {
        xj_mov_n(e, REG_TOS1, REG_TOS0);
        if (src_reg != REG_TOS0) xj_mov_n(e, REG_TOS0, src_reg);
        *tos = 2;
    } else {
        /* Spill oldest cached item (REG_TOS1) to memory, shift TOS0 -> TOS1, load new TOS0 */
        xj_s32i(e, REG_TOS1, REG_SP, 0);
        xj_addi(e, REG_SP, REG_SP, 4);
        xj_mov_n(e, REG_TOS1, REG_TOS0);
        if (src_reg != REG_TOS0) xj_mov_n(e, REG_TOS0, src_reg);
        *tos = 2;
    }
}

static inline void tos_pop(xj_emit_t *e, int *tos, int dst_reg)
{
    if (*tos == 2) {
        if (dst_reg != REG_TOS0) xj_mov_n(e, dst_reg, REG_TOS0);
        xj_mov_n(e, REG_TOS0, REG_TOS1);
        *tos = 1;
    } else if (*tos == 1) {
        if (dst_reg != REG_TOS0) xj_mov_n(e, dst_reg, REG_TOS0);
        *tos = 0;
    } else {
        xj_addi(e, REG_SP, REG_SP, -4);
        xj_l32i(e, dst_reg, REG_SP, 0);
    }
}

static inline void tos_pop2(xj_emit_t *e, int *tos, int reg_first, int reg_second)
{
    /* reg_first = deeper (sp[-2]), reg_second = top (sp[-1]) */
    if (*tos == 2) {
        if (reg_second == REG_TOS1 && reg_first == REG_TOS0) {
            xj_mov_n(e, REG_S4, REG_TOS0);
            xj_mov_n(e, reg_first, REG_TOS1);
            xj_mov_n(e, reg_second, REG_S4);
        } else if (reg_first == REG_TOS0) {
            xj_mov_n(e, reg_second, REG_TOS0);
            xj_mov_n(e, reg_first, REG_TOS1);
        } else {
            if (reg_first != REG_TOS1) xj_mov_n(e, reg_first, REG_TOS1);
            if (reg_second != REG_TOS0) xj_mov_n(e, reg_second, REG_TOS0);
        }
        *tos = 0;
    } else if (*tos == 1) {
        if (reg_second != REG_TOS0) xj_mov_n(e, reg_second, REG_TOS0);
        xj_addi(e, REG_SP, REG_SP, -4);
        xj_l32i(e, reg_first, REG_SP, 0);
        *tos = 0;
    } else {
        xj_addi(e, REG_SP, REG_SP, -8);
        xj_l32i(e, reg_first, REG_SP, 0);
        xj_l32i(e, reg_second, REG_SP, 4);
    }
}

static inline void tos_pop3(xj_emit_t *e, int *tos, int reg_first, int reg_second, int reg_third)
{
    tos_pop(e, tos, reg_third);
    tos_pop2(e, tos, reg_first, reg_second);
}

static bool add_lit(uint32_t val)
{
    if (!s_lits) return false;
    for (int i = 0; i < s_n_lits; i++) {
        if (s_lits[i] == val) return true;
    }
    if (s_n_lits >= MAX_LITS) return false;
    s_lits[s_n_lits++] = val;
    return true;
}

static inline void jit_emit_imm(xj_emit_t *e, int reg, int32_t val)
{
    if (val >= -2048 && val <= 2047) {
        xj_movi(e, reg, val);
        return;
    }
    if (s_lits && s_lit_offsets) {
        for (int i = 0; i < s_n_lits; i++) {
            if ((int32_t)s_lits[i] == val) {
                int disp = s_lit_offsets[i] - ((e->pos + 3) & ~3);
                xj_l32r(e, reg, disp);
                return;
            }
        }
    }
    int32_t low = (val & 0xFF);
    if (low & 0x80) low -= 256;
    int32_t high = val - low;
    xj_movi(e, reg, low);
    xj_addmi(e, reg, reg, high);
}

static inline bool add_resume_point(uint32_t pc)
{
    if (!s_resumes) return false;
    if (s_is_target && pc <= MAX_CODE_SIZE) s_is_target[pc] = 1;
    for (int i = 0; i < s_n_resumes; i++) {
        if (s_resumes[i].pc == pc) return true;
    }
    if (s_n_resumes >= MAX_RESUME_POINTS) return false;
    s_resumes[s_n_resumes].pc = pc;
    s_resumes[s_n_resumes].patch_pos = -1;
    s_n_resumes++;
    return true;
}

static inline void jit_emit_yield(xj_emit_t *e, int *tos, uint32_t pc)
{
    tos_spill(e, tos);
    xj_s32i(e, REG_SP, REG_F, offsetof(Frame, sp));
    jit_emit_imm(e, REG_S1, (int32_t)pc);
    xj_add(e, REG_S1, REG_CODE, REG_S1);
    xj_s32i(e, REG_S1, REG_F, offsetof(Frame, pc));
    xj_movi(e, REG_T, JIT_YIELD_CALL);
    xj_retw_n(e);
}

/* ------------------------------------------------------------------------ */
/* Bytecode Validation & Literal Pre-Collection                             */
/* ------------------------------------------------------------------------ */
static bool is_method_supported(Method *m, Thread *t)
{
    if (m->access & ACC_JIT_UNSUPPORTED) return false;
    if (m->native || m->name == S_clinit || m->code_len > MAX_CODE_SIZE) {
        m->access |= ACC_JIT_UNSUPPORTED;
        return false;
    }

    uint8_t *code = m->code;
    uint32_t len = m->code_len;
    s_n_lits = 0;
    s_n_resumes = 0;
    s_reject_reason = NULL;
    s_reject_pc = 0;
    s_reject_op = 0;
    if (s_is_target) memset(s_is_target, 0, len + 1);

#define MAX_DETECTED_LOOPS 256
    typedef struct { uint32_t start_pc, end_pc; } LoopSpan;
    LoopSpan loops[MAX_DETECTED_LOOPS];
    int n_loops = 0;

    for (uint32_t pc = 0; pc < len; ) {
        uint8_t op = code[pc];
        switch (op) {
        case 0x00: case 0x01: case 0x02:
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
        case 0x09: case 0x0a: case 0x0b: case 0x0e: case 0x10:
            break;

        case 0x0c: if (!add_lit(0x3f800000)) return false; break;
        case 0x0d: if (!add_lit(0x40000000)) return false; break;
        case 0x0f: if (!add_lit(0x3ff00000)) return false; break;

        case 0x11: {
            int16_t val = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            if (val < -2048 || val > 2047)
                if (!add_lit((uint32_t)val)) return false;
            break;
        }

        case 0x12: case 0x13: {
            uint16_t idx = (op == 0x12) ? code[pc + 1] : (((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            CPEntry *cpe = &m->cls->cp[idx];
            if (cpe->tag == CONSTANT_Integer || cpe->tag == CONSTANT_Float) {
                if (cpe->v.i < -2048 || cpe->v.i > 2047)
                    if (!add_lit((uint32_t)cpe->v.i)) return false;
            } else if (cpe->tag == CONSTANT_String) {
                if (!cpe->resolved && t) resolve_string(t, m->cls, idx);
                if (!cpe->resolved) return false;
                if (!add_lit((uint32_t)(uintptr_t)cpe->v.str)) return false;
            } else if (cpe->tag == CONSTANT_Class) {
                if (!cpe->resolved && t) resolve_class(t, m->cls, idx);
                if (!cpe->resolved) return false;
                Object *mir = class_mirror(cpe->v.cls);
                if (!mir) return false;
                if (!add_lit((uint32_t)(uintptr_t)mir)) return false;
            } else {
                return false;
            }
            break;
        }

        case 0x14: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            CPEntry *cpe = &m->cls->cp[idx];
            if (cpe->tag != CONSTANT_Long && cpe->tag != CONSTANT_Double) return false;
            if (!add_lit((uint32_t)m->cls->cp[idx + 1].v.u)) return false;
            if (!add_lit((uint32_t)m->cls->cp[idx].v.u)) return false;
            break;
        }

        case 0x15: case 0x17: case 0x19:
        case 0x16: case 0x18: case 0x37: case 0x39:
            if (code[pc + 1] > 254) return false;
            break;
        case 0x1a: case 0x1b: case 0x1c: case 0x1d:
        case 0x1e: case 0x1f: case 0x20: case 0x21:
        case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x26: case 0x27: case 0x28: case 0x29:
        case 0x2a: case 0x2b: case 0x2c: case 0x2d:
        case 0x2e: case 0x2f: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
        case 0x36: case 0x38: case 0x3a:
        case 0x3b: case 0x3c: case 0x3d: case 0x3e:
        case 0x3f: case 0x40: case 0x41: case 0x42:
        case 0x43: case 0x44: case 0x45: case 0x46:
        case 0x47: case 0x48: case 0x49: case 0x4a:
        case 0x4b: case 0x4c: case 0x4d: case 0x4e:
        case 0x4f: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56:
        case 0x57: case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        case 0x60: case 0x61: case 0x64: case 0x65: case 0x68: case 0x69: case 0x6c: case 0x70:
        case 0x74: case 0x75: case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d:
        case 0x7e: case 0x7f: case 0x80: case 0x81: case 0x82: case 0x83: case 0x84:
        case 0x62: case 0x66: case 0x6a: case 0x76: case 0x86: case 0x8b:
        case 0x85: case 0x88: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96:
        case 0xac: case 0xad: case 0xae: case 0xaf: case 0xb0: case 0xb1:
        case 0xbe: case 0xca: case 0xcb: case 0xcc: case 0xcd:
            break;

        /* Direct C-trampoline helpers */
        case 0x6e: /* fdiv */
            if (!add_lit((uint32_t)(uintptr_t)jit_rt_fdiv)) return false;
            break;
        case 0x72: /* frem */
            if (!add_lit((uint32_t)(uintptr_t)jit_rt_frem)) return false;
            break;
        case 0xbb: { /* new */
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            CPEntry *cpe = &m->cls->cp[idx];
            if (!cpe->resolved && t) resolve_class(t, m->cls, idx);
            if (!cpe->resolved || !cpe->v.cls) return false;
            if (!add_lit((uint32_t)(uintptr_t)jit_rt_new)) return false;
            if (!add_lit((uint32_t)(uintptr_t)cpe->v.cls)) return false;
            break;
        }
        case 0xbc: { /* newarray */
            if (!add_lit((uint32_t)(uintptr_t)jit_rt_newarray)) return false;
            break;
        }
        case 0xbd: { /* anewarray */
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            CPEntry *cpe = &m->cls->cp[idx];
            if (!cpe->resolved && t) resolve_class(t, m->cls, idx);
            if (!cpe->resolved || !cpe->v.cls) return false;
            if (!add_lit((uint32_t)(uintptr_t)jit_rt_anewarray)) return false;
            if (!add_lit((uint32_t)(uintptr_t)cpe->v.cls)) return false;
            break;
        }

        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
        case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
        case 0xa5: case 0xa6: case 0xa7: case 0xc6: case 0xc7:
        {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            uint32_t target = pc + offset;
            if (target <= len && s_is_target) s_is_target[target] = 1;
            if (offset <= 0 && n_loops < MAX_DETECTED_LOOPS) {
                loops[n_loops].start_pc = target;
                loops[n_loops].end_pc = pc;
                n_loops++;
            }
            break;
        }
        case 0xc8: {
            int32_t offset = (int32_t)(((uint32_t)code[pc + 1] << 24) | ((uint32_t)code[pc + 2] << 16) |
                                      ((uint32_t)code[pc + 3] << 8) | code[pc + 4]);
            uint32_t target = pc + offset;
            if (target <= len && s_is_target) s_is_target[target] = 1;
            if (offset <= 0 && n_loops < MAX_DETECTED_LOOPS) {
                loops[n_loops].start_pc = target;
                loops[n_loops].end_pc = pc;
                n_loops++;
            }
            break;
        }

        case 0xaa: {
            uint32_t pad = (4 - ((pc + 1) % 4)) % 4;
            uint32_t p = pc + 1 + pad;
            if (p + 12 > len) return false;
            int32_t dflt = (int32_t)(((uint32_t)code[p] << 24) | ((uint32_t)code[p+1] << 16) | ((uint32_t)code[p+2] << 8) | code[p+3]);
            int32_t low  = (int32_t)(((uint32_t)code[p+4] << 24) | ((uint32_t)code[p+5] << 16) | ((uint32_t)code[p+6] << 8) | code[p+7]);
            int32_t high = (int32_t)(((uint32_t)code[p+8] << 24) | ((uint32_t)code[p+9] << 16) | ((uint32_t)code[p+10] << 8) | code[p+11]);
            if (low > high || (uint32_t)(high - low + 1) > 1024) return false;
            uint32_t count = (uint32_t)(high - low + 1);
            if (p + 12 + count * 4 > len) return false;
            if ((uint32_t)(pc + dflt) <= len && s_is_target) s_is_target[pc + dflt] = 1;
            for (uint32_t i = 0; i < count; i++) {
                int32_t val = low + (int32_t)i;
                if (val < -2048 || val > 2047) add_lit((uint32_t)val);
                int32_t target_offset = (int32_t)(((uint32_t)code[p + 12 + i * 4] << 24) |
                                                  ((uint32_t)code[p + 12 + i * 4 + 1] << 16) |
                                                  ((uint32_t)code[p + 12 + i * 4 + 2] << 8) |
                                                  code[p + 12 + i * 4 + 3]);
                if ((uint32_t)(pc + target_offset) <= len && s_is_target) s_is_target[pc + target_offset] = 1;
            }
            pc = p + 12 + count * 4;
            continue;
        }

        case 0xab: {
            uint32_t pad = (4 - ((pc + 1) % 4)) % 4;
            uint32_t p = pc + 1 + pad;
            if (p + 8 > len) return false;
            int32_t dflt = (int32_t)(((uint32_t)code[p] << 24) | ((uint32_t)code[p+1] << 16) | ((uint32_t)code[p+2] << 8) | code[p+3]);
            int32_t npairs = (int32_t)(((uint32_t)code[p+4] << 24) | ((uint32_t)code[p+5] << 16) | ((uint32_t)code[p+6] << 8) | code[p+7]);
            if (npairs < 0 || (uint32_t)npairs > 512) return false;
            if (p + 8 + (uint32_t)npairs * 8 > len) return false;
            if ((uint32_t)(pc + dflt) <= len && s_is_target) s_is_target[pc + dflt] = 1;
            for (uint32_t i = 0; i < (uint32_t)npairs; i++) {
                int32_t match = (int32_t)(((uint32_t)code[p + 8 + i * 8] << 24) |
                                          ((uint32_t)code[p + 8 + i * 8 + 1] << 16) |
                                          ((uint32_t)code[p + 8 + i * 8 + 2] << 8) |
                                          code[p + 8 + i * 8 + 3]);
                if (match < -2048 || match > 2047)
                    if (!add_lit((uint32_t)match)) return false;
                int32_t target_offset = (int32_t)(((uint32_t)code[p + 8 + i * 8 + 4] << 24) |
                                                  ((uint32_t)code[p + 8 + i * 8 + 5] << 16) |
                                                  ((uint32_t)code[p + 8 + i * 8 + 6] << 8) |
                                                  code[p + 8 + i * 8 + 7]);
                if ((uint32_t)(pc + target_offset) <= len && s_is_target) s_is_target[pc + target_offset] = 1;
            }
            pc = p + 8 + (uint32_t)npairs * 8;
            continue;
        }

        case 0xc4: {
            if (pc + 1 >= len) return false;
            uint8_t sub_op = code[pc + 1];
            if (sub_op == 0x84) {
                if (pc + 5 >= len) return false;
                uint16_t idx = ((uint16_t)code[pc + 2] << 8) | code[pc + 3];
                if (idx > 255) return false;
                int16_t cval = (int16_t)(((uint16_t)code[pc + 4] << 8) | code[pc + 5]);
                if (cval < -2048 || cval > 2047)
                    if (!add_lit((uint32_t)cval)) return false;
                pc += 6;
                continue;
            } else if (sub_op == 0x15 || sub_op == 0x17 || sub_op == 0x19 ||
                       sub_op == 0x36 || sub_op == 0x38 || sub_op == 0x3a) {
                if (pc + 3 >= len) return false;
                if ((((uint16_t)code[pc + 2] << 8) | code[pc + 3]) > 255) return false;
                pc += 4;
                continue;
            } else if (sub_op == 0x16 || sub_op == 0x18 ||
                       sub_op == 0x37 || sub_op == 0x39) {
                if (pc + 3 >= len) return false;
                if ((((uint16_t)code[pc + 2] << 8) | code[pc + 3]) > 254) return false;
                pc += 4;
                continue;
            }
            return false;
        }

        case 0xb2: case 0xb3: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            CPEntry *cpe = &m->cls->cp[idx];
            if (!cpe->resolved && t) resolve_field(t, m->cls, idx);
            if (!cpe->resolved || !cpe->v.field) return false;
            Field *fl = cpe->v.field;
            if (fl->cls->state != CLS_INITIALIZED) return false;
            Slot *s = &fl->cls->statics[fl->offset];
            if (!add_lit((uint32_t)(uintptr_t)s)) return false;
            break;
        }

        case 0xb4: case 0xb5: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            CPEntry *cpe = &m->cls->cp[idx];
            if (!cpe->resolved && t) resolve_field(t, m->cls, idx);
            if (cpe->resolved && cpe->v.field && cpe->v.field->offset < 252) {
                Field *fl = cpe->v.field;
                bool wide = (fl->desc[0] == 'J' || fl->desc[0] == 'D');
                code[pc + 1] = (uint8_t)fl->offset;
                code[pc + 2] = 0;
                __asm__ __volatile__("" ::: "memory");
                code[pc] = (op == 0xb4) ? (wide ? 0xcb : 0xca) : (wide ? 0xcd : 0xcc);
            } else if (cpe->resolved && cpe->v.field) {
                add_resume_point(pc + 3);
            } else {
                return false;
            }
            break;
        }

        case 0xb6: case 0xb7: case 0xb8: case 0xce:
            add_resume_point(pc + 3);
            break;

        case 0xb9:
            add_resume_point(pc + 5);
            break;

        case 0xc0: case 0xc1: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            CPEntry *cpe = &m->cls->cp[idx];
            if (!cpe->resolved && t) resolve_class(t, m->cls, idx);
            if (!cpe->resolved || !cpe->v.cls) return false;
            if (!add_lit((uint32_t)(uintptr_t)cpe->v.cls)) return false;
            if (cpe->v.cls->flags & (CF_INTERFACE | CF_ARRAY)) {
                if (!add_lit((uint32_t)(uintptr_t)(op == 0xc0 ? jit_rt_checkcast : jit_rt_instanceof)))
                    return false;
            }
            break;
        }

        case 0xc5:
            add_resume_point(pc + 4);
            break;

        case 0x6d: case 0x71:
        case 0x63: case 0x67: case 0x6b: case 0x6f: case 0x73: case 0x77:
        case 0x87: case 0x89: case 0x8a: case 0x8c: case 0x8d: case 0x8e: case 0x8f: case 0x90: case 0x97: case 0x98:
            add_resume_point(pc + 1);
            break;

        case 0xbf:
            break;

        case 0xc2: case 0xc3:
            add_resume_point(pc + 1);
            break;

        case 0xa8: case 0xa9: case 0xc9: {
            uint32_t step = (op == 0xa9 ? 2 : (op == 0xc9 ? 5 : 3));
            add_resume_point(pc + step);
            break;
        }

        default:
            s_reject_reason = "unsupported opcode";
            s_reject_pc = pc;
            s_reject_op = op;
            m->access |= ACC_JIT_UNSUPPORTED;
            return false;
        }

        switch (op) {
        case 0x10: case 0x12:
        case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
        case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:
        case 0xa9: case 0xbc:
            pc += 2; break;
        case 0x11: case 0x13: case 0x14:
        case 0x84:
        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
        case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
        case 0xa5: case 0xa6: case 0xa7: case 0xa8: case 0xc6: case 0xc7:
        case 0xb2: case 0xb3: case 0xb4: case 0xb5:
        case 0xca: case 0xcb: case 0xcc: case 0xcd:
        case 0xb6: case 0xb7: case 0xb8: case 0xce:
        case 0xbb: case 0xbd: case 0xc0: case 0xc1:
            pc += 3; break;
        case 0xc5:
            pc += 4; break;
        case 0xc8: case 0xc9: case 0xb9:
            pc += 5; break;
        default:
            pc += 1; break;
        }
    }

    for (int l = 0; l < n_loops; l++) {
        if (loops[l].start_pc > 0) {
            add_resume_point(loops[l].start_pc);
        }
    }

    if (s_n_resumes > MAX_RESUME_POINTS || len > MAX_CODE_SIZE) {
        s_reject_reason = "code/resumes too large";
        m->access |= ACC_JIT_UNSUPPORTED;
        return false;
    }

    return true;
}

static inline void patch_jump(uint8_t *buf, int j_pos, int target_pos)
{
    int disp = target_pos - (j_pos + 4);
    uint32_t w = (((uint32_t)disp & 0x3FFFF) << 6) | 6;
    buf[j_pos + 0] = (uint8_t)(w & 0xFF);
    buf[j_pos + 1] = (uint8_t)((w >> 8) & 0xFF);
    buf[j_pos + 2] = (uint8_t)((w >> 16) & 0xFF);
}

static inline void patch_bz(uint8_t *buf, int pos, int target_pos)
{
    int disp = target_pos - (pos + 4);
    uint32_t w = buf[pos] | ((uint32_t)buf[pos + 1] << 8) | ((uint32_t)buf[pos + 2] << 16);
    w = (w & 0x000FFF) | (((uint32_t)disp & 0xFFF) << 12);
    buf[pos + 0] = (uint8_t)(w & 0xFF);
    buf[pos + 1] = (uint8_t)((w >> 8) & 0xFF);
    buf[pos + 2] = (uint8_t)((w >> 16) & 0xFF);
}

static inline void patch_b8(uint8_t *buf, int pos, int target_pos)
{
    int disp = target_pos - (pos + 4);
    buf[pos + 2] = (uint8_t)(disp & 0xFF);
}

static inline void jit_emit_deopt(xj_emit_t *e, uint32_t pc, int restore_bytes)
{
    if (restore_bytes > 0)
        xj_addi(e, REG_SP, REG_SP, restore_bytes);
    xj_s32i(e, REG_SP, REG_F, offsetof(Frame, sp));
    jit_emit_imm(e, REG_S1, (int32_t)pc);
    xj_add(e, REG_S1, REG_CODE, REG_S1);
    xj_s32i(e, REG_S1, REG_F, offsetof(Frame, pc));
    xj_movi(e, REG_T, JIT_DEOPT);
    xj_retw_n(e);
}

static inline void jit_emit_null_check(xj_emit_t *e, int obj_reg, uint32_t pc, int restore_bytes)
{
    int b_not_null = e->pos;
    xj_bnez(e, obj_reg, 0);
    jit_emit_deopt(e, pc, restore_bytes);
    patch_bz(e->buf, b_not_null, e->pos);
}

static inline void jit_emit_null_check1(xj_emit_t *e, int obj_reg, uint32_t pc)
{
    int b_not_null = e->pos;
    xj_bnez(e, obj_reg, 0);
    xj_s32i(e, obj_reg, REG_SP, 0);
    xj_addi(e, REG_SP, REG_SP, 4);
    jit_emit_deopt(e, pc, 0);
    patch_bz(e->buf, b_not_null, e->pos);
}

static inline void jit_emit_null_check2(xj_emit_t *e, int obj_reg, int val_reg, uint32_t pc)
{
    int b_not_null = e->pos;
    xj_bnez(e, obj_reg, 0);
    xj_s32i(e, obj_reg, REG_SP, 0);
    xj_s32i(e, val_reg, REG_SP, 4);
    xj_addi(e, REG_SP, REG_SP, 8);
    jit_emit_deopt(e, pc, 0);
    patch_bz(e->buf, b_not_null, e->pos);
}

static inline void jit_emit_null_check3(xj_emit_t *e, int obj_reg, int val1_reg, int val2_reg, uint32_t pc)
{
    int b_not_null = e->pos;
    xj_bnez(e, obj_reg, 0);
    xj_s32i(e, obj_reg, REG_SP, 0);
    xj_s32i(e, val1_reg, REG_SP, 4);
    xj_s32i(e, val2_reg, REG_SP, 8);
    xj_addi(e, REG_SP, REG_SP, 12);
    jit_emit_deopt(e, pc, 0);
    patch_bz(e->buf, b_not_null, e->pos);
}

static inline void jit_emit_zero_check(xj_emit_t *e, int div_reg, uint32_t pc, int restore_bytes)
{
    int b_not_zero = e->pos;
    xj_bnez(e, div_reg, 0);
    jit_emit_deopt(e, pc, restore_bytes);
    patch_bz(e->buf, b_not_zero, e->pos);
}

static inline void jit_emit_zero_check2(xj_emit_t *e, int val1_reg, int val2_reg, uint32_t pc)
{
    int b_not_zero = e->pos;
    xj_bnez(e, val2_reg, 0);
    xj_s32i(e, val1_reg, REG_SP, 0);
    xj_s32i(e, val2_reg, REG_SP, 4);
    xj_addi(e, REG_SP, REG_SP, 8);
    jit_emit_deopt(e, pc, 0);
    patch_bz(e->buf, b_not_zero, e->pos);
}

/* Array load check: restores arr and idx to REG_SP on cold deopt */
static inline void jit_emit_array_check(xj_emit_t *e, int arr_reg, int idx_reg, int len_reg, uint32_t pc)
{
    int b_null = e->pos;
    xj_beqz(e, arr_reg, 0);
    xj_l32i(e, len_reg, arr_reg, 12);
    int b_bound = e->pos;
    xj_bgeu(e, idx_reg, len_reg, 0);
    int j_ok = e->pos;
    xj_j(e, 0);

    int deopt_pos = e->pos;
    patch_bz(e->buf, b_null, deopt_pos);
    patch_b8(e->buf, b_bound, deopt_pos);
    xj_s32i(e, arr_reg, REG_SP, 0);
    xj_s32i(e, idx_reg, REG_SP, 4);
    xj_addi(e, REG_SP, REG_SP, 8);
    jit_emit_deopt(e, pc, 0);
    patch_jump(e->buf, j_ok, e->pos);
}

/* Array store check: restores arr, idx, and val to REG_SP on cold deopt */
static inline void jit_emit_array_check_store(xj_emit_t *e, int arr_reg, int idx_reg, int val_reg, int len_reg, uint32_t pc)
{
    int b_null = e->pos;
    xj_beqz(e, arr_reg, 0);
    xj_l32i(e, len_reg, arr_reg, 12);
    int b_bound = e->pos;
    xj_bgeu(e, idx_reg, len_reg, 0);
    int j_ok = e->pos;
    xj_j(e, 0);

    int deopt_pos = e->pos;
    patch_bz(e->buf, b_null, deopt_pos);
    patch_b8(e->buf, b_bound, deopt_pos);
    xj_s32i(e, arr_reg, REG_SP, 0);
    xj_s32i(e, idx_reg, REG_SP, 4);
    xj_s32i(e, val_reg, REG_SP, 8);
    xj_addi(e, REG_SP, REG_SP, 12);
    jit_emit_deopt(e, pc, 0);
    patch_jump(e->buf, j_ok, e->pos);
}

/* Array check for already-spilled stack slots (e.g. lastore, dastore) */
static inline void jit_emit_array_check_spilled(xj_emit_t *e, int arr_reg, int idx_reg, int len_reg, uint32_t pc, int restore_bytes)
{
    int b_null = e->pos;
    xj_beqz(e, arr_reg, 0);
    xj_l32i(e, len_reg, arr_reg, 12);
    int b_bound = e->pos;
    xj_bgeu(e, idx_reg, len_reg, 0);
    int j_ok = e->pos;
    xj_j(e, 0);

    int deopt_pos = e->pos;
    patch_bz(e->buf, b_null, deopt_pos);
    patch_b8(e->buf, b_bound, deopt_pos);
    jit_emit_deopt(e, pc, restore_bytes);
    patch_jump(e->buf, j_ok, e->pos);
}

/* ------------------------------------------------------------------------ */
/* Compiler Entry                                                           */
/* ------------------------------------------------------------------------ */
static int s_jit_depth = 0;
static int s_cache_flushes = 0;
static bool s_jit_flush_pending = false;

static void reset_method_jit_cb(Class *c, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < c->n_methods; i++) {
        Method *meth = &c->methods[i];
        meth->jit_code = NULL;
        meth->jit_size = 0;
        meth->access &= ~ACC_JIT_OSR;
        meth->exec_count = 0;
        meth->jit_fails = 0;
        if (!meth->native && meth->name != S_clinit && meth->code_len <= MAX_CODE_SIZE) {
            meth->access &= ~ACC_JIT_UNSUPPORTED;
        }
    }
}

void dynarec_flush_cache(void)
{
    if (s_jit_depth > 0) {
        s_jit_flush_pending = true;
        return;
    }
    s_cache_flushes++;
    nj_log("[dynarec] Flushing JIT cache (#%d: %u methods, %u KB used)",
           s_cache_flushes, (unsigned)s_methods_compiled, (unsigned)(s_jit_cache_used / 1024));
    class_foreach(reset_method_jit_cb, NULL);
    s_jit_cache_used = 0;
    s_jit_cache_full = false;
    s_jit_flush_pending = false;
    s_methods_compiled = 0;
    if (s_jit_mem.data) xj_exec_sync(&s_jit_mem, 0, s_jit_mem.size);
}

void dynarec_check_pending_flush(void)
{
}

void dynarec_reset(void)
{
    s_jit_cache_used = 0;
    s_jit_cache_full = false;
    s_jit_flush_pending = false;
    s_methods_compiled = 0;
    s_jit_compile_attempts = 0;
    s_jit_compile_rejects = 0;
    s_jit_invocations = 0;
    s_jit_deopts = 0;
    s_jit_bytes_emitted = 0;
    s_jit_depth = 0;
    s_cache_flushes = 0;
}

void dynarec_shutdown(void)
{
    if (s_jit_mem.data) xj_exec_free(&s_jit_mem);
    if (s_lits)        { nj_pal_free(s_lits);        s_lits = NULL; }
    if (s_lit_offsets) { nj_pal_free(s_lit_offsets); s_lit_offsets = NULL; }
    if (s_pc_to_jit)   { nj_pal_free(s_pc_to_jit);   s_pc_to_jit = NULL; }
    if (s_fixups)      { nj_pal_free(s_fixups);      s_fixups = NULL; }
    if (s_resumes)     { nj_pal_free(s_resumes);     s_resumes = NULL; }
    if (s_is_target)   { nj_pal_free(s_is_target);   s_is_target = NULL; }
    dynarec_reset();
}

bool dynarec_compile_method(Method *m, Thread *t)
{
    if (!s_jit_mem.data || m->jit_code || !s_pc_to_jit || !s_fixups || !s_lits || !s_lit_offsets || !s_resumes || !s_is_target)
        return false;

    if (s_jit_cache_full)
        return false;

    size_t remaining = (s_jit_cache_used < s_jit_mem.size) ? (s_jit_mem.size - s_jit_cache_used) : 0;
    if (remaining < 4096) {
        if (!s_jit_cache_full) {
            s_jit_cache_full = true;
            nj_log("[dynarec] JIT cache full: %u methods compiled, %u KB used. Remaining methods run in interpreter.",
                   (unsigned)s_methods_compiled, (unsigned)(s_jit_cache_used / 1024));
        }
        if (t && t->exception) t->exception = NULL;
        s_jit_compile_rejects++;
        return false;
    }

    s_jit_compile_attempts++;
    if (!is_method_supported(m, t)) {
        if (t && t->exception) t->exception = NULL;
        s_jit_compile_rejects++;
        if (++m->jit_fails >= 10 || s_reject_reason != NULL) {
            m->access |= ACC_JIT_UNSUPPORTED;
        }
        m->exec_count = 0;
        return false;
    }

    xj_emit_t e;
    xj_init(&e, s_jit_mem.data + s_jit_cache_used, remaining);

    /* Prologue */
    xj_entry(&e, 1, 32);

    if (s_n_lits > 0) {
        int j_pos = e.pos;
        xj_j(&e, 0);
        while (e.pos % 4 != 0) xj_byte(&e, 0x00);
        for (int i = 0; i < s_n_lits; i++) {
            s_lit_offsets[i] = e.pos;
            xj_32(&e, s_lits[i]);
        }
        patch_jump(e.buf, j_pos, e.pos);
    }

    xj_l32i(&e, REG_LV, REG_F, offsetof(Frame, locals));
    xj_l32i(&e, REG_SP, REG_F, offsetof(Frame, sp));
    xj_l32i(&e, REG_CODE, REG_F, offsetof(Frame, m));
    xj_l32i(&e, REG_CODE, REG_CODE, offsetof(Method, code));

    if (s_n_resumes > 0) {
        xj_l32i(&e, REG_S1, REG_F, offsetof(Frame, pc));
        xj_sub(&e, REG_S1, REG_S1, REG_CODE);
        int b_not_zero = e.pos;
        xj_bnez(&e, REG_S1, 0);
        int j_start_body = e.pos;
        xj_j(&e, 0);
        patch_bz(e.buf, b_not_zero, e.pos);

        for (int i = 0; i < s_n_resumes; i++) {
            jit_emit_imm(&e, REG_S2, (int32_t)s_resumes[i].pc);
            int b_next = e.pos;
            xj_bne(&e, REG_S1, REG_S2, 0);
            s_resumes[i].patch_pos = e.pos;
            xj_j(&e, 0);
            patch_b8(e.buf, b_next, e.pos);
        }
        xj_movi(&e, REG_T, JIT_DEOPT);
        xj_retw_n(&e);
        patch_jump(e.buf, j_start_body, e.pos);
    } else {
        xj_l32i(&e, REG_S1, REG_F, offsetof(Frame, pc));
        int b_entry_ok = e.pos;
        xj_beq(&e, REG_S1, REG_CODE, 0);
        xj_movi(&e, REG_T, JIT_DEOPT);
        xj_retw_n(&e);
        patch_b8(e.buf, b_entry_ok, e.pos);
    }

    memset(s_pc_to_jit, -1, (m->code_len + 1) * sizeof(int32_t));
    int n_fixups = 0;
    int tos = 0; /* Current cached TOS register depth (0, 1, or 2) */

#define EMIT_BRANCH(target) do { \
        tos_spill(&e, &tos); \
        if (n_fixups >= MAX_FIXUPS) { \
            m->exec_count = 0; \
            m->access |= ACC_JIT_UNSUPPORTED; \
            return false; \
        } \
        if ((target) <= pc) { \
            xj_l32i(&e, REG_S4, REG_T, offsetof(Thread, budget)); \
            xj_addi(&e, REG_S4, REG_S4, -1); \
            xj_s32i(&e, REG_S4, REG_T, offsetof(Thread, budget)); \
            int b_budget_ok = e.pos; \
            xj_bgez(&e, REG_S4, 0); \
            jit_emit_deopt(&e, (target), 0); \
            patch_bz(e.buf, b_budget_ok, e.pos); \
        } \
        s_fixups[n_fixups].patch_pos = e.pos; \
        s_fixups[n_fixups++].target_pc = (target); \
        xj_j(&e, 0); \
    } while (0)

    const uint8_t *code = m->code;
    uint32_t len = m->code_len;

    for (uint32_t pc = 0; pc < len; ) {
        /* If this PC is a jump/loop target, flush any cached TOS registers first */
        if (s_is_target && s_is_target[pc]) {
            tos_spill(&e, &tos);
        }
        s_pc_to_jit[pc] = (int32_t)e.pos;
        uint8_t op = code[pc];

        switch (op) {
        case 0x00: pc++; break;

        case 0x01:
            xj_movi(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x02:
            xj_movi(&e, REG_S1, -1);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
            xj_movi(&e, REG_S1, op - 0x03);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x09:
            xj_movi(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x0a:
            xj_movi(&e, REG_S1, 1);
            tos_push(&e, &tos, REG_S1);
            xj_movi(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x0b:
            xj_movi(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x0c:
            jit_emit_imm(&e, REG_S1, 0x3f800000);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x0d:
            jit_emit_imm(&e, REG_S1, 0x40000000);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x0e:
            xj_movi(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x0f:
            xj_movi(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            jit_emit_imm(&e, REG_S1, 0x3ff00000);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x10:
            xj_movi(&e, REG_S1, (int8_t)code[pc + 1]);
            tos_push(&e, &tos, REG_S1);
            pc += 2;
            break;

        case 0x11: {
            int16_t val = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            jit_emit_imm(&e, REG_S1, val);
            tos_push(&e, &tos, REG_S1);
            pc += 3;
            break;
        }

        case 0x12: case 0x13: {
            uint16_t idx = (op == 0x12) ? code[pc + 1] : (((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            CPEntry *cpe = &m->cls->cp[idx];
            if (cpe->tag == CONSTANT_Integer || cpe->tag == CONSTANT_Float) {
                jit_emit_imm(&e, REG_S1, cpe->v.i);
                tos_push(&e, &tos, REG_S1);
            } else if (cpe->tag == CONSTANT_String) {
                jit_emit_imm(&e, REG_S1, (int32_t)(uintptr_t)cpe->v.str);
                tos_push(&e, &tos, REG_S1);
            } else if (cpe->tag == CONSTANT_Class) {
                Object *mir = class_mirror(cpe->v.cls);
                jit_emit_imm(&e, REG_S1, (int32_t)(uintptr_t)mir);
                tos_push(&e, &tos, REG_S1);
            }
            pc += (op == 0x12) ? 2 : 3;
            break;
        }

        case 0x14: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            jit_emit_imm(&e, REG_S1, (int32_t)m->cls->cp[idx + 1].v.u);
            tos_push(&e, &tos, REG_S1);
            jit_emit_imm(&e, REG_S1, (int32_t)m->cls->cp[idx].v.u);
            tos_push(&e, &tos, REG_S1);
            pc += 3;
            break;
        }

        case 0x15: case 0x17: case 0x19:
            xj_l32i(&e, REG_S1, REG_LV, code[pc + 1] * 4);
            tos_push(&e, &tos, REG_S1);
            pc += 2;
            break;

        case 0x16: case 0x18: {
            uint8_t idx = code[pc + 1];
            xj_l32i(&e, REG_S1, REG_LV, idx * 4);
            tos_push(&e, &tos, REG_S1);
            xj_l32i(&e, REG_S1, REG_LV, (idx + 1) * 4);
            tos_push(&e, &tos, REG_S1);
            pc += 2;
            break;
        }

        case 0x1a: case 0x1b: case 0x1c: case 0x1d:
            xj_l32i(&e, REG_S1, REG_LV, (op - 0x1a) * 4);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x1e: case 0x1f: case 0x20: case 0x21: {
            uint8_t idx = op - 0x1e;
            xj_l32i(&e, REG_S1, REG_LV, idx * 4);
            tos_push(&e, &tos, REG_S1);
            xj_l32i(&e, REG_S1, REG_LV, (idx + 1) * 4);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;
        }

        case 0x22: case 0x23: case 0x24: case 0x25:
            xj_l32i(&e, REG_S1, REG_LV, (op - 0x22) * 4);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x26: case 0x27: case 0x28: case 0x29: {
            uint8_t idx = op - 0x26;
            xj_l32i(&e, REG_S1, REG_LV, idx * 4);
            tos_push(&e, &tos, REG_S1);
            xj_l32i(&e, REG_S1, REG_LV, (idx + 1) * 4);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;
        }

        case 0x2a: case 0x2b: case 0x2c: case 0x2d:
            xj_l32i(&e, REG_S1, REG_LV, (op - 0x2a) * 4);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x36: case 0x38: case 0x3a:
            tos_pop(&e, &tos, REG_S1);
            xj_s32i(&e, REG_S1, REG_LV, code[pc + 1] * 4);
            pc += 2;
            break;

        case 0x37: case 0x39: {
            uint8_t idx = code[pc + 1];
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_s32i(&e, REG_S1, REG_LV, idx * 4);
            xj_s32i(&e, REG_S2, REG_LV, (idx + 1) * 4);
            pc += 2;
            break;
        }

        case 0x3b: case 0x3c: case 0x3d: case 0x3e:
            tos_pop(&e, &tos, REG_S1);
            xj_s32i(&e, REG_S1, REG_LV, (op - 0x3b) * 4);
            pc++;
            break;

        case 0x3f: case 0x40: case 0x41: case 0x42: {
            uint8_t idx = op - 0x3f;
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_s32i(&e, REG_S1, REG_LV, idx * 4);
            xj_s32i(&e, REG_S2, REG_LV, (idx + 1) * 4);
            pc++;
            break;
        }

        case 0x43: case 0x44: case 0x45: case 0x46:
            tos_pop(&e, &tos, REG_S1);
            xj_s32i(&e, REG_S1, REG_LV, (op - 0x43) * 4);
            pc++;
            break;

        case 0x47: case 0x48: case 0x49: case 0x4a: {
            uint8_t idx = op - 0x47;
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_s32i(&e, REG_S1, REG_LV, idx * 4);
            xj_s32i(&e, REG_S2, REG_LV, (idx + 1) * 4);
            pc++;
            break;
        }

        case 0x4b: case 0x4c: case 0x4d: case 0x4e:
            tos_pop(&e, &tos, REG_S1);
            xj_s32i(&e, REG_S1, REG_LV, (op - 0x4b) * 4);
            pc++;
            break;

        case 0x84: {
            uint8_t idx = code[pc + 1];
            int8_t c = (int8_t)code[pc + 2];
            xj_l32i(&e, REG_S1, REG_LV, idx * 4);
            xj_addi(&e, REG_S1, REG_S1, c);
            xj_s32i(&e, REG_S1, REG_LV, idx * 4);
            pc += 3;
            break;
        }

        case 0xc4: {
            uint8_t sub_op = code[pc + 1];
            uint16_t idx = ((uint16_t)code[pc + 2] << 8) | code[pc + 3];
            if (sub_op == 0x84) {
                int16_t cval = (int16_t)(((uint16_t)code[pc + 4] << 8) | code[pc + 5]);
                xj_l32i(&e, REG_S1, REG_LV, idx * 4);
                if (cval >= -128 && cval <= 127) xj_addi(&e, REG_S1, REG_S1, cval);
                else { jit_emit_imm(&e, REG_S2, cval); xj_add(&e, REG_S1, REG_S1, REG_S2); }
                xj_s32i(&e, REG_S1, REG_LV, idx * 4);
                pc += 6;
            } else if (sub_op == 0x15 || sub_op == 0x17 || sub_op == 0x19) {
                xj_l32i(&e, REG_S1, REG_LV, idx * 4);
                tos_push(&e, &tos, REG_S1);
                pc += 4;
            } else if (sub_op == 0x16 || sub_op == 0x18) {
                xj_l32i(&e, REG_S1, REG_LV, idx * 4);
                tos_push(&e, &tos, REG_S1);
                xj_l32i(&e, REG_S1, REG_LV, (idx + 1) * 4);
                tos_push(&e, &tos, REG_S1);
                pc += 4;
            } else if (sub_op == 0x36 || sub_op == 0x38 || sub_op == 0x3a) {
                tos_pop(&e, &tos, REG_S1);
                xj_s32i(&e, REG_S1, REG_LV, idx * 4);
                pc += 4;
            } else if (sub_op == 0x37 || sub_op == 0x39) {
                tos_pop2(&e, &tos, REG_S1, REG_S2);
                xj_s32i(&e, REG_S1, REG_LV, idx * 4);
                xj_s32i(&e, REG_S2, REG_LV, (idx + 1) * 4);
                pc += 4;
            } else {
                pc += 1;
            }
            break;
        }

        /* Stack operations (pure register moves when TOS is cached) */
        case 0x57: /* pop */
            if (tos == 2) { xj_mov_n(&e, REG_TOS0, REG_TOS1); tos = 1; }
            else if (tos == 1) { tos = 0; }
            else { xj_addi(&e, REG_SP, REG_SP, -4); }
            pc++;
            break;

        case 0x58: /* pop2 */
            if (tos == 2) { tos = 0; }
            else if (tos == 1) { xj_addi(&e, REG_SP, REG_SP, -4); tos = 0; }
            else { xj_addi(&e, REG_SP, REG_SP, -8); }
            pc++;
            break;

        case 0x59: /* dup */
            if (tos == 1) {
                xj_mov_n(&e, REG_TOS1, REG_TOS0);
                tos = 2;
            } else {
                tos_pop(&e, &tos, REG_S1);
                tos_push(&e, &tos, REG_S1);
                tos_push(&e, &tos, REG_S1);
            }
            pc++;
            break;

        case 0x5a: /* dup_x1 */
            tos_pop2(&e, &tos, REG_S2, REG_S1);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x5b: /* dup_x2 */
            tos_pop3(&e, &tos, REG_S3, REG_S2, REG_S1);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S3);
            tos_push(&e, &tos, REG_S2);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x5c: /* dup2 */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            pc++;
            break;

        case 0x5d: /* dup2_x1 */
            tos_pop3(&e, &tos, REG_S3, REG_S2, REG_S1);
            tos_push(&e, &tos, REG_S2);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S3);
            tos_push(&e, &tos, REG_S2);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x5e: /* dup2_x2 */
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S4, REG_SP, 0);
            xj_l32i(&e, REG_S3, REG_SP, 4);
            xj_l32i(&e, REG_S2, REG_SP, 8);
            xj_l32i(&e, REG_S1, REG_SP, 12);
            xj_s32i(&e, REG_S2, REG_SP, 0);
            xj_s32i(&e, REG_S1, REG_SP, 4);
            xj_s32i(&e, REG_S4, REG_SP, 8);
            xj_s32i(&e, REG_S3, REG_SP, 12);
            xj_s32i(&e, REG_S2, REG_SP, 16);
            xj_s32i(&e, REG_S1, REG_SP, 20);
            xj_addi(&e, REG_SP, REG_SP, 24);
            pc++;
            break;

        case 0x5f: /* swap */
            if (tos == 2) {
                xj_mov_n(&e, REG_S1, REG_TOS0);
                xj_mov_n(&e, REG_TOS0, REG_TOS1);
                xj_mov_n(&e, REG_TOS1, REG_S1);
            } else {
                tos_pop2(&e, &tos, REG_S1, REG_S2);
                tos_push(&e, &tos, REG_S2);
                tos_push(&e, &tos, REG_S1);
            }
            pc++;
            break;

        /* ALU & FPU Operations (zero RAM access when TOS is cached) */
        case 0x60: /* iadd */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_add(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x61: /* ladd */
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S1, REG_SP, 0);
            xj_l32i(&e, REG_S2, REG_SP, 4);
            xj_l32i(&e, REG_S3, REG_SP, 8);
            xj_l32i(&e, REG_S4, REG_SP, 12);
            xj_add(&e, REG_S3, REG_S1, REG_S3);
            xj_saltu(&e, REG_S1, REG_S3, REG_S1);
            xj_add(&e, REG_S2, REG_S2, REG_S4);
            xj_add(&e, REG_S2, REG_S2, REG_S1);
            xj_s32i(&e, REG_S3, REG_SP, 0);
            xj_s32i(&e, REG_S2, REG_SP, 4);
            xj_addi(&e, REG_SP, REG_SP, 8);
            pc++;
            break;

        case 0x62: /* fadd */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_wfr(&e, 0, REG_S1);
            xj_wfr(&e, 1, REG_S2);
            xj_add_s(&e, 0, 0, 1);
            xj_rfr(&e, REG_TOS0, 0);
            tos = 1;
            pc++;
            break;

        case 0x64: /* isub */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_sub(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x65: /* lsub */
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S1, REG_SP, 0);
            xj_l32i(&e, REG_S2, REG_SP, 4);
            xj_l32i(&e, REG_S3, REG_SP, 8);
            xj_l32i(&e, REG_S4, REG_SP, 12);
            xj_sub(&e, REG_S2, REG_S2, REG_S4);
            xj_saltu(&e, REG_S4, REG_S1, REG_S3);
            xj_sub(&e, REG_S1, REG_S1, REG_S3);
            xj_sub(&e, REG_S2, REG_S2, REG_S4);
            xj_s32i(&e, REG_S1, REG_SP, 0);
            xj_s32i(&e, REG_S2, REG_SP, 4);
            xj_addi(&e, REG_SP, REG_SP, 8);
            pc++;
            break;

        case 0x66: /* fsub */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_wfr(&e, 0, REG_S1);
            xj_wfr(&e, 1, REG_S2);
            xj_sub_s(&e, 0, 0, 1);
            xj_rfr(&e, REG_TOS0, 0);
            tos = 1;
            pc++;
            break;

        case 0x68: /* imul */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_mull(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x69: /* lmul */
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S1, REG_SP, 0);
            xj_l32i(&e, REG_S2, REG_SP, 4);
            xj_l32i(&e, REG_S3, REG_SP, 8);
            xj_l32i(&e, REG_S4, REG_SP, 12);
            xj_mull(&e, REG_S2, REG_S2, REG_S3);
            xj_mull(&e, REG_S4, REG_S1, REG_S4);
            xj_add(&e, REG_S2, REG_S2, REG_S4);
            xj_muluh(&e, REG_S4, REG_S1, REG_S3);
            xj_add(&e, REG_S2, REG_S2, REG_S4);
            xj_mull(&e, REG_S1, REG_S1, REG_S3);
            xj_s32i(&e, REG_S1, REG_SP, 0);
            xj_s32i(&e, REG_S2, REG_SP, 4);
            xj_addi(&e, REG_SP, REG_SP, 8);
            pc++;
            break;

        case 0x6a: /* fmul */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_wfr(&e, 0, REG_S1);
            xj_wfr(&e, 1, REG_S2);
            xj_mul_s(&e, 0, 0, 1);
            xj_rfr(&e, REG_TOS0, 0);
            tos = 1;
            pc++;
            break;

        case 0x6e: /* fdiv via direct C trampoline (no interpreter exit!) */
        case 0x72: { /* frem via direct C trampoline */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_spill(&e, &tos);
            xj_mov_n(&e, 10, REG_S1);
            xj_mov_n(&e, 11, REG_S2);
            jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)(op == 0x6e ? jit_rt_fdiv : jit_rt_frem));
            xj_callx8(&e, REG_S3);
            /* Save return value in REG_S1 before reloading REG_CODE */
            xj_mov_n(&e, REG_S1, 10);
            xj_l32i(&e, REG_CODE, REG_F, offsetof(Frame, m));
            xj_l32i(&e, REG_CODE, REG_CODE, offsetof(Method, code));
            xj_mov_n(&e, REG_TOS0, REG_S1);
            tos = 1;
            pc++;
            break;
        }

        case 0x6c: /* idiv */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_zero_check2(&e, REG_S1, REG_S2, pc);
            xj_quos(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x70: /* irem */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_zero_check2(&e, REG_S1, REG_S2, pc);
            xj_rems(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x74: /* ineg */
            tos_pop(&e, &tos, REG_S1);
            xj_neg(&e, REG_S1, REG_S1);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x75: /* lneg */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_movi(&e, REG_S3, 0);
            xj_beqz(&e, REG_S1, 2);
            xj_movi(&e, REG_S3, 1);
            xj_neg(&e, REG_S1, REG_S1);
            xj_neg(&e, REG_S2, REG_S2);
            xj_sub(&e, REG_S2, REG_S2, REG_S3);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            pc++;
            break;

        case 0x76: /* fneg */
            tos_pop(&e, &tos, REG_S1);
            xj_wfr(&e, 0, REG_S1);
            xj_neg_s(&e, 0, 0);
            xj_rfr(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x78: /* ishl */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_ssl(&e, REG_S2);
            xj_sll(&e, REG_TOS0, REG_S1);
            tos = 1;
            pc++;
            break;

        case 0x79: { /* lshl */
            tos_pop3(&e, &tos, REG_S1, REG_S2, REG_S3);
            xj_extui(&e, REG_S3, REG_S3, 0, 6);
            int b_zero = e.pos;
            xj_beqz(&e, REG_S3, 0);
            xj_movi(&e, REG_S4, 32);
            int b_ge32 = e.pos;
            xj_bgeu(&e, REG_S3, REG_S4, 0);
            xj_ssl(&e, REG_S3);
            xj_src(&e, REG_S2, REG_S2, REG_S1);
            xj_sll(&e, REG_S1, REG_S1);
            int j_done = e.pos;
            xj_j(&e, 0);
            patch_b8(e.buf, b_ge32, e.pos);
            xj_ssl(&e, REG_S3);
            xj_sll(&e, REG_S2, REG_S1);
            xj_movi(&e, REG_S1, 0);
            int done_pos = e.pos;
            patch_bz(e.buf, b_zero, done_pos);
            patch_jump(e.buf, j_done, done_pos);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            pc++;
            break;
        }

        case 0x7a: /* ishr */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_ssr(&e, REG_S2);
            xj_sra(&e, REG_TOS0, REG_S1);
            tos = 1;
            pc++;
            break;

        case 0x7b: { /* lshr */
            tos_pop3(&e, &tos, REG_S1, REG_S2, REG_S3);
            xj_extui(&e, REG_S3, REG_S3, 0, 6);
            int b_zero = e.pos;
            xj_beqz(&e, REG_S3, 0);
            xj_movi(&e, REG_S4, 32);
            int b_ge32 = e.pos;
            xj_bgeu(&e, REG_S3, REG_S4, 0);
            xj_ssr(&e, REG_S3);
            xj_src(&e, REG_S1, REG_S2, REG_S1);
            xj_sra(&e, REG_S2, REG_S2);
            int j_done = e.pos;
            xj_j(&e, 0);
            patch_b8(e.buf, b_ge32, e.pos);
            xj_ssr(&e, REG_S3);
            xj_sra(&e, REG_S1, REG_S2);
            xj_srai(&e, REG_S2, REG_S2, 31);
            int done_pos = e.pos;
            patch_bz(e.buf, b_zero, done_pos);
            patch_jump(e.buf, j_done, done_pos);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            pc++;
            break;
        }

        case 0x7c: /* iushr */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_ssr(&e, REG_S2);
            xj_srl(&e, REG_TOS0, REG_S1);
            tos = 1;
            pc++;
            break;

        case 0x7d: { /* lushr */
            tos_pop3(&e, &tos, REG_S1, REG_S2, REG_S3);
            xj_extui(&e, REG_S3, REG_S3, 0, 6);
            int b_zero = e.pos;
            xj_beqz(&e, REG_S3, 0);
            xj_movi(&e, REG_S4, 32);
            int b_ge32 = e.pos;
            xj_bgeu(&e, REG_S3, REG_S4, 0);
            xj_ssr(&e, REG_S3);
            xj_src(&e, REG_S1, REG_S2, REG_S1);
            xj_srl(&e, REG_S2, REG_S2);
            int j_done = e.pos;
            xj_j(&e, 0);
            patch_b8(e.buf, b_ge32, e.pos);
            xj_ssr(&e, REG_S3);
            xj_srl(&e, REG_S1, REG_S2);
            xj_movi(&e, REG_S2, 0);
            int done_pos = e.pos;
            patch_bz(e.buf, b_zero, done_pos);
            patch_jump(e.buf, j_done, done_pos);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            pc++;
            break;
        }

        case 0x7e: /* iand */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_and(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x7f: /* land */
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S1, REG_SP, 0);
            xj_l32i(&e, REG_S2, REG_SP, 4);
            xj_l32i(&e, REG_S3, REG_SP, 8);
            xj_l32i(&e, REG_S4, REG_SP, 12);
            xj_and(&e, REG_S1, REG_S1, REG_S3);
            xj_and(&e, REG_S2, REG_S2, REG_S4);
            xj_s32i(&e, REG_S1, REG_SP, 0);
            xj_s32i(&e, REG_S2, REG_SP, 4);
            xj_addi(&e, REG_SP, REG_SP, 8);
            pc++;
            break;

        case 0x80: /* ior */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_or(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x81: /* lor */
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S1, REG_SP, 0);
            xj_l32i(&e, REG_S2, REG_SP, 4);
            xj_l32i(&e, REG_S3, REG_SP, 8);
            xj_l32i(&e, REG_S4, REG_SP, 12);
            xj_or(&e, REG_S1, REG_S1, REG_S3);
            xj_or(&e, REG_S2, REG_S2, REG_S4);
            xj_s32i(&e, REG_S1, REG_SP, 0);
            xj_s32i(&e, REG_S2, REG_SP, 4);
            xj_addi(&e, REG_SP, REG_SP, 8);
            pc++;
            break;

        case 0x82: /* ixor */
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_xor(&e, REG_TOS0, REG_S1, REG_S2);
            tos = 1;
            pc++;
            break;

        case 0x83: /* lxor */
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S1, REG_SP, 0);
            xj_l32i(&e, REG_S2, REG_SP, 4);
            xj_l32i(&e, REG_S3, REG_SP, 8);
            xj_l32i(&e, REG_S4, REG_SP, 12);
            xj_xor(&e, REG_S1, REG_S1, REG_S3);
            xj_xor(&e, REG_S2, REG_S2, REG_S4);
            xj_s32i(&e, REG_S1, REG_SP, 0);
            xj_s32i(&e, REG_S2, REG_SP, 4);
            xj_addi(&e, REG_SP, REG_SP, 8);
            pc++;
            break;

        case 0x85: /* i2l */
            tos_pop(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S1);
            xj_srai(&e, REG_S1, REG_S1, 31);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x86: /* i2f */
            tos_pop(&e, &tos, REG_S1);
            xj_float_s(&e, 0, REG_S1);
            xj_rfr(&e, REG_S1, 0);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x88: /* l2i */
            tos_pop(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x8b: { /* f2i */
            tos_pop(&e, &tos, REG_S1);
            xj_extui(&e, REG_S2, REG_S1, 23, 8);
            xj_movi(&e, REG_S3, 158);
            int b_clamp = e.pos;
            xj_bge(&e, REG_S2, REG_S3, 0);
            xj_wfr(&e, 0, REG_S1);
            xj_trunc_s(&e, REG_S1, 0);
            int j_done = e.pos;
            xj_j(&e, 0);
            patch_b8(e.buf, b_clamp, e.pos);
            xj_movi(&e, REG_S3, 255);
            int b_overflow = e.pos;
            xj_bne(&e, REG_S2, REG_S3, 0);
            xj_slli(&e, REG_S2, REG_S1, 9);
            int b_inf = e.pos;
            xj_beqz(&e, REG_S2, 0);
            xj_movi(&e, REG_S1, 0);
            int j_nan_done = e.pos;
            xj_j(&e, 0);
            patch_b8(e.buf, b_overflow, e.pos);
            patch_bz(e.buf, b_inf, e.pos);
            xj_movi(&e, REG_S3, 1);
            xj_slli(&e, REG_S3, REG_S3, 31);
            int b_neg = e.pos;
            xj_bltz(&e, REG_S1, 0);
            xj_addi(&e, REG_S1, REG_S3, -1);
            int j_pos_done = e.pos;
            xj_j(&e, 0);
            patch_bz(e.buf, b_neg, e.pos);
            xj_mov_n(&e, REG_S1, REG_S3);
            int pos_f2i_done = e.pos;
            patch_jump(e.buf, j_done, pos_f2i_done);
            patch_jump(e.buf, j_nan_done, pos_f2i_done);
            patch_jump(e.buf, j_pos_done, pos_f2i_done);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;
        }

        case 0x91:
            tos_pop(&e, &tos, REG_S1);
            xj_sext(&e, REG_S1, REG_S1, 7);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x92:
            tos_pop(&e, &tos, REG_S1);
            xj_extui(&e, REG_S1, REG_S1, 0, 16);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0x93:
            tos_pop(&e, &tos, REG_S1);
            xj_sext(&e, REG_S1, REG_S1, 15);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        case 0xbe:
            tos_pop(&e, &tos, REG_S1);
            jit_emit_null_check1(&e, REG_S1, pc);
            xj_l32i(&e, REG_S1, REG_S1, 12);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;

        /* Direct Object & Array Allocation Trampolines (no interpreter exit!) */
        case 0xbb: { /* new */
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            Class *target_cls = m->cls->cp[idx].v.cls;
            tos_spill(&e, &tos);
            xj_mov_n(&e, 10, REG_T);
            jit_emit_imm(&e, 11, (int32_t)(uintptr_t)target_cls);
            jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)jit_rt_new);
            xj_callx8(&e, REG_S3);
            /* Save returned Object* in REG_S1 before reloading REG_CODE (a10) */
            xj_mov_n(&e, REG_S1, 10);
            xj_l32i(&e, REG_CODE, REG_F, offsetof(Frame, m));
            xj_l32i(&e, REG_CODE, REG_CODE, offsetof(Method, code));
            int b_ok = e.pos;
            xj_bnez(&e, REG_S1, 0);
            /* Uninitialized class or OOM: deopt cleanly to interpreter */
            jit_emit_deopt(&e, pc, 0);
            patch_bz(e.buf, b_ok, e.pos);
            xj_mov_n(&e, REG_TOS0, REG_S1);
            tos = 1;
            pc += 3;
            break;
        }

        case 0xbc: { /* newarray */
            uint8_t atype = code[pc + 1];
            tos_pop(&e, &tos, REG_S1); /* count */
            tos_spill(&e, &tos);
            xj_mov_n(&e, 10, REG_T);
            xj_movi(&e, 11, (int32_t)atype);
            xj_mov_n(&e, 12, REG_S1);
            jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)jit_rt_newarray);
            xj_callx8(&e, REG_S3);
            /* Save returned Array* in REG_S2 before reloading REG_CODE (a10) */
            xj_mov_n(&e, REG_S2, 10);
            xj_l32i(&e, REG_CODE, REG_F, offsetof(Frame, m));
            xj_l32i(&e, REG_CODE, REG_CODE, offsetof(Method, code));
            int b_ok = e.pos;
            xj_bnez(&e, REG_S2, 0);
            /* NegativeArraySizeException or OOM: restore count and deopt */
            jit_push(&e, REG_S1);
            jit_emit_deopt(&e, pc, 0);
            patch_bz(e.buf, b_ok, e.pos);
            xj_mov_n(&e, REG_TOS0, REG_S2);
            tos = 1;
            pc += 2;
            break;
        }

        case 0xbd: { /* anewarray */
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            Class *elem_cls = m->cls->cp[idx].v.cls;
            tos_pop(&e, &tos, REG_S1); /* count */
            tos_spill(&e, &tos);
            xj_mov_n(&e, 10, REG_T);
            jit_emit_imm(&e, 11, (int32_t)(uintptr_t)elem_cls);
            xj_mov_n(&e, 12, REG_S1);
            jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)jit_rt_anewarray);
            xj_callx8(&e, REG_S3);
            /* Save returned Array* in REG_S2 before reloading REG_CODE (a10) */
            xj_mov_n(&e, REG_S2, 10);
            xj_l32i(&e, REG_CODE, REG_F, offsetof(Frame, m));
            xj_l32i(&e, REG_CODE, REG_CODE, offsetof(Method, code));
            int b_ok = e.pos;
            xj_bnez(&e, REG_S2, 0);
            jit_push(&e, REG_S1);
            jit_emit_deopt(&e, pc, 0);
            patch_bz(e.buf, b_ok, e.pos);
            xj_mov_n(&e, REG_TOS0, REG_S2);
            tos = 1;
            pc += 3;
            break;
        }

        case 0xb2: case 0xb3: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            CPEntry *cpe = &m->cls->cp[idx];
            Field *fl = cpe->v.field;
            Slot *s = &fl->cls->statics[fl->offset];
            bool wide = (fl->desc[0] == 'J' || fl->desc[0] == 'D');
            if (op == 0xb2) {
                jit_emit_imm(&e, REG_S1, (int32_t)(uintptr_t)s);
                xj_l32i(&e, REG_S2, REG_S1, 0);
                tos_push(&e, &tos, REG_S2);
                if (wide) {
                    xj_l32i(&e, REG_S2, REG_S1, 4);
                    tos_push(&e, &tos, REG_S2);
                }
            } else {
                if (wide) {
                    tos_pop2(&e, &tos, REG_S2, REG_S3);
                    jit_emit_imm(&e, REG_S1, (int32_t)(uintptr_t)s);
                    xj_s32i(&e, REG_S2, REG_S1, 0);
                    xj_s32i(&e, REG_S3, REG_S1, 4);
                } else {
                    tos_pop(&e, &tos, REG_S2);
                    jit_emit_imm(&e, REG_S1, (int32_t)(uintptr_t)s);
                    xj_s32i(&e, REG_S2, REG_S1, 0);
                }
            }
            pc += 3;
            break;
        }

        case 0xca: {
            uint8_t slot_idx = code[pc + 1];
            tos_pop(&e, &tos, REG_S1);
            jit_emit_null_check1(&e, REG_S1, pc);
            if (12 + slot_idx * 4 <= 1020) {
                xj_l32i(&e, REG_S1, REG_S1, 12 + slot_idx * 4);
            } else {
                jit_emit_imm(&e, REG_S2, 12 + slot_idx * 4);
                xj_add(&e, REG_S1, REG_S1, REG_S2);
                xj_l32i(&e, REG_S1, REG_S1, 0);
            }
            tos_push(&e, &tos, REG_S1);
            pc += 3;
            break;
        }

        case 0xcb: {
            uint8_t slot_idx = code[pc + 1];
            tos_pop(&e, &tos, REG_S1);
            jit_emit_null_check1(&e, REG_S1, pc);
            xj_l32i(&e, REG_S2, REG_S1, 12 + slot_idx * 4);
            xj_l32i(&e, REG_S3, REG_S1, 16 + slot_idx * 4);
            tos_push(&e, &tos, REG_S2);
            tos_push(&e, &tos, REG_S3);
            pc += 3;
            break;
        }

        case 0xcc: {
            uint8_t slot_idx = code[pc + 1];
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_null_check2(&e, REG_S1, REG_S2, pc);
            xj_s32i(&e, REG_S2, REG_S1, 12 + slot_idx * 4);
            pc += 3;
            break;
        }

        case 0xcd: {
            uint8_t slot_idx = code[pc + 1];
            tos_pop3(&e, &tos, REG_S1, REG_S2, REG_S3);
            jit_emit_null_check3(&e, REG_S1, REG_S2, REG_S3, pc);
            xj_s32i(&e, REG_S2, REG_S1, 12 + slot_idx * 4);
            xj_s32i(&e, REG_S3, REG_S1, 16 + slot_idx * 4);
            pc += 3;
            break;
        }

        case 0xc0: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            Class *target_cls = m->cls->cp[idx].v.cls;
            tos_pop(&e, &tos, REG_S1);
            if (!(target_cls->flags & (CF_INTERFACE | CF_ARRAY))) {
                int b_null = e.pos;
                xj_beqz(&e, REG_S1, 0);
                xj_l32i(&e, REG_S2, REG_S1, 0);
                jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)target_cls);
                int b_exact = e.pos;
                xj_beq(&e, REG_S2, REG_S3, 0);
                int loop_top = e.pos;
                xj_l32i(&e, REG_S2, REG_S2, 4);
                int b_fail = e.pos;
                xj_beqz(&e, REG_S2, 0);
                int b_match = e.pos;
                xj_beq(&e, REG_S2, REG_S3, 0);
                int j_loop = e.pos;
                xj_j(&e, 0);
                patch_jump(e.buf, j_loop, loop_top);
                patch_bz(e.buf, b_fail, e.pos);
                tos_spill(&e, &tos);
                jit_push(&e, REG_S1);
                jit_emit_deopt(&e, pc, 0);
                int done_pos = e.pos;
                patch_bz(e.buf, b_null, done_pos);
                patch_b8(e.buf, b_exact, done_pos);
                patch_b8(e.buf, b_match, done_pos);
            } else {
                tos_spill(&e, &tos);
                xj_mov_n(&e, 10, REG_S1);
                jit_emit_imm(&e, 11, (int32_t)(uintptr_t)target_cls);
                jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)jit_rt_checkcast);
                xj_callx8(&e, REG_S3);
                /* Save result in REG_S2 before reloading REG_CODE (a10) */
                xj_mov_n(&e, REG_S2, 10);
                xj_l32i(&e, REG_CODE, REG_F, offsetof(Frame, m));
                xj_l32i(&e, REG_CODE, REG_CODE, offsetof(Method, code));
                int b_ok = e.pos;
                xj_bnez(&e, REG_S2, 0);
                jit_push(&e, REG_S1);
                jit_emit_deopt(&e, pc, 0);
                patch_bz(e.buf, b_ok, e.pos);
            }
            tos_push(&e, &tos, REG_S1);
            pc += 3;
            break;
        }

        case 0xc1: {
            uint16_t idx = ((uint16_t)code[pc + 1] << 8) | code[pc + 2];
            Class *target_cls = m->cls->cp[idx].v.cls;
            tos_pop(&e, &tos, REG_S1);
            if (!(target_cls->flags & (CF_INTERFACE | CF_ARRAY))) {
                int b_null = e.pos;
                xj_beqz(&e, REG_S1, 0);
                xj_l32i(&e, REG_S2, REG_S1, 0);
                jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)target_cls);
                int b_exact = e.pos;
                xj_beq(&e, REG_S2, REG_S3, 0);
                int loop_top = e.pos;
                xj_l32i(&e, REG_S2, REG_S2, 4);
                int b_null_super = e.pos;
                xj_beqz(&e, REG_S2, 0);
                int b_match = e.pos;
                xj_beq(&e, REG_S2, REG_S3, 0);
                int j_loop = e.pos;
                xj_j(&e, 0);
                patch_jump(e.buf, j_loop, loop_top);
                patch_bz(e.buf, b_null_super, e.pos);
                xj_movi(&e, REG_S1, 0);
                int j_done1 = e.pos;
                xj_j(&e, 0);
                int match_pos = e.pos;
                patch_b8(e.buf, b_match, match_pos);
                patch_b8(e.buf, b_exact, match_pos);
                xj_movi(&e, REG_S1, 1);
                int j_done2 = e.pos;
                xj_j(&e, 0);
                patch_bz(e.buf, b_null, e.pos);
                xj_movi(&e, REG_S1, 0);
                int done_pos = e.pos;
                patch_jump(e.buf, j_done1, done_pos);
                patch_jump(e.buf, j_done2, done_pos);
            } else {
                tos_spill(&e, &tos);
                xj_mov_n(&e, 10, REG_S1);
                jit_emit_imm(&e, 11, (int32_t)(uintptr_t)target_cls);
                jit_emit_imm(&e, REG_S3, (int32_t)(uintptr_t)jit_rt_instanceof);
                xj_callx8(&e, REG_S3);
                /* Save result in REG_S1 before reloading REG_CODE (a10) */
                xj_mov_n(&e, REG_S1, 10);
                xj_l32i(&e, REG_CODE, REG_F, offsetof(Frame, m));
                xj_l32i(&e, REG_CODE, REG_CODE, offsetof(Method, code));
            }
            tos_push(&e, &tos, REG_S1);
            pc += 3;
            break;
        }

        /* Arrays */
        case 0x2e: case 0x30: case 0x32:
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_array_check(&e, REG_S1, REG_S2, REG_S4, pc);
            xj_addx4(&e, REG_S3, REG_S2, REG_S1);
            xj_l32i(&e, REG_TOS0, REG_S3, 16);
            tos = 1;
            pc++;
            break;

        case 0x2f: case 0x31:
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_array_check(&e, REG_S1, REG_S2, REG_S4, pc);
            xj_slli(&e, REG_S2, REG_S2, 3);
            xj_add(&e, REG_S3, REG_S2, REG_S1);
            xj_l32i(&e, REG_S1, REG_S3, 16);
            xj_l32i(&e, REG_S2, REG_S3, 20);
            tos_push(&e, &tos, REG_S1);
            tos_push(&e, &tos, REG_S2);
            pc++;
            break;

        case 0x33:
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_array_check(&e, REG_S1, REG_S2, REG_S4, pc);
            xj_add(&e, REG_S3, REG_S2, REG_S1);
            xj_l8ui(&e, REG_S1, REG_S3, 16);
            xj_sext(&e, REG_TOS0, REG_S1, 7);
            tos = 1;
            pc++;
            break;

        case 0x34:
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_array_check(&e, REG_S1, REG_S2, REG_S4, pc);
            xj_addx2(&e, REG_S3, REG_S2, REG_S1);
            xj_l16ui(&e, REG_TOS0, REG_S3, 16);
            tos = 1;
            pc++;
            break;

        case 0x35:
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            jit_emit_array_check(&e, REG_S1, REG_S2, REG_S4, pc);
            xj_addx2(&e, REG_S3, REG_S2, REG_S1);
            xj_l16si(&e, REG_TOS0, REG_S3, 16);
            tos = 1;
            pc++;
            break;

        case 0x4f: case 0x51: case 0x53:
            tos_pop3(&e, &tos, REG_S1, REG_S2, REG_S3);
            jit_emit_array_check_store(&e, REG_S1, REG_S2, REG_S3, REG_S4, pc);
            xj_addx4(&e, REG_S1, REG_S2, REG_S1);
            xj_s32i(&e, REG_S3, REG_S1, 16);
            pc++;
            break;

        case 0x50: case 0x52:
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S4, REG_SP, 0);
            xj_l32i(&e, REG_S1, REG_SP, 4);
            jit_emit_array_check_spilled(&e, REG_S4, REG_S1, REG_S2, pc, 16);
            xj_l32i(&e, REG_S2, REG_SP, 8);
            xj_l32i(&e, REG_S3, REG_SP, 12);
            xj_slli(&e, REG_S1, REG_S1, 3);
            xj_add(&e, REG_S1, REG_S1, REG_S4);
            xj_s32i(&e, REG_S2, REG_S1, 16);
            xj_s32i(&e, REG_S3, REG_S1, 20);
            pc++;
            break;

        case 0x54:
            tos_pop3(&e, &tos, REG_S1, REG_S2, REG_S3);
            jit_emit_array_check_store(&e, REG_S1, REG_S2, REG_S3, REG_S4, pc);
            xj_add(&e, REG_S1, REG_S2, REG_S1);
            xj_s8i(&e, REG_S3, REG_S1, 16);
            pc++;
            break;

        case 0x55: case 0x56:
            tos_pop3(&e, &tos, REG_S1, REG_S2, REG_S3);
            jit_emit_array_check_store(&e, REG_S1, REG_S2, REG_S3, REG_S4, pc);
            xj_addx2(&e, REG_S1, REG_S2, REG_S1);
            xj_s16i(&e, REG_S3, REG_S1, 16);
            pc++;
            break;

        case 0x94: {
            tos_spill(&e, &tos);
            xj_addi(&e, REG_SP, REG_SP, -16);
            xj_l32i(&e, REG_S1, REG_SP, 0);
            xj_l32i(&e, REG_S2, REG_SP, 4);
            xj_l32i(&e, REG_S3, REG_SP, 8);
            xj_l32i(&e, REG_S4, REG_SP, 12);
            int b_hi_lt = e.pos; xj_blt(&e, REG_S2, REG_S4, 0);
            int b_hi_gt = e.pos; xj_blt(&e, REG_S4, REG_S2, 0);
            int b_lo_lt = e.pos; xj_bltu(&e, REG_S1, REG_S3, 0);
            int b_lo_gt = e.pos; xj_bltu(&e, REG_S3, REG_S1, 0);
            xj_movi(&e, REG_S1, 0);
            int j_done = e.pos; xj_j(&e, 0);
            int pos_lt = e.pos;
            patch_b8(e.buf, b_hi_lt, pos_lt); patch_b8(e.buf, b_lo_lt, pos_lt);
            xj_movi(&e, REG_S1, -1);
            int j_done2 = e.pos; xj_j(&e, 0);
            int pos_gt = e.pos;
            patch_b8(e.buf, b_hi_gt, pos_gt); patch_b8(e.buf, b_lo_gt, pos_gt);
            xj_movi(&e, REG_S1, 1);
            int pos_done = e.pos;
            patch_jump(e.buf, j_done, pos_done); patch_jump(e.buf, j_done2, pos_done);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;
        }

        case 0x95: case 0x96: {
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_wfr(&e, 1, REG_S1);
            xj_wfr(&e, 0, REG_S2);
            xj_olt_s(&e, 0, 1, 0);
            xj_movi(&e, REG_S1, -1);
            int b_lt = e.pos; xj_bt(&e, 0, 0);
            xj_olt_s(&e, 0, 0, 1);
            xj_movi(&e, REG_S1, 1);
            int b_gt = e.pos; xj_bt(&e, 0, 0);
            xj_oeq_s(&e, 0, 1, 0);
            xj_movi(&e, REG_S3, 0);
            xj_movt(&e, REG_S1, REG_S3, 0);
            if (op == 0x95) xj_neg(&e, REG_S1, REG_S1);
            int done_pos = e.pos;
            patch_b8(e.buf, b_lt, done_pos); patch_b8(e.buf, b_gt, done_pos);
            tos_push(&e, &tos, REG_S1);
            pc++;
            break;
        }

        /* Branches (spill TOS before conditional branch) */
        case 0x99: case 0xc6: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_bnez(&e, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_bz(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0x9a: case 0xc7: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_beqz(&e, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_bz(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0x9b: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_bgez(&e, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_bz(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0x9c: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_bltz(&e, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_bz(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0x9d: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            xj_movi(&e, REG_S2, 0);
            int bpos = e.pos;
            xj_bge(&e, REG_S2, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0x9e: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            xj_movi(&e, REG_S2, 0);
            int bpos = e.pos;
            xj_blt(&e, REG_S2, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0x9f: case 0xa5: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_bne(&e, REG_S1, REG_S2, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0xa0: case 0xa6: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_beq(&e, REG_S1, REG_S2, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0xa1: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_bge(&e, REG_S1, REG_S2, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0xa2: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_blt(&e, REG_S1, REG_S2, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0xa3: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_bge(&e, REG_S2, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0xa4: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            tos_spill(&e, &tos);
            int bpos = e.pos;
            xj_blt(&e, REG_S2, REG_S1, 0);
            EMIT_BRANCH(pc + offset);
            patch_b8(e.buf, bpos, e.pos);
            pc += 3;
            break;
        }

        case 0xa7: {
            int16_t offset = (int16_t)(((uint16_t)code[pc + 1] << 8) | code[pc + 2]);
            EMIT_BRANCH(pc + offset);
            pc += 3;
            break;
        }

        case 0xc8: {
            int32_t offset = (int32_t)(((uint32_t)code[pc + 1] << 24) | ((uint32_t)code[pc + 2] << 16) |
                                      ((uint32_t)code[pc + 3] << 8) | code[pc + 4]);
            EMIT_BRANCH(pc + offset);
            pc += 5;
            break;
        }

        case 0xaa: {
            uint32_t pad = (4 - ((pc + 1) % 4)) % 4;
            uint32_t p = pc + 1 + pad;
            int32_t dflt = (int32_t)(((uint32_t)code[p] << 24) | ((uint32_t)code[p+1] << 16) | ((uint32_t)code[p+2] << 8) | code[p+3]);
            int32_t low  = (int32_t)(((uint32_t)code[p+4] << 24) | ((uint32_t)code[p+5] << 16) | ((uint32_t)code[p+6] << 8) | code[p+7]);
            int32_t high = (int32_t)(((uint32_t)code[p+8] << 24) | ((uint32_t)code[p+9] << 16) | ((uint32_t)code[p+10] << 8) | code[p+11]);
            int32_t count = high - low + 1;
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            for (int32_t i = 0; i < count; i++) {
                int32_t target_offset = (int32_t)(((uint32_t)code[p + 12 + i * 4] << 24) |
                                                  ((uint32_t)code[p + 12 + i * 4 + 1] << 16) |
                                                  ((uint32_t)code[p + 12 + i * 4 + 2] << 8) |
                                                  code[p + 12 + i * 4 + 3]);
                jit_emit_imm(&e, REG_S2, low + i);
                int bpos = e.pos;
                xj_bne(&e, REG_S1, REG_S2, 0);
                EMIT_BRANCH(pc + target_offset);
                patch_b8(e.buf, bpos, e.pos);
            }
            EMIT_BRANCH(pc + dflt);
            pc = p + 12 + count * 4;
            break;
        }

        case 0xab: {
            uint32_t pad = (4 - ((pc + 1) % 4)) % 4;
            uint32_t p = pc + 1 + pad;
            int32_t dflt = (int32_t)(((uint32_t)code[p] << 24) | ((uint32_t)code[p+1] << 16) | ((uint32_t)code[p+2] << 8) | code[p+3]);
            int32_t npairs = (int32_t)(((uint32_t)code[p+4] << 24) | ((uint32_t)code[p+5] << 16) | ((uint32_t)code[p+6] << 8) | code[p+7]);
            tos_pop(&e, &tos, REG_S1);
            tos_spill(&e, &tos);
            for (int32_t i = 0; i < npairs; i++) {
                int32_t match = (int32_t)(((uint32_t)code[p + 8 + i * 8] << 24) |
                                          ((uint32_t)code[p + 8 + i * 8 + 1] << 16) |
                                          ((uint32_t)code[p + 8 + i * 8 + 2] << 8) |
                                          code[p + 8 + i * 8 + 3]);
                int32_t target_offset = (int32_t)(((uint32_t)code[p + 8 + i * 8 + 4] << 24) |
                                                  ((uint32_t)code[p + 8 + i * 8 + 5] << 16) |
                                                  ((uint32_t)code[p + 8 + i * 8 + 6] << 8) |
                                                  code[p + 8 + i * 8 + 7]);
                jit_emit_imm(&e, REG_S2, match);
                int bpos = e.pos;
                xj_bne(&e, REG_S1, REG_S2, 0);
                EMIT_BRANCH(pc + target_offset);
                patch_b8(e.buf, bpos, e.pos);
            }
            EMIT_BRANCH(pc + dflt);
            pc = p + 8 + npairs * 8;
            break;
        }

        case 0xb6: case 0xb7: case 0xb8: case 0xce:
            jit_emit_yield(&e, &tos, pc);
            pc += 3;
            break;

        case 0xb9:
            jit_emit_yield(&e, &tos, pc);
            pc += 5;
            break;

        case 0xc5:
            jit_emit_yield(&e, &tos, pc);
            pc += 4;
            break;

        case 0x6d: case 0x71:
        case 0x63: case 0x67: case 0x6b: case 0x6f: case 0x73: case 0x77:
        case 0x87: case 0x89: case 0x8a: case 0x8c: case 0x8d: case 0x8e: case 0x8f: case 0x90: case 0x97: case 0x98:
            jit_emit_yield(&e, &tos, pc);
            pc += 1;
            break;

        case 0xb4: case 0xb5:
            jit_emit_yield(&e, &tos, pc);
            pc += 3;
            break;

        case 0xac: case 0xae: case 0xb0:
            tos_pop(&e, &tos, REG_S1);
            xj_s32i(&e, REG_S1, REG_LV, 0);
            xj_s32i(&e, REG_SP, REG_F, offsetof(Frame, sp));
            xj_movi(&e, REG_T, JIT_OK);
            xj_retw_n(&e);
            pc++;
            break;

        case 0xad: case 0xaf:
            tos_pop2(&e, &tos, REG_S1, REG_S2);
            xj_s32i(&e, REG_S1, REG_LV, 0);
            xj_s32i(&e, REG_S2, REG_LV, 4);
            xj_s32i(&e, REG_SP, REG_F, offsetof(Frame, sp));
            xj_movi(&e, REG_T, JIT_OK);
            xj_retw_n(&e);
            pc++;
            break;

        case 0xb1:
            xj_s32i(&e, REG_SP, REG_F, offsetof(Frame, sp));
            xj_movi(&e, REG_T, JIT_OK);
            xj_retw_n(&e);
            pc++;
            break;

        case 0xbf:
            tos_spill(&e, &tos);
            jit_emit_deopt(&e, pc, 0);
            pc++;
            break;

        case 0xc2: case 0xc3:
            jit_emit_yield(&e, &tos, pc);
            pc++;
            break;

        case 0xa8: case 0xa9: case 0xc9: {
            uint32_t step = (op == 0xa9 ? 2 : (op == 0xc9 ? 5 : 3));
            jit_emit_yield(&e, &tos, pc);
            pc += step;
            break;
        }

        default:
            tos_spill(&e, &tos);
            xj_s32i(&e, REG_SP, REG_F, offsetof(Frame, sp));
            xj_movi(&e, REG_T, JIT_DEOPT);
            xj_retw_n(&e);
            goto compile_end;
        }

        if (!xj_ok(&e)) {
            m->exec_count = 0;
            if (!e.overflow) m->access |= ACC_JIT_UNSUPPORTED;
            return false;
        }
    }

compile_end:
#undef EMIT_BRANCH

    s_pc_to_jit[len] = (int32_t)e.pos;

    for (int i = 0; i < n_fixups; i++) {
        uint32_t target_pc = s_fixups[i].target_pc;
        if (target_pc > MAX_CODE_SIZE || s_pc_to_jit[target_pc] < 0) {
            if (t && t->exception) t->exception = NULL;
            m->access |= ACC_JIT_UNSUPPORTED;
            m->exec_count = 0;
            return false;
        }
        int target_pos = s_pc_to_jit[target_pc];
        int j_pos = s_fixups[i].patch_pos;
        int disp = target_pos - (j_pos + 4);
        if (disp < -131072 || disp > 131071) {
            if (t && t->exception) t->exception = NULL;
            m->access |= ACC_JIT_UNSUPPORTED;
            m->exec_count = 0;
            return false;
        }
        patch_jump(e.buf, j_pos, target_pos);
    }

    for (int i = 0; i < s_n_resumes; i++) {
        uint32_t target_pc = s_resumes[i].pc;
        if (target_pc > MAX_CODE_SIZE || s_pc_to_jit[target_pc] < 0) {
            if (t && t->exception) t->exception = NULL;
            m->access |= ACC_JIT_UNSUPPORTED;
            m->exec_count = 0;
            return false;
        }
        patch_jump(e.buf, s_resumes[i].patch_pos, s_pc_to_jit[target_pc]);
    }

    while (e.pos % 4 != 0) xj_byte(&e, 0x00);

    if (!xj_ok(&e)) {
        if (t && t->exception) t->exception = NULL;
        if (!e.overflow) m->access |= ACC_JIT_UNSUPPORTED;
        m->exec_count = 0;
        return false;
    }

    if (xj_exec_sync(&s_jit_mem, s_jit_cache_used, e.pos) != 0) {
        if (t && t->exception) t->exception = NULL;
        m->jit_code = NULL;
        m->jit_size = 0;
        m->access |= ACC_JIT_UNSUPPORTED;
        m->exec_count = 0;
        return false;
    }

    m->jit_code = (void *)(uintptr_t)(s_jit_mem.exec + s_jit_cache_used);
    m->jit_size = e.pos;
    m->jit_fails = 0;
    /* Enable OSR on all compiled methods since every back-edge is registered as a resume point */
    m->access |= ACC_JIT_OSR;

    s_jit_cache_used = (s_jit_cache_used + e.pos + 15) & ~15u;
    s_methods_compiled++;
    s_jit_bytes_emitted += e.pos;
    return true;
}

int dynarec_run_method(Method *m, Thread *t, Frame *f)
{
    if (!m->jit_code) return JIT_DEOPT;

    s_jit_invocations++;
    s_jit_depth++;

    JitFunc fn = (JitFunc)m->jit_code;
    int res = fn(t, f);

    s_jit_depth--;

    if (res == JIT_OK) {
        if (f->lock) {
            monitor_exit(t, f->lock);
            f->lock = NULL;
        }
        int n = m->ret_slots;
        Slot *dst = f->locals;
        Slot *old_sp = f->sp;
        for (Slot *s = dst + n; s < old_sp; s++)
            s->raw = 0;
        int skip = f->ret_skip;
        if (--t->depth == 0) {
            t->state = TS_TERMINATED;
            return JIT_OK;
        }
        f = &t->frames[t->depth - 1];
        f->sp = dst + n;
        f->pc += skip;
        return JIT_OK;
    }

    if (res == JIT_YIELD_CALL) return JIT_YIELD_CALL;
    if (res == JIT_DEOPT) {
        s_jit_deopts++;
        return JIT_DEOPT;
    }

    s_jit_deopts++;
    m->jit_code = NULL;
    return res;
}

void dynarec_dump_stats(void)
{
    nj_log("[dynarec] Stats: %u/%u methods compiled (%u rejected), %u KB cache used (%u%%), %u calls, %u deopts",
           (unsigned)s_methods_compiled,
           (unsigned)s_jit_compile_attempts,
           (unsigned)s_jit_compile_rejects,
           (unsigned)(s_jit_cache_used / 1024),
           (unsigned)(s_jit_cache_used * 100 / (s_jit_mem.size ? s_jit_mem.size : 1)),
           (unsigned)s_jit_invocations,
           (unsigned)s_jit_deopts);
}
