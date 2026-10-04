/*
 * endian.h - one source of truth for byte-order conversions.
 *
 * Endianness model (project-wide):
 *   - MSB_FIRST   defined  -> host is big-endian
 *   - MSB_FIRST undefined  -> host is little-endian
 *
 * There are no runtime endianness checks anywhere in this codebase, and
 * the host endianness is always known at compile time via -DMSB_FIRST in
 * the per-platform Makefile entry.
 *
 * All byte-order conversion (zip headers, WAV file fields, PNG network-
 * order ints, etc.) goes through the helpers in this header.  Do not add
 * new copies of intelLong / read_word / convert_from_network_order
 * anywhere else in the tree.
 *
 *   le32toh(x), le16toh(x)   little-endian on-disk value -> host value
 *   be32toh(x), be16toh(x)   big-endian    on-disk value -> host value
 *   htole32(x), htole16(x)   host value -> little-endian on-disk
 *   htobe32(x), htobe16(x)   host value -> big-endian    on-disk
 *
 *   read_le16(p), read_le32(p)   read N bytes at p as a little-endian int
 *   read_be16(p), read_be32(p)   read N bytes at p as a big-endian int
 *
 * Naming follows the BSD/glibc <endian.h> convention, which is the closest
 * thing to a portable standard for these names.  We provide our own
 * implementations so that we don't depend on platform headers that may or
 * may not exist on every libretro target (Vita, NGC, WiiU, 3DS, ...).
 */
#ifndef MAME2000_ENDIAN_H
#define MAME2000_ENDIAN_H

#include <stdint.h>

/* ---------- byte swaps ----------------------------------------------- */

static __inline uint16_t mame_bswap16(uint16_t v)
{
#if defined(__GNUC__) && (__GNUC__ >= 5 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
    return __builtin_bswap16(v);
#else
    return (uint16_t)((v << 8) | (v >> 8));
#endif
}

static __inline uint32_t mame_bswap32(uint32_t v)
{
#if defined(__GNUC__) && (__GNUC__ >= 4 && __GNUC_MINOR__ >= 3)
    return __builtin_bswap32(v);
#else
    return ((v << 24)
          | ((v & UINT32_C(0x0000ff00)) <<  8)
          | ((v & UINT32_C(0x00ff0000)) >>  8)
          | (v >> 24));
#endif
}

/* ---------- host <-> LE/BE ------------------------------------------- */

#ifdef MSB_FIRST
  /* host is big-endian */
  #define le16toh(x)  mame_bswap16((uint16_t)(x))
  #define le32toh(x)  mame_bswap32((uint32_t)(x))
  #define htole16(x)  mame_bswap16((uint16_t)(x))
  #define htole32(x)  mame_bswap32((uint32_t)(x))
  #define be16toh(x)  ((uint16_t)(x))
  #define be32toh(x)  ((uint32_t)(x))
  #define htobe16(x)  ((uint16_t)(x))
  #define htobe32(x)  ((uint32_t)(x))
#else
  /* host is little-endian */
  #define le16toh(x)  ((uint16_t)(x))
  #define le32toh(x)  ((uint32_t)(x))
  #define htole16(x)  ((uint16_t)(x))
  #define htole32(x)  ((uint32_t)(x))
  #define be16toh(x)  mame_bswap16((uint16_t)(x))
  #define be32toh(x)  mame_bswap32((uint32_t)(x))
  #define htobe16(x)  mame_bswap16((uint16_t)(x))
  #define htobe32(x)  mame_bswap32((uint32_t)(x))
#endif

/* ---------- unaligned-safe reads ------------------------------------- *
 * These do byte-by-byte loads so they're safe even on strict-alignment
 * targets (ARMv5, MIPS without unaligned support, etc.).  Use these
 * everywhere you're reading a binary file header into a local int.
 */

static __inline uint16_t read_le16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static __inline uint32_t read_le32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0]
          | ((uint32_t)b[1] <<  8)
          | ((uint32_t)b[2] << 16)
          | ((uint32_t)b[3] << 24));
}

static __inline uint16_t read_be16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

static __inline uint32_t read_be32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (((uint32_t)b[0] << 24)
          | ((uint32_t)b[1] << 16)
          | ((uint32_t)b[2] <<  8)
          |  (uint32_t)b[3]);
}

#endif /* MAME2000_ENDIAN_H */
