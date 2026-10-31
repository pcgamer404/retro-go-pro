#pragma once

/*
 * C-linkage bridge for Retro-Go's C APIs.
 *
 * IMPORTANT:
 * lodepng.h is deliberately NOT included here.  It is a mixed C/C++
 * header and contains C++ templates/namespaces.  Force-including it inside
 * extern "C" makes the C++ standard library itself inherit C linkage and
 * produces "template with C linkage" errors.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* rg_storage.h expects RG_PATH_MAX from Retro-Go's global config header.
 * This compatibility header is force-included before that header, so provide
 * the same fixed-size fallback used by the storage API when it is absent. */
#ifndef RG_PATH_MAX
#define RG_PATH_MAX 255
#endif

#include <rg_settings.h>
#include <rg_audio.h>
#include <rg_storage.h>
#include <rg_input.h>

/*
 * Oregon Trail's C++ frontend only calls this C API from lodepng.
 * Declare just that function here with C linkage; lodepng.c provides it.
 * Do not include lodepng.h from this force-included compatibility header.
 */
#include <stddef.h>
unsigned lodepng_decode32(unsigned char** out,
                          unsigned* w,
                          unsigned* h,
                          const unsigned char* in,
                          size_t insize);

#ifdef __cplusplus
}
#endif
