/* Streaming raw-deflate decoder on top of tinfl (ESP32-S3 ROM miniz). Same technique as
 * miniz's own mz_inflate(): 32 KB circular dictionary, output copied out of it on demand. */
#include <stdlib.h>
#include <string.h>
#include "miniz.h"
#include "rg_inflate.h"

struct rg_inflate {
    tinfl_decompressor dec;
    uint8_t *dict;      /* TINFL_LZ_DICT_SIZE bytes */
    size_t dict_ofs;    /* where undelivered output starts / next output is written */
    size_t dict_avail;  /* decompressed bytes in dict not yet delivered */
    int done;           /* tinfl reported the end of the deflate stream */
};

rg_inflate_t *rg_inflate_new(void)
{
    rg_inflate_t *s = (rg_inflate_t *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
    if (!s->dict)
    {
        free(s);
        return NULL;
    }
    tinfl_init(&s->dec);
    return s;
}

void rg_inflate_free(rg_inflate_t *s)
{
    if (!s)
        return;
    free(s->dict);
    free(s);
}

int rg_inflate_run(rg_inflate_t *s, const uint8_t **next_in, size_t *avail_in, int more_input,
                   uint8_t **next_out, size_t *avail_out)
{
    while (*avail_out)
    {
        if (s->dict_avail)
        {
            size_t n = s->dict_avail < *avail_out ? s->dict_avail : *avail_out;
            memcpy(*next_out, s->dict + s->dict_ofs, n);
            *next_out += n;
            *avail_out -= n;
            s->dict_avail -= n;
            s->dict_ofs = (s->dict_ofs + n) & (TINFL_LZ_DICT_SIZE - 1);
            continue;
        }
        if (s->done)
            return RG_INFLATE_DONE;

        size_t in_bytes = *avail_in;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - s->dict_ofs;
        tinfl_status st = tinfl_decompress(&s->dec, *next_in, &in_bytes, s->dict, s->dict + s->dict_ofs,
                                           &out_bytes, more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        *next_in += in_bytes;
        *avail_in -= in_bytes;
        s->dict_avail = out_bytes;

        if (st == TINFL_STATUS_DONE)
            s->done = 1;
        else if ((int)st < 0)
            return RG_INFLATE_ERROR;
        else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && out_bytes == 0)
            return RG_INFLATE_NEED_INPUT;
    }
    return (s->done && !s->dict_avail) ? RG_INFLATE_DONE : RG_INFLATE_OK;
}
