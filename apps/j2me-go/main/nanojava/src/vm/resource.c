/*
 * resource.c - where classes and resources come from.
 *
 * Classes are looked up in the system class library first and then in the
 * application (jar or directory). Resources (getResourceAsStream) only come
 * from the application.
 */
#include "../pal/alloc.h"
#include "vm.h"
#include "../util/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_APP 4

static Zip        *system_zip;
static Zip        *app_zips[MAX_APP];
static int         n_app_zips;
static const char *app_dirs[MAX_APP];
static int         n_app_dirs;

static uint8_t *read_file(const char *dir, const char *path, uint32_t *len)
{
    char full[512];
    snprintf(full, sizeof full, "%s/%s", dir, path[0] == '/' ? path + 1 : path);
    FILE *f = fopen(full, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = size >= 0 ? nj_pal_malloc((size_t)size + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        nj_pal_free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[size] = 0;
        *len = (uint32_t)size;
    }
    return buf;
}

static uint8_t *system_source(const char *path, uint32_t *len)
{
    return system_zip ? zip_read_name(system_zip, path, len) : NULL;
}

/* J2ME titles are written against a case-sensitive device filesystem, but
 * repacked jars are often built on Windows and some hand-rolled packers store
 * backslashes or "./" prefixes.  Normalise a requested resource path once so
 * "res\\spl.png", "/res//spl.png" and "./res/spl.png" all find "res/spl.png". */
static const char *normalize_path(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (*in == '/' || *in == '\\')
        in++;
    while (*in && n + 1 < cap) {
        char c = *in++;
        if (c == '\\')
            c = '/';
        if (c == '/' && n > 0 && out[n - 1] == '/')
            continue;                       /* collapse "//" */
        if (c == '.' && (in[0] == '/' || in[0] == '\\' || in[0] == 0)) {
            if (in[0])
                in++;                       /* skip "./" */
            continue;
        }
        out[n++] = c;
    }
    out[n] = 0;
    return out;
}

static bool ascii_ieq(const char *a, const char *b)
{
    for (;;) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

/* Case-insensitive name index, built lazily the first time an exact lookup
 * misses.  Titles that probe for optional files (localisation, per-device
 * variants) used to pay a full scan of the archive, with a string compare per
 * entry, for every miss - thousands of times on a loading screen.  With the
 * index a miss costs one hash and a short probe. */
typedef struct { uint32_t hash; ZipEntry *e; } CiSlot;
static CiSlot   *ci_tab[MAX_APP];
static uint32_t  ci_cap[MAX_APP];
static bool      ci_built[MAX_APP];

static uint32_t ci_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static void ci_build(int i)
{
    ci_built[i] = true;
    Zip     *z = app_zips[i];
    uint32_t n = zip_count(z);
    uint32_t cap = 16;
    while (cap < n * 2)
        cap <<= 1;
    CiSlot *t = nj_pal_calloc(cap, sizeof *t);
    if (!t)
        return;                  /* lookups fall back to the linear scan */
    for (uint32_t k = 0; k < n; k++) {
        ZipEntry *e = zip_entry(z, k);
        if (!e || !e->name)
            continue;
        uint32_t h = ci_hash(e->name);
        uint32_t idx = h & (cap - 1);
        while (t[idx].e)
            idx = (idx + 1) & (cap - 1);
        t[idx].hash = h;
        t[idx].e = e;
    }
    ci_tab[i] = t;
    ci_cap[i] = cap;
}

static ZipEntry *ci_find(int i, const char *norm)
{
    if (!ci_built[i])
        ci_build(i);
    CiSlot *t = ci_tab[i];
    if (!t) {
        Zip *z = app_zips[i];
        uint32_t n = zip_count(z);
        for (uint32_t k = 0; k < n; k++) {
            ZipEntry *e = zip_entry(z, k);
            if (e && ascii_ieq(e->name, norm))
                return e;
        }
        return NULL;
    }
    uint32_t h = ci_hash(norm);
    uint32_t idx = h & (ci_cap[i] - 1);
    while (t[idx].e) {
        if (t[idx].hash == h && ascii_ieq(t[idx].e->name, norm))
            return t[idx].e;
        idx = (idx + 1) & (ci_cap[i] - 1);
    }
    return NULL;
}

static void ci_reset(void)
{
    for (int i = 0; i < MAX_APP; i++) {
        nj_pal_free(ci_tab[i]);
        ci_tab[i] = NULL;
        ci_cap[i] = 0;
        ci_built[i] = false;
    }
}

/* Locate an application resource.  On success *len is its uncompressed size
 * and canon (when non-NULL) receives the exact archive name, which may differ
 * from the request only by case.  The second pass is deliberately limited to
 * lookups that already failed: a title probing for optional files cannot make
 * the common (found) path pay for it. */
bool res_lookup_app(const char *path, uint32_t *len, char *canon, size_t canon_cap)
{
    char norm[256];
    normalize_path(path, norm, sizeof norm);

    for (int i = 0; i < n_app_zips; i++) {
        ZipEntry *e = zip_find(app_zips[i], norm);
        if (e) {
            *len = e->usize;
            if (canon)
                snprintf(canon, canon_cap, "%s", e->name);
            return true;
        }
    }
    for (int i = 0; i < n_app_dirs; i++) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", app_dirs[i], norm);
        FILE *f = fopen(full, "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size >= 0) {
            *len = (uint32_t)size;
            if (canon)
                snprintf(canon, canon_cap, "%s", norm);
            return true;
        }
    }

    for (int i = 0; i < n_app_zips; i++) {
        ZipEntry *e = ci_find(i, norm);
        if (!e)
            continue;
        *len = e->usize;
        if (canon)
            snprintf(canon, canon_cap, "%s", e->name);
        return true;
    }
    return false;
}

/* Read a resource straight into a caller buffer (e.g. a Java byte[]), avoiding
 * the malloc+memcpy round trip that used to double peak memory during loads. */
int32_t res_read_app_into(const char *path, uint8_t *dst, uint32_t cap)
{
    char norm[256];
    normalize_path(path, norm, sizeof norm);

    for (int i = 0; i < n_app_zips; i++) {
        ZipEntry *e = zip_find(app_zips[i], norm);
        if (e)
            return zip_read_into(app_zips[i], e, dst, cap);
    }
    for (int i = 0; i < n_app_dirs; i++) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", app_dirs[i], norm);
        FILE *f = fopen(full, "rb");
        if (!f)
            continue;
        size_t got = fread(dst, 1, cap, f);
        fclose(f);
        return (int32_t)got;
    }
    return -1;
}

