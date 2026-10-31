/* Streaming raw-deflate decoder for the ZIP reader, built on the ESP32 ROM miniz (tinfl).
 * Kept behind this tiny C interface so miniz.h never meets unzip.cpp's own zlib typedefs. */
#ifndef RG_INFLATE_H
#define RG_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RG_INFLATE_OK          0   /* made progress / output buffer is full */
#define RG_INFLATE_DONE        1   /* stream finished and every byte was delivered */
#define RG_INFLATE_NEED_INPUT  2   /* consumed all input, caller must supply more */
#define RG_INFLATE_ERROR      (-1) /* corrupt data or out of memory */

typedef struct rg_inflate rg_inflate_t;

rg_inflate_t *rg_inflate_new(void);
void rg_inflate_free(rg_inflate_t *s);

/* Decompress raw deflate data (no zlib header, as stored in ZIP members).
 * Advances the next_in/avail_in and next_out/avail_out pairs (passed by pointer).
 * more_input != 0 means further compressed bytes exist beyond the ones passed in now. */
int rg_inflate_run(rg_inflate_t *s, const uint8_t **next_in, size_t *avail_in, int more_input,
                   uint8_t **next_out, size_t *avail_out);

#ifdef __cplusplus
}
#endif
#endif
