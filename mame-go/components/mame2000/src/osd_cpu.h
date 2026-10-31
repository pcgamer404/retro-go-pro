/*******************************************************************************
*                                                                              *
*  osd_cpu.h - integer types and PAIR union                                    *
*                                                                              *
*  Integer types are <stdint.h> directly.  The legacy UINT8/INT8 .. UINT64/    *
*  INT64 aliases have been removed; use uint8_t/int8_t .. uint64_t/int64_t.    *
*                                                                              *
*  Endianness model:                                                           *
*    - MSB_FIRST defined  -> host is big-endian                                *
*    - MSB_FIRST undefined -> host is little-endian                            *
*  There are no runtime endianness checks anywhere in this codebase.           *
*                                                                              *
*******************************************************************************/

#ifndef OSD_CPU_H
#define OSD_CPU_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 64-bit helpers - kept for the cpu cores that use them.
 * ------------------------------------------------------------------------- */
#define COMBINE_64_32_32(A,B)     ((((uint64_t)(A))<<32) | (uint32_t)(B))
#define COMBINE_U64_U32_U32(A,B)  COMBINE_64_32_32(A,B)

#define HI32_32_64(A)             (((uint64_t)(A)) >> 32)
#define HI32_U32_U64(A)           HI32_32_64(A)

#define LO32_32_64(A)             ((A) & UINT32_C(0xffffffff))
#define LO32_U32_U64(A)           LO32_32_64(A)

#define DIV_64_64_32(A,B)         ((A)/(B))
#define DIV_U64_U64_U32(A,B)      ((A)/(uint32_t)(B))

#define MOD_32_64_32(A,B)         ((A)%(B))
#define MOD_U32_U64_U32(A,B)      ((A)%(uint32_t)(B))

#define MUL_64_32_32(A,B)         ((A)*(int64_t)(B))
#define MUL_U64_U32_U32(A,B)      ((A)*(uint64_t)(uint32_t)(B))

/* ---------------------------------------------------------------------------
 * Endian-aware union used by every emulated CPU core to alias 8/16/32-bit
 * register pieces.  Layout is locked to host endianness via MSB_FIRST.
 * ------------------------------------------------------------------------- */
typedef union {
#ifdef MSB_FIRST
    struct { uint8_t  h3, h2, h, l; } b;
    struct { uint16_t h,  l;        } w;
#else
    struct { uint8_t  l,  h,  h2, h3; } b;
    struct { uint16_t l,  h;          } w;
#endif
    uint32_t d;
} PAIR;

/* Sanity checks.  If any of these fire someone has broken the host build. */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(PAIR)     == 4, "PAIR must be exactly 4 bytes");
_Static_assert(sizeof(uint8_t)  == 1, "uint8_t must be 1 byte");
_Static_assert(sizeof(uint16_t) == 2, "uint16_t must be 2 bytes");
_Static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");
_Static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
#endif

#endif /* OSD_CPU_H */