uint8_t *res_read_app(const char *path, uint32_t *len)
{
    char norm[256];
    normalize_path(path, norm, sizeof norm);

    for (int i = 0; i < n_app_zips; i++) {
        uint8_t *d = zip_read_name(app_zips[i], norm, len);
        if (d)
            return d;
    }
    for (int i = 0; i < n_app_dirs; i++) {
        uint8_t *d = read_file(app_dirs[i], norm, len);
        if (d)
            return d;
    }
    return NULL;
}

static uint8_t *jad_data;
static uint32_t jad_len;

void res_set_jad(const uint8_t *data, uint32_t len)
{
    nj_pal_free(jad_data);
    jad_data = nj_pal_malloc(len + 1);
    memcpy(jad_data, data, len);
    jad_len = len;
}

const uint8_t *res_jad(uint32_t *len)
{
    *len = jad_len;
    return jad_data;
}

static bool sources_added;

static void add_sources(void)
{
    if (!sources_added) {
        class_add_source(system_source);
        class_add_source(res_read_app);
        sources_added = true;
    }
}

void res_reset(void)
{
    zip_close(system_zip);
    for (int i = 0; i < n_app_zips; i++)
        zip_close(app_zips[i]);
    system_zip = NULL;
    n_app_zips = n_app_dirs = 0;
    ci_reset();
    nj_pal_free(jad_data);
    jad_data = NULL;
    jad_len = 0;
    sources_added = false;
}

void res_set_system(Zip *z)
{
    system_zip = z;
    add_sources();
}

void res_add_app_zip(Zip *z)
{
    if (n_app_zips < MAX_APP) {
        ci_built[n_app_zips] = false;
        app_zips[n_app_zips++] = z;
    }
    add_sources();
}

void res_add_app_dir(const char *dir)
{
    if (n_app_dirs < MAX_APP)
        app_dirs[n_app_dirs++] = dir;
    add_sources();
}
