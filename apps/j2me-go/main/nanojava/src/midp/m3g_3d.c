/*
 * m3g_3d.c - Full Z-Buffered Software 3D Rasterizer, Binary .m3g Scene Loader,
 *            Keyframe Animator, & MascotCapsule Micro3D v3 Engine for ESP32-S3.
 */
#include "m3g_3d.h"
#include "../pal/alloc.h"
#include "../pal/pal.h"
#include "../vm/vm.h"
#include "../util/inflate.h"
#include "../util/png.h"
#include "../util/bmp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

#define A_REF(n)   (args[(n)].ref)
#define A_INT(n)   (args[(n)].i)
#define A_FLT(n)   (args[(n)].f)

/* ------------------------------------------------------------------------ */
/* Core 3D State & Z-Buffer                                                 */
/* ------------------------------------------------------------------------ */
static uint16_t       *s_zbuf = NULL;
static int             s_zw = 0, s_zh = 0;
static Surface         s_target;
static Clip            s_clip;
static bool            s_depth_enabled = true;
static int             s_vp_x = 0, s_vp_y = 0, s_vp_w = 240, s_vp_h = 320;
static M3GMatrix       s_proj, s_mv, s_mvp;
static const uint16_t *s_tex_px = NULL;
static const uint8_t  *s_tex_al = NULL;
static int             s_tex_w = 0, s_tex_h = 0;

static void mat_identity(M3GMatrix *m)
{
    memset(m->m, 0, sizeof(m->m));
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
}

static void mat_mul(M3GMatrix *out, const M3GMatrix *a, const M3GMatrix *b)
{
    M3GMatrix r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r.m[col * 4 + row] =
                a->m[0 * 4 + row] * b->m[col * 4 + 0] +
                a->m[1 * 4 + row] * b->m[col * 4 + 1] +
                a->m[2 * 4 + row] * b->m[col * 4 + 2] +
                a->m[3 * 4 + row] * b->m[col * 4 + 3];
        }
    }
    *out = r;
}

static void mat_rotate_axis(M3GMatrix *out, float angle_deg, float ax, float ay, float az)
{
    mat_identity(out);
    float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len < 1e-6f) return;
    ax /= len; ay /= len; az /= len;
    float rad = angle_deg * (3.141592653589793f / 180.0f);
    float s = sinf(rad), c = cosf(rad), ic = 1.0f - c;
    out->m[0]  = ax * ax * ic + c;
    out->m[1]  = ay * ax * ic + az * s;
    out->m[2]  = az * ax * ic - ay * s;
    out->m[4]  = ax * ay * ic - az * s;
    out->m[5]  = ay * ay * ic + c;
    out->m[6]  = az * ay * ic + ax * s;
    out->m[8]  = ax * az * ic + ay * s;
    out->m[9]  = ay * az * ic - ax * s;
    out->m[10] = az * az * ic + c;
}

static bool mat_invert(M3GMatrix *out, const M3GMatrix *in)
{
    const float *m = in->m;
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-12f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out->m[i] = inv[i] * det;
    return true;
}

static void bind_graphics_target(Object *g, bool depth_en)
{
    if (!g || !g->cls) return;
    Field *f_tgt = class_find_field(g->cls, "target", "Ljavax/microedition/lcdui/Image;");
    Field *f_cx1 = class_find_field(g->cls, "cx1", "I");
    Field *f_cy1 = class_find_field(g->cls, "cy1", "I");
    Field *f_cx2 = class_find_field(g->cls, "cx2", "I");
    Field *f_cy2 = class_find_field(g->cls, "cy2", "I");
    if (!f_tgt || !f_cx1) return;
    Object *img = GET_REF(g, f_tgt);
    if (!img || !img->cls) return;
    Field *f_px = class_find_field(img->cls, "pixels", "[S");
    Field *f_w  = class_find_field(img->cls, "width", "I");
    Field *f_h  = class_find_field(img->cls, "height", "I");
    if (!f_px || !f_w || !f_h) return;
    Object *px = GET_REF(img, f_px);
    if (!px) return;

    s_target.px    = ARRAY_DATA(px, uint16_t);
    s_target.alpha = NULL;
    s_target.w     = GET_INT(img, f_w);
    s_target.h     = GET_INT(img, f_h);
    s_clip.x1      = GET_INT(g, f_cx1);
    s_clip.y1      = GET_INT(g, f_cy1);
    s_clip.x2      = GET_INT(g, f_cx2);
    s_clip.y2      = GET_INT(g, f_cy2);
    s_depth_enabled = depth_en;

    if (depth_en && (s_zw != s_target.w || s_zh != s_target.h)) {
        if (s_zbuf) nj_pal_free(s_zbuf);
        s_zbuf = (uint16_t *)nj_pal_malloc((size_t)s_target.w * (size_t)s_target.h * sizeof(uint16_t));
        s_zw = s_target.w;
        s_zh = s_target.h;
        if (s_zbuf) memset(s_zbuf, 0xFF, (size_t)s_zw * (size_t)s_zh * sizeof(uint16_t));
    }
}

/* ------------------------------------------------------------------------ */
/* Fast IRAM Scanline Triangle Rasterizer (Z-Buffered + Textured + Lit)     */
/* ------------------------------------------------------------------------ */
static IRAM_ATTR void rasterize_tri(const M3GVertex *v0, const M3GVertex *v1, const M3GVertex *v2, int blend_mode)
{
    if (!s_target.px) return;

    const M3GVertex *a = v0, *b = v1, *c = v2, *tmp;
    if (a->y > b->y) { tmp = a; a = b; b = tmp; }
    if (b->y > c->y) { tmp = b; b = c; c = tmp; }
    if (a->y > b->y) { tmp = a; a = b; b = tmp; }

    int y0 = a->y >> 16, y1 = b->y >> 16, y2 = c->y >> 16;
    if (y2 < s_clip.y1 || y0 >= s_clip.y2 || y0 == y2) return;

    int dy02 = y2 - y0;
    int32_t dx02 = (c->x - a->x) / dy02;
    int32_t dz02 = (c->z - a->z) / dy02;
    int32_t du02 = (c->u - a->u) / dy02;
    int32_t dv02 = (c->v - a->v) / dy02;

    uint16_t flat_col = gfx_pack(a->color);
    uint32_t alpha_val = (a->color >> 24) & 0xFF;
    bool has_tex = (s_tex_px != NULL && s_tex_w > 0 && s_tex_h > 0);
    int tw_mask = s_tex_w - 1, th_mask = s_tex_h - 1;
    bool pow2 = has_tex && ((s_tex_w & tw_mask) == 0) && ((s_tex_h & th_mask) == 0);

    for (int half = 0; half < 2; half++) {
        int ys = (half == 0) ? y0 : y1;
        int ye = (half == 0) ? y1 : y2;
        if (ys >= ye) continue;

        const M3GVertex *s0 = (half == 0) ? a : b;
        const M3GVertex *s1 = (half == 0) ? b : c;
        int dy = ye - ys;

        int32_t xl = a->x + dx02 * (ys - y0);
        int32_t zl = a->z + dz02 * (ys - y0);
        int32_t ul = a->u + du02 * (ys - y0);
        int32_t vl = a->v + dv02 * (ys - y0);

        int32_t xr = s0->x, zr = s0->z, ur = s0->u, vr = s0->v;
        int32_t dxr = (s1->x - s0->x) / dy;
        int32_t dzr = (s1->z - s0->z) / dy;
        int32_t dur = (s1->u - s0->u) / dy;
        int32_t dvr = (s1->v - s0->v) / dy;

        for (int y = ys; y < ye; y++) {
            if (y >= s_clip.y1 && y < s_clip.y2) {
                int32_t x_a = xl, x_b = xr, z_a = zl, z_b = zr, u_a = ul, u_b = ur, v_a = vl, v_b = vr;
                if (x_a > x_b) {
                    int32_t t;
                    t = x_a; x_a = x_b; x_b = t;
                    t = z_a; z_a = z_b; z_b = t;
                    t = u_a; u_a = u_b; u_b = t;
                    t = v_a; v_a = v_b; v_b = t;
                }
                int xs = x_a >> 16;
                int xe = x_b >> 16;
                int span = xe - xs;
                if (span > 0) {
                    int32_t dz = (z_b - z_a) / span;
                    int32_t du = (u_b - u_a) / span;
                    int32_t dv = (v_b - v_a) / span;
                    if (xs < s_clip.x1) {
                        int skip = s_clip.x1 - xs;
                        z_a += dz * skip; u_a += du * skip; v_a += dv * skip;
                        xs = s_clip.x1;
                    }
                    if (xe > s_clip.x2) xe = s_clip.x2;

                    uint16_t *dst = s_target.px + y * s_target.w + xs;
                    uint16_t *zrow = (s_depth_enabled && s_zbuf) ? (s_zbuf + y * s_zw + xs) : NULL;

                    for (int x = xs; x < xe; x++, dst++) {
                        uint16_t z16 = (uint16_t)(z_a >> 16);
                        if (zrow) {
                            if (z16 >= *zrow) {
                                z_a += dz; u_a += du; v_a += dv; zrow++;
                                continue;
                            }
                            if (blend_mode == 68 /* REPLACE */) {
                                *zrow = z16;
                            }
                            zrow++;
                        }
                        if (has_tex) {
                            int tx = pow2 ? ((u_a >> 16) & tw_mask) : (((u_a >> 16) % s_tex_w + s_tex_w) % s_tex_w);
                            int ty = pow2 ? ((v_a >> 16) & th_mask) : (((v_a >> 16) % s_tex_h + s_tex_h) % s_tex_h);
                            int tidx = ty * s_tex_w + tx;
                            uint8_t ta = s_tex_al ? s_tex_al[tidx] : 255;
                            if (ta >= 16) {
                                uint16_t src_px = s_tex_px[tidx];
                                if (blend_mode == 65 /* ALPHA_ADD */) {
                                    uint16_t d = *dst;
                                    unsigned r = (d & 31) + (src_px & 31);
                                    unsigned g = ((d >> 5) & 31) + ((src_px >> 5) & 31);
                                    unsigned b_c = ((d >> 10) & 31) + ((src_px >> 10) & 31);
                                    if (r > 31) r = 31;
                                    if (g > 31) g = 31;
                                    if (b_c > 31) b_c = 31;
                                    *dst = (uint16_t)(0x8000 | r | (g << 5) | (b_c << 10));
                                } else if (ta < 245 || blend_mode == 64 /* ALPHA */) {
                                    *dst = gfx_blend(*dst, src_px, ta);
                                } else {
                                    *dst = src_px;
                                }
                            }
                        } else {
                            if (blend_mode == 64 && alpha_val < 250) {
                                *dst = gfx_blend(*dst, flat_col, alpha_val);
                            } else {
                                *dst = flat_col;
                            }
                        }
                        z_a += dz; u_a += du; v_a += dv;
                    }
                }
            }
            xl += dx02; zl += dz02; ul += du02; vl += dv02;
            xr += dxr;  zr += dzr;  ur += dur;  vr += dvr;
        }
    }
}

static void submit_triangle_list(const void *pos_ptr, int pos_type, int pos_comps, float pos_scale, const float pos_bias[3],
                                 const void *tex_ptr, int tex_type, float tex_scale, const float tex_bias[2],
                                 const void *col_ptr, int col_comps, uint32_t default_col,
                                 const uint16_t *indices, int index_count,
                                 int winding, int culling, int blend_mode)
{
    if (!pos_ptr || !indices || index_count < 3) return;
    float half_w = s_vp_w * 0.5f;
    float half_h = s_vp_h * 0.5f;
    float cx = s_vp_x + half_w;
    float cy = s_vp_y + half_h;

    for (int i = 0; i + 2 < index_count; i += 3) {
        M3GVertex v[3];
        bool behind = false;
        for (int k = 0; k < 3; k++) {
            uint16_t idx = indices[i + k];
            float vx, vy, vz;
            if (pos_type == 1) {
                const int8_t *p = (const int8_t *)pos_ptr + idx * pos_comps;
                vx = p[0] * pos_scale + pos_bias[0];
                vy = p[1] * pos_scale + pos_bias[1];
                vz = (pos_comps >= 3 ? p[2] : 0) * pos_scale + pos_bias[2];
            } else {
                const int16_t *p = (const int16_t *)pos_ptr + idx * pos_comps;
                vx = p[0] * pos_scale + pos_bias[0];
                vy = p[1] * pos_scale + pos_bias[1];
                vz = (pos_comps >= 3 ? p[2] : 0) * pos_scale + pos_bias[2];
            }

            float clip_x = s_mvp.m[0]*vx + s_mvp.m[4]*vy + s_mvp.m[8]*vz  + s_mvp.m[12];
            float clip_y = s_mvp.m[1]*vx + s_mvp.m[5]*vy + s_mvp.m[9]*vz  + s_mvp.m[13];
            float clip_z = s_mvp.m[2]*vx + s_mvp.m[6]*vy + s_mvp.m[10]*vz + s_mvp.m[14];
            float clip_w = s_mvp.m[3]*vx + s_mvp.m[7]*vy + s_mvp.m[11]*vz + s_mvp.m[15];

            if (clip_w <= 0.001f) { behind = true; break; }
            float inv_w = 1.0f / clip_w;
            float ndc_x = clip_x * inv_w;
            float ndc_y = clip_y * inv_w;
            float ndc_z = (clip_z * inv_w + 1.0f) * 0.5f;
            if (ndc_z < 0.0f) ndc_z = 0.0f;
            if (ndc_z > 1.0f) ndc_z = 1.0f;

            v[k].x = (int32_t)((cx + ndc_x * half_w) * 65536.0f);
            v[k].y = (int32_t)((cy - ndc_y * half_h) * 65536.0f);
            v[k].z = (int32_t)(ndc_z * 65535.0f * 65536.0f);

            if (tex_ptr && s_tex_w > 0 && s_tex_h > 0) {
                float tu, tv;
                if (tex_type == 1) {
                    const int8_t *tc = (const int8_t *)tex_ptr + idx * 2;
                    tu = tc[0] * tex_scale + tex_bias[0];
                    tv = tc[1] * tex_scale + tex_bias[1];
                } else {
                    const int16_t *tc = (const int16_t *)tex_ptr + idx * 2;
                    tu = tc[0] * tex_scale + tex_bias[0];
                    tv = tc[1] * tex_scale + tex_bias[1];
                }
                v[k].u = (int32_t)(tu * s_tex_w * 65536.0f);
                v[k].v = (int32_t)(tv * s_tex_h * 65536.0f);
            } else {
                v[k].u = v[k].v = 0;
            }

            if (col_ptr) {
                const uint8_t *cp = (const uint8_t *)col_ptr + idx * col_comps;
                uint32_t ca = (col_comps >= 4) ? cp[3] : 255;
                v[k].color = (ca << 24) | ((uint32_t)cp[0] << 16) | ((uint32_t)cp[1] << 8) | cp[2];
            } else {
                v[k].color = default_col;
            }
        }
        if (behind) continue;

        int64_t area = (int64_t)(v[1].x - v[0].x) * (v[2].y - v[0].y) -
                       (int64_t)(v[1].y - v[0].y) * (v[2].x - v[0].x);
        if (culling == 160 /* CULL_BACK */) {
            if ((winding == 168 && area >= 0) || (winding == 169 && area <= 0)) continue;
        } else if (culling == 161 /* CULL_FRONT */) {
            if ((winding == 168 && area <= 0) || (winding == 169 && area >= 0)) continue;
        }
        rasterize_tri(&v[0], &v[1], &v[2], blend_mode);
    }
}

/* ------------------------------------------------------------------------ */
/* JSR-184 (M3G) Native Methods & Complete Scene Graph                      */
/* ------------------------------------------------------------------------ */

static void get_transform_matrix(Object *tr_obj, M3GMatrix *out)
{
    mat_identity(out);
    if (!tr_obj || !tr_obj->cls) return;
    Field *f = class_find_field(tr_obj->cls, "matrix", "[F");
    if (!f) return;
    Object *arr = GET_REF(tr_obj, f);
    if (arr && ARRAY_LEN(arr) >= 16) {
        memcpy(out->m, ARRAY_DATA(arr, float), 16 * sizeof(float));
    }
}

static void set_transform_matrix(Object *tr_obj, const M3GMatrix *in)
{
    if (!tr_obj || !tr_obj->cls) return;
    Field *f = class_find_field(tr_obj->cls, "matrix", "[F");
    if (!f) return;
    Object *arr = GET_REF(tr_obj, f);
    if (arr && ARRAY_LEN(arr) >= 16) {
        memcpy(ARRAY_DATA(arr, float), in->m, 16 * sizeof(float));
    }
}

/* Transform natives */
static int M3G_Transform_setIdentity(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    M3GMatrix m;
    mat_identity(&m);
    set_transform_matrix(A_REF(0), &m);
    return NATIVE_OK;
}

static int M3G_Transform_setArray(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    if (ARRAY_LEN(arr) < 16) { vm_throw_new(t, "java/lang/IllegalArgumentException", NULL); return NATIVE_OK; }
    M3GMatrix m;
    memcpy(m.m, ARRAY_DATA(arr, float), 16 * sizeof(float));
    set_transform_matrix(A_REF(0), &m);
    return NATIVE_OK;
}

static int M3G_Transform_setTransform(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    if (!A_REF(1)) { NPE(t); return NATIVE_OK; }
    M3GMatrix m;
    get_transform_matrix(A_REF(1), &m);
    set_transform_matrix(A_REF(0), &m);
    return NATIVE_OK;
}

static int M3G_Transform_get(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    if (ARRAY_LEN(arr) < 16) { vm_throw_new(t, "java/lang/IllegalArgumentException", NULL); return NATIVE_OK; }
    M3GMatrix m;
    get_transform_matrix(A_REF(0), &m);
    memcpy(ARRAY_DATA(arr, float), m.m, 16 * sizeof(float));
    return NATIVE_OK;
}

static int M3G_Transform_invert(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    M3GMatrix m, inv;
    get_transform_matrix(A_REF(0), &m);
    if (!mat_invert(&inv, &m)) {
        vm_throw_new(t, "java/lang/ArithmeticException", "singular matrix");
        return NATIVE_OK;
    }
    set_transform_matrix(A_REF(0), &inv);
    return NATIVE_OK;
}

static int M3G_Transform_transpose(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    M3GMatrix m, tr;
    get_transform_matrix(A_REF(0), &m);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            tr.m[c * 4 + r] = m.m[r * 4 + c];
    set_transform_matrix(A_REF(0), &tr);
    return NATIVE_OK;
}

static int M3G_Transform_postMultiply(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    if (!A_REF(1)) { NPE(t); return NATIVE_OK; }
    M3GMatrix a, b, r;
    get_transform_matrix(A_REF(0), &a);
    get_transform_matrix(A_REF(1), &b);
    mat_mul(&r, &a, &b);
    set_transform_matrix(A_REF(0), &r);
    return NATIVE_OK;
}

static int M3G_Transform_postScale(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    M3GMatrix a, s, r;
    get_transform_matrix(A_REF(0), &a);
    mat_identity(&s);
    s.m[0] = A_FLT(1); s.m[5] = A_FLT(2); s.m[10] = A_FLT(3);
    mat_mul(&r, &a, &s);
    set_transform_matrix(A_REF(0), &r);
    return NATIVE_OK;
}

static int M3G_Transform_postRotate(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    M3GMatrix a, rot, r;
    get_transform_matrix(A_REF(0), &a);
    mat_rotate_axis(&rot, A_FLT(1), A_FLT(2), A_FLT(3), A_FLT(4));
    mat_mul(&r, &a, &rot);
    set_transform_matrix(A_REF(0), &r);
    return NATIVE_OK;
}

static int M3G_Transform_postRotateQuat(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    float qx = A_FLT(1), qy = A_FLT(2), qz = A_FLT(3), qw = A_FLT(4);
    float len = sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
    if (len > 1e-6f) { qx /= len; qy /= len; qz /= len; qw /= len; }
    M3GMatrix a, q, r;
    get_transform_matrix(A_REF(0), &a);
    mat_identity(&q);
    q.m[0] = 1.0f - 2.0f*(qy*qy + qz*qz);
    q.m[1] = 2.0f*(qx*qy + qz*qw);
    q.m[2] = 2.0f*(qx*qz - qy*qw);
    q.m[4] = 2.0f*(qx*qy - qz*qw);
    q.m[5] = 1.0f - 2.0f*(qx*qx + qz*qz);
    q.m[6] = 2.0f*(qy*qz + qx*qw);
    q.m[8] = 2.0f*(qx*qz + qy*qw);
    q.m[9] = 2.0f*(qy*qz - qx*qw);
    q.m[10] = 1.0f - 2.0f*(qx*qx + qy*qy);
    mat_mul(&r, &a, &q);
    set_transform_matrix(A_REF(0), &r);
    return NATIVE_OK;
}

static int M3G_Transform_postTranslate(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    M3GMatrix a, tr, r;
    get_transform_matrix(A_REF(0), &a);
    mat_identity(&tr);
    tr.m[12] = A_FLT(1); tr.m[13] = A_FLT(2); tr.m[14] = A_FLT(3);
    mat_mul(&r, &a, &tr);
    set_transform_matrix(A_REF(0), &r);
    return NATIVE_OK;
}

static int M3G_Transform_transformFloats(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    int n = ARRAY_LEN(arr);
    float *v = ARRAY_DATA(arr, float);
    M3GMatrix m;
    get_transform_matrix(A_REF(0), &m);
    for (int i = 0; i + 3 < n; i += 4) {
        float x = v[i], y = v[i+1], z = v[i+2], w = v[i+3];
        v[i]   = m.m[0]*x + m.m[4]*y + m.m[8]*z  + m.m[12]*w;
        v[i+1] = m.m[1]*x + m.m[5]*y + m.m[9]*z  + m.m[13]*w;
        v[i+2] = m.m[2]*x + m.m[6]*y + m.m[10]*z + m.m[14]*w;
        v[i+3] = m.m[3]*x + m.m[7]*y + m.m[11]*z + m.m[15]*w;
    }
    return NATIVE_OK;
}

/* Transformable helpers */
static void compute_node_matrix(Object *node, M3GMatrix *out)
{
    mat_identity(out);
    if (!node || !node->cls) return;
    Field *f_tx = class_find_field(node->cls, "tx", "F");
    Field *f_ty = class_find_field(node->cls, "ty", "F");
    Field *f_tz = class_find_field(node->cls, "tz", "F");
    Field *f_sx = class_find_field(node->cls, "sx", "F");
    Field *f_sy = class_find_field(node->cls, "sy", "F");
    Field *f_sz = class_find_field(node->cls, "sz", "F");
    Field *f_rx = class_find_field(node->cls, "rx", "F");
    Field *f_ry = class_find_field(node->cls, "ry", "F");
    Field *f_rz = class_find_field(node->cls, "rz", "F");
    Field *f_ra = class_find_field(node->cls, "rangle", "F");
    Field *f_hc = class_find_field(node->cls, "hasCustomTransform", "Z");
    Field *f_cm = class_find_field(node->cls, "customMatrix", "[F");
    if (!f_tx) return;

    M3GMatrix tr, rot, sc, tmp;
    mat_identity(&tr);
    tr.m[12] = FIELDS(node)[f_tx->offset].f;
    tr.m[13] = FIELDS(node)[f_ty->offset].f;
    tr.m[14] = FIELDS(node)[f_tz->offset].f;

    mat_rotate_axis(&rot, FIELDS(node)[f_ra->offset].f,
                    FIELDS(node)[f_rx->offset].f,
                    FIELDS(node)[f_ry->offset].f,
                    FIELDS(node)[f_rz->offset].f);

    mat_identity(&sc);
    sc.m[0]  = FIELDS(node)[f_sx->offset].f;
    sc.m[5]  = FIELDS(node)[f_sy->offset].f;
    sc.m[10] = FIELDS(node)[f_sz->offset].f;

    mat_mul(&tmp, &tr, &rot);
    mat_mul(out, &tmp, &sc);

    if (f_hc && GET_INT(node, f_hc) && f_cm) {
        Object *cm = GET_REF(node, f_cm);
        if (cm && ARRAY_LEN(cm) >= 16) {
            M3GMatrix custom;
            memcpy(custom.m, ARRAY_DATA(cm, float), 16 * sizeof(float));
            mat_mul(&tmp, out, &custom);
            *out = tmp;
        }
    }
}

static int M3G_Transformable_initDefaults(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *o = A_REF(0);
    if (!o || !o->cls) return NATIVE_OK;
    FIELDS(o)[class_find_field(o->cls, "sx", "F")->offset].f = 1.0f;
    FIELDS(o)[class_find_field(o->cls, "sy", "F")->offset].f = 1.0f;
    FIELDS(o)[class_find_field(o->cls, "sz", "F")->offset].f = 1.0f;
    FIELDS(o)[class_find_field(o->cls, "rz", "F")->offset].f = 1.0f;
    Field *f_re = class_find_field(o->cls, "renderingEnabled", "Z");
    if (f_re) SET_INT(o, f_re, 1);
    Field *f_af = class_find_field(o->cls, "alphaFactor", "F");
    if (f_af) FIELDS(o)[f_af->offset].f = 1.0f;
    return NATIVE_OK;
}

static int M3G_Transformable_setTranslation(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *o = A_REF(0);
    FIELDS(o)[class_find_field(o->cls, "tx", "F")->offset].f = A_FLT(1);
    FIELDS(o)[class_find_field(o->cls, "ty", "F")->offset].f = A_FLT(2);
    FIELDS(o)[class_find_field(o->cls, "tz", "F")->offset].f = A_FLT(3);
    return NATIVE_OK;
}

static int M3G_Transformable_translate(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *o = A_REF(0);
    FIELDS(o)[class_find_field(o->cls, "tx", "F")->offset].f += A_FLT(1);
    FIELDS(o)[class_find_field(o->cls, "ty", "F")->offset].f += A_FLT(2);
    FIELDS(o)[class_find_field(o->cls, "tz", "F")->offset].f += A_FLT(3);
    return NATIVE_OK;
}

static int M3G_Transformable_getTranslation(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *o = A_REF(0), *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    if (ARRAY_LEN(arr) >= 3) {
        float *d = ARRAY_DATA(arr, float);
        d[0] = FIELDS(o)[class_find_field(o->cls, "tx", "F")->offset].f;
        d[1] = FIELDS(o)[class_find_field(o->cls, "ty", "F")->offset].f;
        d[2] = FIELDS(o)[class_find_field(o->cls, "tz", "F")->offset].f;
    }
    return NATIVE_OK;
}

static int M3G_Transformable_setScale(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *o = A_REF(0);
    FIELDS(o)[class_find_field(o->cls, "sx", "F")->offset].f = A_FLT(1);
    FIELDS(o)[class_find_field(o->cls, "sy", "F")->offset].f = A_FLT(2);
    FIELDS(o)[class_find_field(o->cls, "sz", "F")->offset].f = A_FLT(3);
    return NATIVE_OK;
}

static int M3G_Transformable_getScale(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *o = A_REF(0), *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    if (ARRAY_LEN(arr) >= 3) {
        float *d = ARRAY_DATA(arr, float);
        d[0] = FIELDS(o)[class_find_field(o->cls, "sx", "F")->offset].f;
        d[1] = FIELDS(o)[class_find_field(o->cls, "sy", "F")->offset].f;
        d[2] = FIELDS(o)[class_find_field(o->cls, "sz", "F")->offset].f;
    }
    return NATIVE_OK;
}

static int M3G_Transformable_setOrientation(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *o = A_REF(0);
    FIELDS(o)[class_find_field(o->cls, "rangle", "F")->offset].f = A_FLT(1);
    FIELDS(o)[class_find_field(o->cls, "rx", "F")->offset].f     = A_FLT(2);
    FIELDS(o)[class_find_field(o->cls, "ry", "F")->offset].f     = A_FLT(3);
    FIELDS(o)[class_find_field(o->cls, "rz", "F")->offset].f     = A_FLT(4);
    return NATIVE_OK;
}

static int M3G_Transformable_getOrientation(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *o = A_REF(0), *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    if (ARRAY_LEN(arr) >= 4) {
        float *d = ARRAY_DATA(arr, float);
        d[0] = FIELDS(o)[class_find_field(o->cls, "rangle", "F")->offset].f;
        d[1] = FIELDS(o)[class_find_field(o->cls, "rx", "F")->offset].f;
        d[2] = FIELDS(o)[class_find_field(o->cls, "ry", "F")->offset].f;
        d[3] = FIELDS(o)[class_find_field(o->cls, "rz", "F")->offset].f;
    }
    return NATIVE_OK;
}

static int M3G_Transformable_postRotate(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *o = A_REF(0);
    Field *f_ra = class_find_field(o->cls, "rangle", "F");
    FIELDS(o)[f_ra->offset].f += A_FLT(1);
    FIELDS(o)[class_find_field(o->cls, "rx", "F")->offset].f = A_FLT(2);
    FIELDS(o)[class_find_field(o->cls, "ry", "F")->offset].f = A_FLT(3);
    FIELDS(o)[class_find_field(o->cls, "rz", "F")->offset].f = A_FLT(4);
    return NATIVE_OK;
}

static int M3G_Transformable_setTransform(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *o = A_REF(0);
    Object *tr = A_REF(1);
    Field *f_hc = class_find_field(o->cls, "hasCustomTransform", "Z");
    Field *f_cm = class_find_field(o->cls, "customMatrix", "[F");
    if (!tr) {
        SET_INT(o, f_hc, 0);
        return NATIVE_OK;
    }
    Array *arr = (Array *)GET_REF(o, f_cm);
    if (!arr) {
        arr = vm_new_prim_array(t, 'F', 16);
        if (!arr) return NATIVE_OK;
        SET_REF(o, f_cm, (Object *)arr);
    }
    M3GMatrix m;
    get_transform_matrix(tr, &m);
    memcpy(ARRAY_DATA(arr, float), m.m, 16 * sizeof(float));
    SET_INT(o, f_hc, 1);
    return NATIVE_OK;
}

static int M3G_Transformable_getTransform(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *o = A_REF(0), *tr = A_REF(1);
    if (!tr) { NPE(t); return NATIVE_OK; }
    Field *f_hc = class_find_field(o->cls, "hasCustomTransform", "Z");
    Field *f_cm = class_find_field(o->cls, "customMatrix", "[F");
    M3GMatrix m;
    mat_identity(&m);
    if (f_hc && GET_INT(o, f_hc) && f_cm) {
        Object *cm = GET_REF(o, f_cm);
        if (cm && ARRAY_LEN(cm) >= 16) memcpy(m.m, ARRAY_DATA(cm, float), 16 * sizeof(float));
    }
    set_transform_matrix(tr, &m);
    return NATIVE_OK;
}

static int M3G_Transformable_getCompositeTransform(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    if (!A_REF(1)) { NPE(t); return NATIVE_OK; }
    M3GMatrix m;
    compute_node_matrix(A_REF(0), &m);
    set_transform_matrix(A_REF(1), &m);
    return NATIVE_OK;
}

/* Node & Group getters/setters */
static int M3G_Node_setRenderingEnable(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "renderingEnabled", "Z"), A_INT(1) ? 1 : 0);
    return NATIVE_OK;
}

static int M3G_Node_isRenderingEnabled(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->i = GET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "renderingEnabled", "Z"));
    return NATIVE_OK;
}

static int M3G_Node_setAlphaFactor(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    FIELDS(A_REF(0))[class_find_field(A_REF(0)->cls, "alphaFactor", "F")->offset].f = A_FLT(1);
    return NATIVE_OK;
}

static int M3G_Node_getAlphaFactor(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->f = FIELDS(A_REF(0))[class_find_field(A_REF(0)->cls, "alphaFactor", "F")->offset].f;
    return NATIVE_OK;
}

static int M3G_Group_addChild(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *grp = A_REF(0), *child = A_REF(1);
    if (!child) { NPE(t); return NATIVE_OK; }
    SET_REF(child, class_find_field(child->cls, "parent", "Ljavax/microedition/m3g/Node;"), grp);
    Object *vec = GET_REF(grp, class_find_field(grp->cls, "children", "Ljava/util/Vector;"));
    if (vec) {
        Field *f_ed = class_find_field(vec->cls, "elementData", "[Ljava/lang/Object;");
        Field *f_ec = class_find_field(vec->cls, "elementCount", "I");
        Array *ed = (Array *)GET_REF(vec, f_ed);
        int ec = GET_INT(vec, f_ec);
        if (ed) {
            if (ec >= ARRAY_LEN(ed)) {
                int new_cap = ARRAY_LEN(ed) * 2 + 4;
                Array *ned = vm_new_array(t, ed->hdr.cls, new_cap);
                if (!ned) return NATIVE_OK;
                memcpy(ARRAY_DATA(ned, Object *), ARRAY_DATA(ed, Object *), (size_t)ec * sizeof(Object *));
                SET_REF(vec, f_ed, (Object *)ned);
                ed = ned;
            }
            ARRAY_DATA(ed, Object *)[ec] = child;
            SET_INT(vec, f_ec, ec + 1);
        }
    }
    return NATIVE_OK;
}

static int M3G_Group_getChildCount(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *grp = A_REF(0);
    Object *vec = GET_REF(grp, class_find_field(grp->cls, "children", "Ljava/util/Vector;"));
    ret->i = vec ? GET_INT(vec, class_find_field(vec->cls, "elementCount", "I")) : 0;
    return NATIVE_OK;
}

static int M3G_Group_getChild(Thread *t, Slot *args, Slot *ret)
{
    Object *grp = A_REF(0);
    int idx = A_INT(1);
    Object *vec = GET_REF(grp, class_find_field(grp->cls, "children", "Ljava/util/Vector;"));
    if (!vec) { vm_throw_new(t, "java/lang/IndexOutOfBoundsException", NULL); return NATIVE_OK; }
    int ec = GET_INT(vec, class_find_field(vec->cls, "elementCount", "I"));
    if (idx < 0 || idx >= ec) { vm_throw_new(t, "java/lang/IndexOutOfBoundsException", NULL); return NATIVE_OK; }
    Array *ed = (Array *)GET_REF(vec, class_find_field(vec->cls, "elementData", "[Ljava/lang/Object;"));
    ret->ref = ed ? ARRAY_DATA(ed, Object *)[idx] : NULL;
    return NATIVE_OK;
}

/* Camera natives */
static int M3G_Camera_setPerspective(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *c = A_REF(0);
    float fovy = A_FLT(1), aspect = A_FLT(2), near = A_FLT(3), far = A_FLT(4);
    SET_INT(c, class_find_field(c->cls, "projType", "I"), 50);
    FIELDS(c)[class_find_field(c->cls, "fovy", "F")->offset].f   = fovy;
    FIELDS(c)[class_find_field(c->cls, "aspect", "F")->offset].f = aspect;
    FIELDS(c)[class_find_field(c->cls, "near", "F")->offset].f   = near;
    FIELDS(c)[class_find_field(c->cls, "far", "F")->offset].f    = far;

    M3GMatrix p;
    memset(p.m, 0, sizeof(p.m));
    float rad = fovy * 0.5f * (3.141592653589793f / 180.0f);
    float f = 1.0f / tanf(rad);
    p.m[0]  = f / (aspect > 0.001f ? aspect : 1.0f);
    p.m[5]  = f;
    p.m[10] = (far + near) / (near - far);
    p.m[11] = -1.0f;
    p.m[14] = (2.0f * far * near) / (near - far);

    Field *f_pm = class_find_field(c->cls, "projMatrix", "[F");
    Array *arr = (Array *)GET_REF(c, f_pm);
    if (!arr) {
        arr = vm_new_prim_array(t, 'F', 16);
        if (arr) SET_REF(c, f_pm, (Object *)arr);
    }
    if (arr) memcpy(ARRAY_DATA(arr, float), p.m, 16 * sizeof(float));
    return NATIVE_OK;
}

static int M3G_Camera_setParallel(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *c = A_REF(0);
    float h = A_FLT(1), aspect = A_FLT(2), near = A_FLT(3), far = A_FLT(4);
    float w = h * aspect;
    SET_INT(c, class_find_field(c->cls, "projType", "I"), 49);
    M3GMatrix p;
    mat_identity(&p);
    p.m[0]  = 2.0f / (w > 0.001f ? w : 1.0f);
    p.m[5]  = 2.0f / (h > 0.001f ? h : 1.0f);
    p.m[10] = -2.0f / (far - near);
    p.m[14] = -(far + near) / (far - near);

    Field *f_pm = class_find_field(c->cls, "projMatrix", "[F");
    Array *arr = (Array *)GET_REF(c, f_pm);
    if (!arr) {
        arr = vm_new_prim_array(t, 'F', 16);
        if (arr) SET_REF(c, f_pm, (Object *)arr);
    }
    if (arr) memcpy(ARRAY_DATA(arr, float), p.m, 16 * sizeof(float));
    return NATIVE_OK;
}

static int M3G_Camera_setGeneric(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *c = A_REF(0), *tr = A_REF(1);
    if (!tr) { NPE(t); return NATIVE_OK; }
    SET_INT(c, class_find_field(c->cls, "projType", "I"), 48);
    M3GMatrix m;
    get_transform_matrix(tr, &m);
    Field *f_pm = class_find_field(c->cls, "projMatrix", "[F");
    Array *arr = (Array *)GET_REF(c, f_pm);
    if (!arr) {
        arr = vm_new_prim_array(t, 'F', 16);
        if (arr) SET_REF(c, f_pm, (Object *)arr);
    }
    if (arr) memcpy(ARRAY_DATA(arr, float), m.m, 16 * sizeof(float));
    return NATIVE_OK;
}

static int M3G_Camera_getProjectionTransform(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *c = A_REF(0), *tr = A_REF(1);
    Field *f_pm = class_find_field(c->cls, "projMatrix", "[F");
    Object *pm = GET_REF(c, f_pm);
    if (tr && pm && ARRAY_LEN(pm) >= 16) {
        M3GMatrix m;
        memcpy(m.m, ARRAY_DATA(pm, float), 16 * sizeof(float));
        set_transform_matrix(tr, &m);
    }
    ret->i = GET_INT(c, class_find_field(c->cls, "projType", "I"));
    return NATIVE_OK;
}

/* Background natives */
static int M3G_Background_setColor(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "color", "I"), A_INT(1));
    return NATIVE_OK;
}
static int M3G_Background_getColor(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->i = GET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "color", "I"));
    return NATIVE_OK;
}
static int M3G_Background_setImage(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "image", "Ljavax/microedition/m3g/Image2D;"), A_REF(1));
    return NATIVE_OK;
}
static int M3G_Background_getImage(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->ref = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "image", "Ljavax/microedition/m3g/Image2D;"));
    return NATIVE_OK;
}
static int M3G_Background_setColorClearEnable(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "colorClearEnabled", "Z"), A_INT(1) ? 1 : 0);
    return NATIVE_OK;
}
static int M3G_Background_isColorClearEnabled(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->i = GET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "colorClearEnabled", "Z"));
    return NATIVE_OK;
}
static int M3G_Background_setDepthClearEnable(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "depthClearEnabled", "Z"), A_INT(1) ? 1 : 0);
    return NATIVE_OK;
}
static int M3G_Background_isDepthClearEnabled(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->i = GET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "depthClearEnabled", "Z"));
    return NATIVE_OK;
}

/* Image2D natives */
static int M3G_Image2D_initFromObj(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    int fmt = A_INT(1);
    Object *src = A_REF(2);
    if (!src) { NPE(t); return NATIVE_OK; }
    Field *f_w = class_find_field(src->cls, "width", "I");
    Field *f_h = class_find_field(src->cls, "height", "I");
    Field *f_px = class_find_field(src->cls, "pixels", "[S");
    Field *f_al = class_find_field(src->cls, "alpha", "[B");
    if (!f_w || !f_h || !f_px) return NATIVE_OK;
    SET_INT(self, class_find_field(self->cls, "format", "I"), fmt);
    SET_INT(self, class_find_field(self->cls, "width", "I"), GET_INT(src, f_w));
    SET_INT(self, class_find_field(self->cls, "height", "I"), GET_INT(src, f_h));
    SET_REF(self, class_find_field(self->cls, "pixels", "[S"), GET_REF(src, f_px));
    if (f_al) SET_REF(self, class_find_field(self->cls, "alpha", "[B"), GET_REF(src, f_al));
    return NATIVE_OK;
}

static int M3G_Image2D_initBlank(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    int fmt = A_INT(1), w = A_INT(2), h = A_INT(3);
    if (w <= 0 || h <= 0) { vm_throw_new(t, "java/lang/IllegalArgumentException", NULL); return NATIVE_OK; }
    Array *px = vm_new_prim_array(t, 'S', w * h);
    if (!px) return NATIVE_OK;
    SET_INT(self, class_find_field(self->cls, "format", "I"), fmt);
    SET_INT(self, class_find_field(self->cls, "width", "I"), w);
    SET_INT(self, class_find_field(self->cls, "height", "I"), h);
    SET_REF(self, class_find_field(self->cls, "pixels", "[S"), (Object *)px);
    return NATIVE_OK;
}

static int M3G_Image2D_initBytes(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    int fmt = A_INT(1), w = A_INT(2), h = A_INT(3);
    Object *data = A_REF(4);
    if (!data) { NPE(t); return NATIVE_OK; }
    Array *px = vm_new_prim_array(t, 'S', w * h);
    Array *al = vm_new_prim_array(t, 'B', w * h);
    if (!px || !al) return NATIVE_OK;
    uint16_t *dst = ARRAY_DATA(px, uint16_t);
    uint8_t *adst = ARRAY_DATA(al, uint8_t);
    const uint8_t *src = ARRAY_DATA(data, uint8_t);
    int n = w * h;
    for (int i = 0; i < n; i++) {
        uint8_t r = 255, g = 255, b = 255, a = 255;
        if (fmt == 96) { a = src[i]; }
        else if (fmt == 97) { r = g = b = src[i]; }
        else if (fmt == 98) { r = g = b = src[i * 2]; a = src[i * 2 + 1]; }
        else if (fmt == 99) { r = src[i * 3]; g = src[i * 3 + 1]; b = src[i * 3 + 2]; }
        else { r = src[i * 4]; g = src[i * 4 + 1]; b = src[i * 4 + 2]; a = src[i * 4 + 3]; }
        dst[i] = gfx_pack(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        adst[i] = a;
    }
    SET_INT(self, class_find_field(self->cls, "format", "I"), fmt);
    SET_INT(self, class_find_field(self->cls, "width", "I"), w);
    SET_INT(self, class_find_field(self->cls, "height", "I"), h);
    SET_REF(self, class_find_field(self->cls, "pixels", "[S"), (Object *)px);
    SET_REF(self, class_find_field(self->cls, "alpha", "[B"), (Object *)al);
    return NATIVE_OK;
}

static int M3G_Image2D_initPal(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    int fmt = A_INT(1), w = A_INT(2), h = A_INT(3);
    Object *idx_arr = A_REF(4), *pal_arr = A_REF(5);
    if (!idx_arr || !pal_arr) { NPE(t); return NATIVE_OK; }
    Array *px = vm_new_prim_array(t, 'S', w * h);
    Array *al = vm_new_prim_array(t, 'B', w * h);
    if (!px || !al) return NATIVE_OK;
    uint16_t *dst = ARRAY_DATA(px, uint16_t);
    uint8_t *adst = ARRAY_DATA(al, uint8_t);
    const uint8_t *idxs = ARRAY_DATA(idx_arr, uint8_t);
    const uint8_t *pal  = ARRAY_DATA(pal_arr, uint8_t);
    int stride = (fmt == 100) ? 4 : (fmt == 99 ? 3 : (fmt == 98 ? 2 : 1));
    int n = w * h;
    for (int i = 0; i < n; i++) {
        int p = idxs[i] * stride;
        uint8_t r = 255, g = 255, b = 255, a = 255;
        if (fmt == 96) a = pal[p];
        else if (fmt == 97) r = g = b = pal[p];
        else if (fmt == 98) { r = g = b = pal[p]; a = pal[p + 1]; }
        else if (fmt == 99) { r = pal[p]; g = pal[p + 1]; b = pal[p + 2]; }
        else { r = pal[p]; g = pal[p + 1]; b = pal[p + 2]; a = pal[p + 3]; }
        dst[i] = gfx_pack(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        adst[i] = a;
    }
    SET_INT(self, class_find_field(self->cls, "format", "I"), fmt);
    SET_INT(self, class_find_field(self->cls, "width", "I"), w);
    SET_INT(self, class_find_field(self->cls, "height", "I"), h);
    SET_REF(self, class_find_field(self->cls, "pixels", "[S"), (Object *)px);
    SET_REF(self, class_find_field(self->cls, "alpha", "[B"), (Object *)al);
    return NATIVE_OK;
}

/* VertexArray, VertexBuffer, TriangleStripArray, Mesh, Appearance */
static int M3G_VertexArray_init(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    int vc = A_INT(1), cc = A_INT(2), ct = A_INT(3);
    SET_INT(self, class_find_field(self->cls, "vertexCount", "I"), vc);
    SET_INT(self, class_find_field(self->cls, "componentCount", "I"), cc);
    SET_INT(self, class_find_field(self->cls, "componentType", "I"), ct);
    if (ct == 1) {
        Array *b = vm_new_prim_array(t, 'B', vc * cc);
        if (b) SET_REF(self, class_find_field(self->cls, "byteData", "[B"), (Object *)b);
    } else {
        Array *s = vm_new_prim_array(t, 'S', vc * cc);
        if (s) SET_REF(self, class_find_field(self->cls, "shortData", "[S"), (Object *)s);
    }
    return NATIVE_OK;
}

static int M3G_VertexArray_setBytes(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0), *src = A_REF(3);
    if (!src) { NPE(t); return NATIVE_OK; }
    int first = A_INT(1), count = A_INT(2);
    int cc = GET_INT(self, class_find_field(self->cls, "componentCount", "I"));
    Object *dst = GET_REF(self, class_find_field(self->cls, "byteData", "[B"));
    if (dst) memcpy(ARRAY_DATA(dst, int8_t) + first * cc, ARRAY_DATA(src, int8_t), (size_t)(count * cc));
    return NATIVE_OK;
}

static int M3G_VertexArray_setShorts(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0), *src = A_REF(3);
    if (!src) { NPE(t); return NATIVE_OK; }
    int first = A_INT(1), count = A_INT(2);
    int cc = GET_INT(self, class_find_field(self->cls, "componentCount", "I"));
    Object *dst = GET_REF(self, class_find_field(self->cls, "shortData", "[S"));
    if (dst) memcpy(ARRAY_DATA(dst, int16_t) + first * cc, ARRAY_DATA(src, int16_t), (size_t)(count * cc) * 2);
    return NATIVE_OK;
}

static int M3G_VertexBuffer_init(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    FIELDS(self)[class_find_field(self->cls, "posScale", "F")->offset].f = 1.0f;
    SET_INT(self, class_find_field(self->cls, "defaultColor", "I"), (jint)0xFFFFFFFF);
    Array *pb = vm_new_prim_array(t, 'F', 3);
    Array *ta = vm_new_array(t, class_array_of(class_load("javax/microedition/m3g/VertexArray")), 2);
    Array *ts = vm_new_prim_array(t, 'F', 2);
    if (pb) SET_REF(self, class_find_field(self->cls, "posBias", "[F"), (Object *)pb);
    if (ta) SET_REF(self, class_find_field(self->cls, "texcoords", "[Ljavax/microedition/m3g/VertexArray;"), (Object *)ta);
    if (ts) {
        ARRAY_DATA(ts, float)[0] = ARRAY_DATA(ts, float)[1] = 1.0f;
        SET_REF(self, class_find_field(self->cls, "texScale", "[F"), (Object *)ts);
    }
    return NATIVE_OK;
}

static int M3G_VertexBuffer_getVertexCount(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *self = A_REF(0);
    Object *pos = GET_REF(self, class_find_field(self->cls, "positions", "Ljavax/microedition/m3g/VertexArray;"));
    ret->i = pos ? GET_INT(pos, class_find_field(pos->cls, "vertexCount", "I")) : 0;
    return NATIVE_OK;
}

static int M3G_VertexBuffer_setPositions(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *self = A_REF(0), *va = A_REF(1), *bias = A_REF(3);
    SET_REF(self, class_find_field(self->cls, "positions", "Ljavax/microedition/m3g/VertexArray;"), va);
    FIELDS(self)[class_find_field(self->cls, "posScale", "F")->offset].f = A_FLT(2);
    Object *pb = GET_REF(self, class_find_field(self->cls, "posBias", "[F"));
    if (pb && bias && ARRAY_LEN(bias) >= 3) {
        memcpy(ARRAY_DATA(pb, float), ARRAY_DATA(bias, float), 3 * sizeof(float));
    }
    return NATIVE_OK;
}

static int M3G_VertexBuffer_getPositions(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *self = A_REF(0), *sb = A_REF(1);
    if (sb && ARRAY_LEN(sb) >= 4) {
        float *d = ARRAY_DATA(sb, float);
        d[0] = FIELDS(self)[class_find_field(self->cls, "posScale", "F")->offset].f;
        Object *pb = GET_REF(self, class_find_field(self->cls, "posBias", "[F"));
        if (pb && ARRAY_LEN(pb) >= 3) memcpy(d + 1, ARRAY_DATA(pb, float), 3 * sizeof(float));
    }
    ret->ref = GET_REF(self, class_find_field(self->cls, "positions", "Ljavax/microedition/m3g/VertexArray;"));
    return NATIVE_OK;
}

static int M3G_VertexBuffer_setTexCoords(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *self = A_REF(0);
    int idx = A_INT(1);
    Object *va = A_REF(2);
    if (idx < 0 || idx >= 2) return NATIVE_OK;
    Object *ta = GET_REF(self, class_find_field(self->cls, "texcoords", "[Ljavax/microedition/m3g/VertexArray;"));
    Object *ts = GET_REF(self, class_find_field(self->cls, "texScale", "[F"));
    if (ta) ARRAY_DATA(ta, Object *)[idx] = va;
    if (ts) ARRAY_DATA(ts, float)[idx] = A_FLT(3);
    return NATIVE_OK;
}

static int M3G_VertexBuffer_setNormals(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "normals", "Ljavax/microedition/m3g/VertexArray;"), A_REF(1));
    return NATIVE_OK;
}

static int M3G_VertexBuffer_setColors(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "colors", "Ljavax/microedition/m3g/VertexArray;"), A_REF(1));
    return NATIVE_OK;
}

static int M3G_VertexBuffer_setDefaultColor(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "defaultColor", "I"), A_INT(1));
    return NATIVE_OK;
}

static int M3G_TriangleStripArray_initExplicit(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0), *idx_arr = A_REF(1), *len_arr = A_REF(2);
    if (!idx_arr || !len_arr) { NPE(t); return NATIVE_OK; }
    const int32_t *in_idx = ARRAY_DATA(idx_arr, int32_t);
    const int32_t *strips = ARRAY_DATA(len_arr, int32_t);
    int n_strips = ARRAY_LEN(len_arr);
    int total_tris = 0;
    for (int s = 0; s < n_strips; s++) {
        if (strips[s] >= 3) total_tris += (strips[s] - 2);
    }
    Array *out = vm_new_prim_array(t, 'S', total_tris * 3);
    if (!out) return NATIVE_OK;
    uint16_t *dst = ARRAY_DATA(out, uint16_t);
    int src_pos = 0, dst_pos = 0;
    for (int s = 0; s < n_strips; s++) {
        int slen = strips[s];
        for (int k = 0; k + 2 < slen; k++) {
            if ((k & 1) == 0) {
                dst[dst_pos++] = (uint16_t)in_idx[src_pos + k];
                dst[dst_pos++] = (uint16_t)in_idx[src_pos + k + 1];
                dst[dst_pos++] = (uint16_t)in_idx[src_pos + k + 2];
            } else {
                dst[dst_pos++] = (uint16_t)in_idx[src_pos + k + 1];
                dst[dst_pos++] = (uint16_t)in_idx[src_pos + k];
                dst[dst_pos++] = (uint16_t)in_idx[src_pos + k + 2];
            }
        }
        src_pos += slen;
    }
    SET_REF(self, class_find_field(self->cls, "indices", "[S"), (Object *)out);
    return NATIVE_OK;
}

static int M3G_TriangleStripArray_initImplicit(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    int first = A_INT(1);
    Object *len_arr = A_REF(2);
    if (!len_arr) { NPE(t); return NATIVE_OK; }
    const int32_t *strips = ARRAY_DATA(len_arr, int32_t);
    int n_strips = ARRAY_LEN(len_arr);
    int total_tris = 0;
    for (int s = 0; s < n_strips; s++) {
        if (strips[s] >= 3) total_tris += (strips[s] - 2);
    }
    Array *out = vm_new_prim_array(t, 'S', total_tris * 3);
    if (!out) return NATIVE_OK;
    uint16_t *dst = ARRAY_DATA(out, uint16_t);
    int cur = first, dst_pos = 0;
    for (int s = 0; s < n_strips; s++) {
        int slen = strips[s];
        for (int k = 0; k + 2 < slen; k++) {
            if ((k & 1) == 0) {
                dst[dst_pos++] = (uint16_t)(cur + k);
                dst[dst_pos++] = (uint16_t)(cur + k + 1);
                dst[dst_pos++] = (uint16_t)(cur + k + 2);
            } else {
                dst[dst_pos++] = (uint16_t)(cur + k + 1);
                dst[dst_pos++] = (uint16_t)(cur + k);
                dst[dst_pos++] = (uint16_t)(cur + k + 2);
            }
        }
        cur += slen;
    }
    SET_REF(self, class_find_field(self->cls, "indices", "[S"), (Object *)out);
    return NATIVE_OK;
}

static int M3G_IndexBuffer_getIndexCount(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *idx_obj = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "indices", "[S"));
    ret->i = idx_obj ? ARRAY_LEN(idx_obj) : 0;
    return NATIVE_OK;
}

static int M3G_Mesh_initSingle(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0);
    SET_REF(self, class_find_field(self->cls, "vertices", "Ljavax/microedition/m3g/VertexBuffer;"), A_REF(1));
    Array *ibs = vm_new_array(t, class_array_of(class_load("javax/microedition/m3g/IndexBuffer")), 1);
    Array *aps = vm_new_array(t, class_array_of(class_load("javax/microedition/m3g/Appearance")), 1);
    if (ibs) { ARRAY_DATA(ibs, Object *)[0] = A_REF(2); SET_REF(self, class_find_field(self->cls, "submeshes", "[Ljavax/microedition/m3g/IndexBuffer;"), (Object *)ibs); }
    if (aps) { ARRAY_DATA(aps, Object *)[0] = A_REF(3); SET_REF(self, class_find_field(self->cls, "appearances", "[Ljavax/microedition/m3g/Appearance;"), (Object *)aps); }
    return NATIVE_OK;
}

static int M3G_Mesh_initMulti(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *self = A_REF(0);
    SET_REF(self, class_find_field(self->cls, "vertices", "Ljavax/microedition/m3g/VertexBuffer;"), A_REF(1));
    SET_REF(self, class_find_field(self->cls, "submeshes", "[Ljavax/microedition/m3g/IndexBuffer;"), A_REF(2));
    SET_REF(self, class_find_field(self->cls, "appearances", "[Ljavax/microedition/m3g/Appearance;"), A_REF(3));
    return NATIVE_OK;
}

static int M3G_Mesh_getVertexBuffer(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->ref = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "vertices", "Ljavax/microedition/m3g/VertexBuffer;"));
    return NATIVE_OK;
}

static int M3G_Mesh_getSubmeshCount(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *ibs = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "submeshes", "[Ljavax/microedition/m3g/IndexBuffer;"));
    ret->i = ibs ? ARRAY_LEN(ibs) : 0;
    return NATIVE_OK;
}

static int M3G_Mesh_getIndexBuffer(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *ibs = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "submeshes", "[Ljavax/microedition/m3g/IndexBuffer;"));
    int i = A_INT(1);
    ret->ref = (ibs && i >= 0 && i < ARRAY_LEN(ibs)) ? ARRAY_DATA(ibs, Object *)[i] : NULL;
    return NATIVE_OK;
}

static int M3G_Mesh_setAppearance(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *aps = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "appearances", "[Ljavax/microedition/m3g/Appearance;"));
    int i = A_INT(1);
    if (aps && i >= 0 && i < ARRAY_LEN(aps)) ARRAY_DATA(aps, Object *)[i] = A_REF(2);
    return NATIVE_OK;
}

static int M3G_Mesh_getAppearance(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *aps = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "appearances", "[Ljavax/microedition/m3g/Appearance;"));
    int i = A_INT(1);
    ret->ref = (aps && i >= 0 && i < ARRAY_LEN(aps)) ? ARRAY_DATA(aps, Object *)[i] : NULL;
    return NATIVE_OK;
}

static int M3G_Appearance_setTexture(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *self = A_REF(0);
    int unit = A_INT(1);
    Object *arr = GET_REF(self, class_find_field(self->cls, "textures", "[Ljavax/microedition/m3g/Texture2D;"));
    if (arr && unit >= 0 && unit < ARRAY_LEN(arr)) {
        ARRAY_DATA(arr, Object *)[unit] = A_REF(2);
    }
    return NATIVE_OK;
}

static int M3G_Appearance_getTexture(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    Object *arr = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "textures", "[Ljavax/microedition/m3g/Texture2D;"));
    int unit = A_INT(1);
    ret->ref = (arr && unit >= 0 && unit < ARRAY_LEN(arr)) ? ARRAY_DATA(arr, Object *)[unit] : NULL;
    return NATIVE_OK;
}

static int M3G_Appearance_setPolygonMode(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "polygonMode", "Ljavax/microedition/m3g/PolygonMode;"), A_REF(1));
    return NATIVE_OK;
}

static int M3G_Appearance_getPolygonMode(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->ref = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "polygonMode", "Ljavax/microedition/m3g/PolygonMode;"));
    return NATIVE_OK;
}

static int M3G_Appearance_setCompositingMode(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "compositingMode", "Ljavax/microedition/m3g/CompositingMode;"), A_REF(1));
    return NATIVE_OK;
}

static int M3G_Appearance_getCompositingMode(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->ref = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "compositingMode", "Ljavax/microedition/m3g/CompositingMode;"));
    return NATIVE_OK;
}

static int M3G_Appearance_setMaterial(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "material", "Ljavax/microedition/m3g/Material;"), A_REF(1));
    return NATIVE_OK;
}

static int M3G_Appearance_getMaterial(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->ref = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "material", "Ljavax/microedition/m3g/Material;"));
    return NATIVE_OK;
}

static int M3G_PolygonMode_setCulling(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "culling", "I"), A_INT(1));
    return NATIVE_OK;
}

static int M3G_PolygonMode_setWinding(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "winding", "I"), A_INT(1));
    return NATIVE_OK;
}

static int M3G_CompositingMode_setBlending(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "blending", "I"), A_INT(1));
    return NATIVE_OK;
}

static void render_submesh(Object *vb, Object *ib, Object *ap, const M3GMatrix *modelview)
{
    if (!vb || !ib) return;
    s_mv = *modelview;
    mat_mul(&s_mvp, &s_proj, &s_mv);

    Object *pos_va = GET_REF(vb, class_find_field(vb->cls, "positions", "Ljavax/microedition/m3g/VertexArray;"));
    if (!pos_va) return;
    int pos_ct = GET_INT(pos_va, class_find_field(pos_va->cls, "componentType", "I"));
    int pos_cc = GET_INT(pos_va, class_find_field(pos_va->cls, "componentCount", "I"));
    const void *pos_ptr = (pos_ct == 1)
        ? (const void *)ARRAY_DATA(GET_REF(pos_va, class_find_field(pos_va->cls, "byteData", "[B")), int8_t)
        : (const void *)ARRAY_DATA(GET_REF(pos_va, class_find_field(pos_va->cls, "shortData", "[S")), int16_t);
    float pos_scale = FIELDS(vb)[class_find_field(vb->cls, "posScale", "F")->offset].f;
    Object *pb_obj = GET_REF(vb, class_find_field(vb->cls, "posBias", "[F"));
    float pos_bias[3] = {0, 0, 0};
    if (pb_obj && ARRAY_LEN(pb_obj) >= 3) memcpy(pos_bias, ARRAY_DATA(pb_obj, float), 3 * sizeof(float));

    const void *tex_ptr = NULL;
    int tex_ct = 2;
    float tex_scale = 1.0f;
    float tex_bias[2] = {0, 0};
    Object *ta_obj = GET_REF(vb, class_find_field(vb->cls, "texcoords", "[Ljavax/microedition/m3g/VertexArray;"));
    if (ta_obj && ARRAY_LEN(ta_obj) > 0 && ARRAY_DATA(ta_obj, Object *)[0]) {
        Object *tva = ARRAY_DATA(ta_obj, Object *)[0];
        tex_ct = GET_INT(tva, class_find_field(tva->cls, "componentType", "I"));
        tex_ptr = (tex_ct == 1)
            ? (const void *)ARRAY_DATA(GET_REF(tva, class_find_field(tva->cls, "byteData", "[B")), int8_t)
            : (const void *)ARRAY_DATA(GET_REF(tva, class_find_field(tva->cls, "shortData", "[S")), int16_t);
        Object *ts_obj = GET_REF(vb, class_find_field(vb->cls, "texScale", "[F"));
        if (ts_obj) tex_scale = ARRAY_DATA(ts_obj, float)[0];
    }

    const void *col_ptr = NULL;
    int col_cc = 3;
    Object *cva = GET_REF(vb, class_find_field(vb->cls, "colors", "Ljavax/microedition/m3g/VertexArray;"));
    if (cva) {
        col_cc = GET_INT(cva, class_find_field(cva->cls, "componentCount", "I"));
        Object *cbd = GET_REF(cva, class_find_field(cva->cls, "byteData", "[B"));
        if (cbd) col_ptr = ARRAY_DATA(cbd, uint8_t);
    }
    uint32_t def_col = (uint32_t)GET_INT(vb, class_find_field(vb->cls, "defaultColor", "I"));

    Object *idx_obj = GET_REF(ib, class_find_field(ib->cls, "indices", "[S"));
    if (!idx_obj) return;
    const uint16_t *indices = ARRAY_DATA(idx_obj, uint16_t);
    int index_count = ARRAY_LEN(idx_obj);

    s_tex_px = NULL; s_tex_al = NULL; s_tex_w = 0; s_tex_h = 0;
    int winding = 168, culling = 160, blend_mode = 68;
    if (ap) {
        Object *pm = GET_REF(ap, class_find_field(ap->cls, "polygonMode", "Ljavax/microedition/m3g/PolygonMode;"));
        if (pm) {
            int c = GET_INT(pm, class_find_field(pm->cls, "culling", "I"));
            int w = GET_INT(pm, class_find_field(pm->cls, "winding", "I"));
            if (c) culling = c;
            if (w) winding = w;
        }
        Object *cm = GET_REF(ap, class_find_field(ap->cls, "compositingMode", "Ljavax/microedition/m3g/CompositingMode;"));
        if (cm) {
            int b = GET_INT(cm, class_find_field(cm->cls, "blending", "I"));
            if (b) blend_mode = b;
        }
        Object *texs = GET_REF(ap, class_find_field(ap->cls, "textures", "[Ljavax/microedition/m3g/Texture2D;"));
        if (texs && ARRAY_LEN(texs) > 0 && ARRAY_DATA(texs, Object *)[0]) {
            Object *t2d = ARRAY_DATA(texs, Object *)[0];
            Object *img = GET_REF(t2d, class_find_field(t2d->cls, "image", "Ljavax/microedition/m3g/Image2D;"));
            if (img) {
                Object *ipx = GET_REF(img, class_find_field(img->cls, "pixels", "[S"));
                Object *ial = GET_REF(img, class_find_field(img->cls, "alpha", "[B"));
                if (ipx) {
                    s_tex_px = ARRAY_DATA(ipx, uint16_t);
                    s_tex_al = ial ? ARRAY_DATA(ial, uint8_t) : NULL;
                    s_tex_w  = GET_INT(img, class_find_field(img->cls, "width", "I"));
                    s_tex_h  = GET_INT(img, class_find_field(img->cls, "height", "I"));
                }
            }
        }
    }

    submit_triangle_list(pos_ptr, pos_ct, pos_cc, pos_scale, pos_bias,
                         tex_ptr, tex_ct, tex_scale, tex_bias,
                         col_ptr, col_cc, def_col,
                         indices, index_count, winding, culling, blend_mode);
}

static void render_node_recursive(Object *node, const M3GMatrix *parent_mv)
{
    if (!node || !node->cls) return;
    Field *f_re = class_find_field(node->cls, "renderingEnabled", "Z");
    if (f_re && !GET_INT(node, f_re)) return;

    M3GMatrix local, world_mv;
    compute_node_matrix(node, &local);
    mat_mul(&world_mv, parent_mv, &local);

    if (class_is_subclass(node->cls, class_load("javax/microedition/m3g/Mesh"))) {
        Object *vb  = GET_REF(node, class_find_field(node->cls, "vertices", "Ljavax/microedition/m3g/VertexBuffer;"));
        Object *ibs = GET_REF(node, class_find_field(node->cls, "submeshes", "[Ljavax/microedition/m3g/IndexBuffer;"));
        Object *aps = GET_REF(node, class_find_field(node->cls, "appearances", "[Ljavax/microedition/m3g/Appearance;"));
        if (vb && ibs) {
            int n = ARRAY_LEN(ibs);
            for (int i = 0; i < n; i++) {
                Object *ib = ARRAY_DATA(ibs, Object *)[i];
                Object *ap = (aps && i < ARRAY_LEN(aps)) ? ARRAY_DATA(aps, Object *)[i] : NULL;
                render_submesh(vb, ib, ap, &world_mv);
            }
        }
    } else if (class_is_subclass(node->cls, class_load("javax/microedition/m3g/Group"))) {
        Object *vec = GET_REF(node, class_find_field(node->cls, "children", "Ljava/util/Vector;"));
        if (vec) {
            Array *ed = (Array *)GET_REF(vec, class_find_field(vec->cls, "elementData", "[Ljava/lang/Object;"));
            int ec = GET_INT(vec, class_find_field(vec->cls, "elementCount", "I"));
            if (ed) {
                for (int i = 0; i < ec && i < ARRAY_LEN(ed); i++) {
                    render_node_recursive(ARRAY_DATA(ed, Object *)[i], &world_mv);
                }
            }
        }
    }
}

/* Graphics3D natives */
static int M3G_G3D_bindTarget(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0), *g = A_REF(1);
    if (!g) { NPE(t); return NATIVE_OK; }
    SET_REF(self, class_find_field(self->cls, "target", "Ljava/lang/Object;"), g);
    SET_INT(self, class_find_field(self->cls, "depthEnabled", "Z"), 1);
    bind_graphics_target(g, true);
    s_vp_x = s_clip.x1; s_vp_y = s_clip.y1;
    s_vp_w = s_clip.x2 - s_clip.x1; s_vp_h = s_clip.y2 - s_clip.y1;
    SET_INT(self, class_find_field(self->cls, "viewportX", "I"), s_vp_x);
    SET_INT(self, class_find_field(self->cls, "viewportY", "I"), s_vp_y);
    SET_INT(self, class_find_field(self->cls, "viewportWidth", "I"), s_vp_w);
    SET_INT(self, class_find_field(self->cls, "viewportHeight", "I"), s_vp_h);
    return NATIVE_OK;
}

static int M3G_G3D_bindTargetEx(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0), *g = A_REF(1);
    bool depth = A_INT(2) != 0;
    if (!g) { NPE(t); return NATIVE_OK; }
    SET_REF(self, class_find_field(self->cls, "target", "Ljava/lang/Object;"), g);
    SET_INT(self, class_find_field(self->cls, "depthEnabled", "Z"), depth ? 1 : 0);
    bind_graphics_target(g, depth);
    s_vp_x = s_clip.x1; s_vp_y = s_clip.y1;
    s_vp_w = s_clip.x2 - s_clip.x1; s_vp_h = s_clip.y2 - s_clip.y1;
    return NATIVE_OK;
}

static int M3G_G3D_releaseTarget(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *self = A_REF(0);
    SET_REF(self, class_find_field(self->cls, "target", "Ljava/lang/Object;"), NULL);
    s_target.px = NULL;
    return NATIVE_OK;
}

static int M3G_G3D_setViewport(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *self = A_REF(0);
    s_vp_x = A_INT(1); s_vp_y = A_INT(2); s_vp_w = A_INT(3); s_vp_h = A_INT(4);
    SET_INT(self, class_find_field(self->cls, "viewportX", "I"), s_vp_x);
    SET_INT(self, class_find_field(self->cls, "viewportY", "I"), s_vp_y);
    SET_INT(self, class_find_field(self->cls, "viewportWidth", "I"), s_vp_w);
    SET_INT(self, class_find_field(self->cls, "viewportHeight", "I"), s_vp_h);
    return NATIVE_OK;
}

static int M3G_G3D_setCamera(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *self = A_REF(0), *cam = A_REF(1), *tr = A_REF(2);
    SET_REF(self, class_find_field(self->cls, "camera", "Ljavax/microedition/m3g/Camera;"), cam);
    SET_REF(self, class_find_field(self->cls, "cameraTransform", "Ljavax/microedition/m3g/Transform;"), tr);
    if (cam) {
        Field *f_pm = class_find_field(cam->cls, "projMatrix", "[F");
        Object *pm = GET_REF(cam, f_pm);
        if (pm && ARRAY_LEN(pm) >= 16) memcpy(s_proj.m, ARRAY_DATA(pm, float), 16 * sizeof(float));
        else mat_identity(&s_proj);
    }
    return NATIVE_OK;
}

static int M3G_G3D_clear(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *bg = A_REF(1);
    uint32_t col = 0xFF000000;
    bool c_en = true, d_en = true;
    if (bg) {
        col  = (uint32_t)GET_INT(bg, class_find_field(bg->cls, "color", "I"));
        c_en = GET_INT(bg, class_find_field(bg->cls, "colorClearEnabled", "Z")) != 0;
        d_en = GET_INT(bg, class_find_field(bg->cls, "depthClearEnabled", "Z")) != 0;
    }
    if (c_en && s_target.px) {
        gfx_fill_rect(&s_target, &s_clip, s_vp_x, s_vp_y, s_vp_w, s_vp_h, gfx_pack(col));
    }
    if (d_en && s_zbuf) {
        memset(s_zbuf, 0xFF, (size_t)s_zw * (size_t)s_zh * sizeof(uint16_t));
    }
    return NATIVE_OK;
}

static int M3G_G3D_renderSubmesh(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    M3GMatrix mv, cam_m, cam_inv;
    get_transform_matrix(A_REF(4), &mv);
    Object *self = A_REF(0);
    Object *cam_tr = GET_REF(self, class_find_field(self->cls, "cameraTransform", "Ljavax/microedition/m3g/Transform;"));
    if (cam_tr) {
        get_transform_matrix(cam_tr, &cam_m);
        if (mat_invert(&cam_inv, &cam_m)) {
            M3GMatrix tmp;
            mat_mul(&tmp, &cam_inv, &mv);
            mv = tmp;
        }
    }
    render_submesh(A_REF(1), A_REF(2), A_REF(3), &mv);
    return NATIVE_OK;
}

static int M3G_G3D_renderNode(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    M3GMatrix mv, cam_m, cam_inv;
    get_transform_matrix(A_REF(2), &mv);
    Object *self = A_REF(0);
    Object *cam_tr = GET_REF(self, class_find_field(self->cls, "cameraTransform", "Ljavax/microedition/m3g/Transform;"));
    if (cam_tr) {
        get_transform_matrix(cam_tr, &cam_m);
        if (mat_invert(&cam_inv, &cam_m)) {
            M3GMatrix tmp;
            mat_mul(&tmp, &cam_inv, &mv);
            mv = tmp;
        }
    }
    render_node_recursive(A_REF(1), &mv);
    return NATIVE_OK;
}

static int M3G_G3D_renderWorld(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *w = A_REF(1);
    if (!w) { NPE(t); return NATIVE_OK; }
    Object *bg = GET_REF(w, class_find_field(w->cls, "background", "Ljavax/microedition/m3g/Background;"));
    Slot clr_args[2]; clr_args[0].ref = A_REF(0); clr_args[1].ref = bg;
    M3G_G3D_clear(t, clr_args, ret);

    Object *cam = GET_REF(w, class_find_field(w->cls, "activeCamera", "Ljavax/microedition/m3g/Camera;"));
    M3GMatrix view;
    mat_identity(&view);
    if (cam) {
        Field *f_pm = class_find_field(cam->cls, "projMatrix", "[F");
        Object *pm = GET_REF(cam, f_pm);
        if (pm && ARRAY_LEN(pm) >= 16) memcpy(s_proj.m, ARRAY_DATA(pm, float), 16 * sizeof(float));
        M3GMatrix cam_local, cam_inv;
        compute_node_matrix(cam, &cam_local);
        if (mat_invert(&cam_inv, &cam_local)) view = cam_inv;
    }
    render_node_recursive(w, &view);
    return NATIVE_OK;
}

/* ------------------------------------------------------------------------ */
/* Complete JSR-184 Binary .m3g Stream Parser                               */
/* ------------------------------------------------------------------------ */
typedef struct {
    const uint8_t *buf;
    uint32_t       len;
    uint32_t       pos;
} M3GReader;

static inline uint8_t r_u8(M3GReader *r) { return (r->pos < r->len) ? r->buf[r->pos++] : 0; }
static inline uint16_t r_u16(M3GReader *r) {
    if (r->pos + 2 > r->len) { r->pos = r->len; return 0; }
    uint16_t v = (uint16_t)r->buf[r->pos] | ((uint16_t)r->buf[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}
static inline uint32_t r_u32(M3GReader *r) {
    if (r->pos + 4 > r->len) { r->pos = r->len; return 0; }
    uint32_t v = (uint32_t)r->buf[r->pos] | ((uint32_t)r->buf[r->pos + 1] << 8) |
                 ((uint32_t)r->buf[r->pos + 2] << 16) | ((uint32_t)r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}
static inline float r_f32(M3GReader *r) {
    uint32_t u = r_u32(r);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline uint32_t r_rgba(M3GReader *r) {
    uint8_t red = r_u8(r), grn = r_u8(r), blu = r_u8(r), alp = r_u8(r);
    return ((uint32_t)alp << 24) | ((uint32_t)red << 16) | ((uint32_t)grn << 8) | blu;
}
static void r_skip_str(M3GReader *r) {
    while (r->pos < r->len && r->buf[r->pos++] != 0) {}
}

#define M3G_MAX_OBJS 512

static void m3g_parse_object3d_base(M3GReader *r, Object *obj)
{
    uint32_t uid = r_u32(r);
    if (obj) SET_INT(obj, class_find_field(obj->cls, "userID", "I"), (jint)uid);
    uint32_t n_tracks = r_u32(r);
    for (uint32_t i = 0; i < n_tracks; i++) (void)r_u32(r);
    uint32_t n_params = r_u32(r);
    for (uint32_t i = 0; i < n_params; i++) {
        (void)r_u32(r);
        uint32_t plen = r_u32(r);
        r->pos = (r->pos + plen <= r->len) ? (r->pos + plen) : r->len;
    }
}

static void m3g_parse_transformable(M3GReader *r, Thread *t, Object *obj)
{
    m3g_parse_object3d_base(r, obj);
    uint8_t has_comp = r_u8(r);
    if (has_comp) {
        float tx = r_f32(r), ty = r_f32(r), tz = r_f32(r);
        float sx = r_f32(r), sy = r_f32(r), sz = r_f32(r);
        float ang = r_f32(r), ax = r_f32(r), ay = r_f32(r), az = r_f32(r);
        if (obj) {
            FIELDS(obj)[class_find_field(obj->cls, "tx", "F")->offset].f = tx;
            FIELDS(obj)[class_find_field(obj->cls, "ty", "F")->offset].f = ty;
            FIELDS(obj)[class_find_field(obj->cls, "tz", "F")->offset].f = tz;
            FIELDS(obj)[class_find_field(obj->cls, "sx", "F")->offset].f = sx;
            FIELDS(obj)[class_find_field(obj->cls, "sy", "F")->offset].f = sy;
            FIELDS(obj)[class_find_field(obj->cls, "sz", "F")->offset].f = sz;
            FIELDS(obj)[class_find_field(obj->cls, "rangle", "F")->offset].f = ang;
            FIELDS(obj)[class_find_field(obj->cls, "rx", "F")->offset].f = ax;
            FIELDS(obj)[class_find_field(obj->cls, "ry", "F")->offset].f = ay;
            FIELDS(obj)[class_find_field(obj->cls, "rz", "F")->offset].f = az;
        }
    } else if (obj) {
        FIELDS(obj)[class_find_field(obj->cls, "sx", "F")->offset].f = 1.0f;
        FIELDS(obj)[class_find_field(obj->cls, "sy", "F")->offset].f = 1.0f;
        FIELDS(obj)[class_find_field(obj->cls, "sz", "F")->offset].f = 1.0f;
        FIELDS(obj)[class_find_field(obj->cls, "rz", "F")->offset].f = 1.0f;
    }
    uint8_t has_gen = r_u8(r);
    if (has_gen) {
        float mat[16];
        /* M3G file stores 4x4 matrix in row-major order; convert to column-major */
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++)
                mat[col * 4 + row] = r_f32(r);
        if (obj) {
            Array *cm = vm_new_prim_array(t, 'F', 16);
            if (cm) {
                memcpy(ARRAY_DATA(cm, float), mat, 16 * sizeof(float));
                SET_REF(obj, class_find_field(obj->cls, "customMatrix", "[F"), (Object *)cm);
                SET_INT(obj, class_find_field(obj->cls, "hasCustomTransform", "Z"), 1);
            }
        }
    }
}

static void m3g_parse_node(M3GReader *r, Thread *t, Object *obj)
{
    m3g_parse_transformable(r, t, obj);
    uint8_t ren = r_u8(r);
    uint8_t pick = r_u8(r);
    uint8_t alpha = r_u8(r);
    uint32_t scope = r_u32(r);
    uint8_t has_align = r_u8(r);
    if (has_align) {
        (void)r_u8(r); (void)r_u8(r); (void)r_u32(r); (void)r_u32(r);
    }
    if (obj) {
        SET_INT(obj, class_find_field(obj->cls, "renderingEnabled", "Z"), ren ? 1 : 0);
        SET_INT(obj, class_find_field(obj->cls, "pickingEnabled", "Z"), pick ? 1 : 0);
        FIELDS(obj)[class_find_field(obj->cls, "alphaFactor", "F")->offset].f = alpha / 255.0f;
        SET_INT(obj, class_find_field(obj->cls, "scope", "I"), (jint)scope);
    }
}

static Array *m3g_parse_stream(Thread *t, const uint8_t *data, uint32_t len, int offset)
{
    static const uint8_t sig[12] = { 0xAB, 'J', 'S', 'R', '1', '8', '4', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    if (!data || offset < 0 || (uint32_t)offset + 12 > len) return NULL;
    if (memcmp(data + offset, sig, 12) != 0) return NULL;

    Object **objs = (Object **)nj_pal_calloc(M3G_MAX_OBJS, sizeof(Object *));
    if (!objs) return NULL;
    uint32_t n_objs = 1; /* 1-based object indices in M3G */

    M3GReader file_r = { data, len, (uint32_t)offset + 12 };
    while (file_r.pos + 9 <= file_r.len) {
        uint8_t comp_scheme = r_u8(&file_r);
        uint32_t total_sec_len = r_u32(&file_r);
        uint32_t uncomp_len = r_u32(&file_r);
        if (total_sec_len < 13 || file_r.pos + (total_sec_len - 9) > file_r.len) break;
        uint32_t payload_len = total_sec_len - 13;
        const uint8_t *payload = file_r.buf + file_r.pos;
        file_r.pos += total_sec_len - 9; /* skip payload + 4-byte Adler32 */

        uint8_t *uncomp_buf = NULL;
        M3GReader sec_r;
        if (comp_scheme == 0) {
            sec_r.buf = payload;
            sec_r.len = payload_len;
            sec_r.pos = 0;
        } else if (comp_scheme == 1 && uncomp_len > 0 && uncomp_len <= (4u << 20)) {
            uncomp_buf = (uint8_t *)nj_pal_malloc(uncomp_len);
            if (!uncomp_buf) continue;
            int32_t dec = nj_zlib_inflate(uncomp_buf, uncomp_len, payload, payload_len);
            if (dec < 0) { nj_pal_free(uncomp_buf); continue; }
            sec_r.buf = uncomp_buf;
            sec_r.len = (uint32_t)dec;
            sec_r.pos = 0;
        } else {
            continue;
        }

        while (sec_r.pos + 5 <= sec_r.len && n_objs < M3G_MAX_OBJS) {
            uint8_t obj_type = r_u8(&sec_r);
            uint32_t obj_len = r_u32(&sec_r);
            uint32_t next_obj_pos = sec_r.pos + obj_len;
            if (next_obj_pos > sec_r.len) break;

            M3GReader or = { sec_r.buf, next_obj_pos, sec_r.pos };
            Object *created = NULL;

            switch (obj_type) {
            case 0: /* Header */
                break;
            case 3: { /* Appearance */
                created = vm_new_object(t, class_load("javax/microedition/m3g/Appearance"));
                m3g_parse_object3d_base(&or, created);
                int8_t layer = (int8_t)r_u8(&or);
                uint32_t comp_idx = r_u32(&or);
                uint32_t fog_idx  = r_u32(&or);
                uint32_t poly_idx = r_u32(&or);
                uint32_t mat_idx  = r_u32(&or);
                uint32_t n_tex    = r_u32(&or);
                if (created) {
                    SET_INT(created, class_find_field(created->cls, "layer", "I"), layer);
                    if (comp_idx < n_objs) SET_REF(created, class_find_field(created->cls, "compositingMode", "Ljavax/microedition/m3g/CompositingMode;"), objs[comp_idx]);
                    if (fog_idx < n_objs)  SET_REF(created, class_find_field(created->cls, "fog", "Ljavax/microedition/m3g/Fog;"), objs[fog_idx]);
                    if (poly_idx < n_objs) SET_REF(created, class_find_field(created->cls, "polygonMode", "Ljavax/microedition/m3g/PolygonMode;"), objs[poly_idx]);
                    if (mat_idx < n_objs)  SET_REF(created, class_find_field(created->cls, "material", "Ljavax/microedition/m3g/Material;"), objs[mat_idx]);
                    Array *texs = vm_new_array(t, class_array_of(class_load("javax/microedition/m3g/Texture2D")), 4);
                    if (texs) {
                        SET_REF(created, class_find_field(created->cls, "textures", "[Ljavax/microedition/m3g/Texture2D;"), (Object *)texs);
                        for (uint32_t u = 0; u < n_tex; u++) {
                            uint32_t tidx = r_u32(&or);
                            if (u < 4 && tidx < n_objs) ARRAY_DATA(texs, Object *)[u] = objs[tidx];
                        }
                    }
                }
                break;
            }
            case 4: { /* Background */
                created = vm_new_object(t, class_load("javax/microedition/m3g/Background"));
                m3g_parse_object3d_base(&or, created);
                uint32_t col = r_rgba(&or);
                uint32_t img_idx = r_u32(&or);
                (void)r_u8(&or); (void)r_u8(&or);
                (void)r_u32(&or); (void)r_u32(&or); (void)r_u32(&or); (void)r_u32(&or);
                uint8_t c_en = r_u8(&or), d_en = r_u8(&or);
                if (created) {
                    SET_INT(created, class_find_field(created->cls, "color", "I"), (jint)col);
                    if (img_idx < n_objs) SET_REF(created, class_find_field(created->cls, "image", "Ljavax/microedition/m3g/Image2D;"), objs[img_idx]);
                    SET_INT(created, class_find_field(created->cls, "colorClearEnabled", "Z"), c_en ? 1 : 0);
                    SET_INT(created, class_find_field(created->cls, "depthClearEnabled", "Z"), d_en ? 1 : 0);
                }
                break;
            }
            case 5: { /* Camera */
                created = vm_new_object(t, class_load("javax/microedition/m3g/Camera"));
                m3g_parse_node(&or, t, created);
                uint8_t ptype = r_u8(&or);
                if (created) {
                    if (ptype == 48) {
                        float mat[16];
                        for (int row = 0; row < 4; row++)
                            for (int col = 0; col < 4; col++)
                                mat[col * 4 + row] = r_f32(&or);
                        Array *pm = vm_new_prim_array(t, 'F', 16);
                        if (pm) {
                            memcpy(ARRAY_DATA(pm, float), mat, 16 * sizeof(float));
                            SET_REF(created, class_find_field(created->cls, "projMatrix", "[F"), (Object *)pm);
                        }
                    } else {
                        float fovy = r_f32(&or), aspect = r_f32(&or), near = r_f32(&or), far = r_f32(&or);
                        Slot ca[5]; ca[0].ref = created; ca[1].f = fovy; ca[2].f = aspect; ca[3].f = near; ca[4].f = far;
                        Slot cr;
                        if (ptype == 49) M3G_Camera_setParallel(t, ca, &cr);
                        else M3G_Camera_setPerspective(t, ca, &cr);
                    }
                }
                break;
            }
            case 6: { /* CompositingMode */
                created = vm_new_object(t, class_load("javax/microedition/m3g/CompositingMode"));
                m3g_parse_object3d_base(&or, created);
                uint8_t dt = r_u8(&or), dw = r_u8(&or), cw = r_u8(&or), aw = r_u8(&or);
                (void)aw;
                uint8_t bl = r_u8(&or);
                float ath = r_f32(&or);
                if (created) {
                    SET_INT(created, class_find_field(created->cls, "depthTest", "Z"), dt ? 1 : 0);
                    SET_INT(created, class_find_field(created->cls, "depthWrite", "Z"), dw ? 1 : 0);
                    SET_INT(created, class_find_field(created->cls, "colorWrite", "Z"), cw ? 1 : 0);
                    SET_INT(created, class_find_field(created->cls, "blending", "I"), bl);
                    FIELDS(created)[class_find_field(created->cls, "alphaThreshold", "F")->offset].f = ath;
                }
                break;
            }
            case 9: { /* Group */
                created = vm_new_object(t, class_load("javax/microedition/m3g/Group"));
                m3g_parse_node(&or, t, created);
                if (created) {
                    Object *vec = vm_new_object(t, class_load("java/util/Vector"));
                    Array *ed = vm_new_array(t, class_array_of(class_load("java/lang/Object")), 16);
                    if (vec && ed) {
                        SET_REF(vec, class_find_field(vec->cls, "elementData", "[Ljava/lang/Object;"), (Object *)ed);
                        SET_REF(created, class_find_field(created->cls, "children", "Ljava/util/Vector;"), vec);
                    }
                    uint32_t n_ch = r_u32(&or);
                    for (uint32_t c = 0; c < n_ch; c++) {
                        uint32_t cidx = r_u32(&or);
                        if (cidx < n_objs && objs[cidx]) {
                            Slot ga[2]; ga[0].ref = created; ga[1].ref = objs[cidx];
                            Slot gr;
                            M3G_Group_addChild(t, ga, &gr);
                        }
                    }
                }
                break;
            }
            case 10: { /* Image2D */
                created = vm_new_object(t, class_load("javax/microedition/m3g/Image2D"));
                m3g_parse_object3d_base(&or, created);
                uint8_t fmt = r_u8(&or);
                uint8_t is_mut = r_u8(&or);
                uint32_t w = r_u32(&or), h = r_u32(&or);
                if (created && !is_mut && w > 0 && h > 0) {
                    uint32_t pal_len = r_u32(&or);
                    const uint8_t *pal_ptr = or.buf + or.pos;
                    or.pos += pal_len;
                    uint32_t pix_len = r_u32(&or);
                    const uint8_t *pix_ptr = or.buf + or.pos;
                    if (pal_len > 0) {
                        Array *pa = vm_new_prim_array(t, 'B', (jint)pal_len);
                        Array *ia = vm_new_prim_array(t, 'B', (jint)pix_len);
                        if (pa && ia) {
                            memcpy(ARRAY_DATA(pa, uint8_t), pal_ptr, pal_len);
                            memcpy(ARRAY_DATA(ia, uint8_t), pix_ptr, pix_len);
                            Slot ia_args[6];
                            ia_args[0].ref = created; ia_args[1].i = fmt; ia_args[2].i = (jint)w; ia_args[3].i = (jint)h;
                            ia_args[4].ref = (Object *)ia; ia_args[5].ref = (Object *)pa;
                            Slot ir;
                            M3G_Image2D_initPal(t, ia_args, &ir);
                        }
                    } else if (pix_len > 0) {
                        Array *ia = vm_new_prim_array(t, 'B', (jint)pix_len);
                        if (ia) {
                            memcpy(ARRAY_DATA(ia, uint8_t), pix_ptr, pix_len);
                            Slot ia_args[5];
                            ia_args[0].ref = created; ia_args[1].i = fmt; ia_args[2].i = (jint)w; ia_args[3].i = (jint)h;
                            ia_args[4].ref = (Object *)ia;
                            Slot ir;
                            M3G_Image2D_initBytes(t, ia_args, &ir);
                        }
                    }
                }
                break;
            }
            case 11: { /* TriangleStripArray */
                created = vm_new_object(t, class_load("javax/microedition/m3g/TriangleStripArray"));
                m3g_parse_object3d_base(&or, created);
                uint8_t enc = r_u8(&or);
                if (created) {
                    if (enc == 0 || enc == 1 || enc == 2) {
                        int32_t start = (enc == 0) ? (int32_t)r_u32(&or) : ((enc == 1) ? r_u8(&or) : r_u16(&or));
                        uint32_t n_strips = r_u32(&or);
                        Array *la = vm_new_prim_array(t, 'I', (jint)n_strips);
                        if (la) {
                            int32_t *ld = ARRAY_DATA(la, int32_t);
                            for (uint32_t s = 0; s < n_strips; s++) ld[s] = (int32_t)r_u32(&or);
                            Slot ta[3]; ta[0].ref = created; ta[1].i = start; ta[2].ref = (Object *)la;
                            Slot tr;
                            M3G_TriangleStripArray_initImplicit(t, ta, &tr);
                        }
                    } else {
                        uint32_t n_idx = r_u32(&or);
                        Array *ia = vm_new_prim_array(t, 'I', (jint)n_idx);
                        if (ia) {
                            int32_t *id = ARRAY_DATA(ia, int32_t);
                            for (uint32_t k = 0; k < n_idx; k++) {
                                id[k] = (enc == 128) ? (int32_t)r_u32(&or) : ((enc == 129) ? r_u8(&or) : r_u16(&or));
                            }
                            uint32_t n_strips = r_u32(&or);
                            Array *la = vm_new_prim_array(t, 'I', (jint)n_strips);
                            if (la) {
                                int32_t *ld = ARRAY_DATA(la, int32_t);
                                for (uint32_t s = 0; s < n_strips; s++) ld[s] = (int32_t)r_u32(&or);
                                Slot ta[3]; ta[0].ref = created; ta[1].ref = (Object *)ia; ta[2].ref = (Object *)la;
                                Slot tr;
                                M3G_TriangleStripArray_initExplicit(t, ta, &tr);
                            }
                        }
                    }
                }
                break;
            }
            case 14: case 16: { /* Mesh or SkinnedMesh */
                created = vm_new_object(t, class_load(obj_type == 16 ? "javax/microedition/m3g/SkinnedMesh" : "javax/microedition/m3g/Mesh"));
                m3g_parse_node(&or, t, created);
                uint32_t vb_idx = r_u32(&or);
                uint32_t n_sub  = r_u32(&or);
                if (created && vb_idx < n_objs) {
                    Array *ibs = vm_new_array(t, class_array_of(class_load("javax/microedition/m3g/IndexBuffer")), (jint)n_sub);
                    Array *aps = vm_new_array(t, class_array_of(class_load("javax/microedition/m3g/Appearance")), (jint)n_sub);
                    for (uint32_t s = 0; s < n_sub; s++) {
                        uint32_t ib_i = r_u32(&or);
                        uint32_t ap_i = r_u32(&or);
                        if (ibs && ib_i < n_objs) ARRAY_DATA(ibs, Object *)[s] = objs[ib_i];
                        if (aps && ap_i < n_objs) ARRAY_DATA(aps, Object *)[s] = objs[ap_i];
                    }
                    Slot ma[4]; ma[0].ref = created; ma[1].ref = objs[vb_idx]; ma[2].ref = (Object *)ibs; ma[3].ref = (Object *)aps;
                    Slot mr;
                    M3G_Mesh_initMulti(t, ma, &mr);
                }
                break;
            }
            case 18: { /* PolygonMode */
                created = vm_new_object(t, class_load("javax/microedition/m3g/PolygonMode"));
                m3g_parse_object3d_base(&or, created);
                uint8_t cull = r_u8(&or), shade = r_u8(&or), wind = r_u8(&or), two_s = r_u8(&or);
                if (created) {
                    SET_INT(created, class_find_field(created->cls, "culling", "I"), cull);
                    SET_INT(created, class_find_field(created->cls, "shading", "I"), shade);
                    SET_INT(created, class_find_field(created->cls, "winding", "I"), wind);
                    SET_INT(created, class_find_field(created->cls, "twoSidedLighting", "Z"), two_s ? 1 : 0);
                }
                break;
            }
            case 20: { /* Texture2D */
                created = vm_new_object(t, class_load("javax/microedition/m3g/Texture2D"));
                m3g_parse_transformable(&or, t, created);
                uint32_t img_idx = r_u32(&or);
                uint32_t bcol = r_rgba(&or);
                uint8_t blend = r_u8(&or), ws = r_u8(&or), wt = r_u8(&or);
                if (created) {
                    if (img_idx < n_objs) SET_REF(created, class_find_field(created->cls, "image", "Ljavax/microedition/m3g/Image2D;"), objs[img_idx]);
                    SET_INT(created, class_find_field(created->cls, "blendColor", "I"), (jint)bcol);
                    SET_INT(created, class_find_field(created->cls, "blending", "I"), blend);
                    SET_INT(created, class_find_field(created->cls, "wrapS", "I"), ws);
                    SET_INT(created, class_find_field(created->cls, "wrapT", "I"), wt);
                }
                break;
            }
            case 21: { /* VertexArray */
                created = vm_new_object(t, class_load("javax/microedition/m3g/VertexArray"));
                m3g_parse_object3d_base(&or, created);
                uint8_t csize = r_u8(&or);
                uint8_t ccnt  = r_u8(&or);
                uint8_t enc   = r_u8(&or);
                uint16_t vcnt = r_u16(&or);
                if (created && vcnt > 0 && ccnt > 0 && ccnt <= 4) {
                    Slot va[4]; va[0].ref = created; va[1].i = vcnt; va[2].i = ccnt; va[3].i = csize;
                    Slot vr;
                    M3G_VertexArray_init(t, va, &vr);
                    if (csize == 1) {
                        Object *bd = GET_REF(created, class_find_field(created->cls, "byteData", "[B"));
                        int8_t *dst = bd ? ARRAY_DATA(bd, int8_t) : NULL;
                        int8_t acc[4] = {0, 0, 0, 0};
                        for (uint16_t v = 0; v < vcnt; v++) {
                            for (uint8_t c = 0; c < ccnt; c++) {
                                int8_t val = (int8_t)r_u8(&or);
                                if (enc == 1) { acc[c] = (int8_t)(acc[c] + val); val = acc[c]; }
                                if (dst) dst[v * ccnt + c] = val;
                            }
                        }
                    } else {
                        Object *sd = GET_REF(created, class_find_field(created->cls, "shortData", "[S"));
                        int16_t *dst = sd ? ARRAY_DATA(sd, int16_t) : NULL;
                        int16_t acc[4] = {0, 0, 0, 0};
                        for (uint16_t v = 0; v < vcnt; v++) {
                            for (uint8_t c = 0; c < ccnt; c++) {
                                int16_t val = (int16_t)r_u16(&or);
                                if (enc == 1) { acc[c] = (int16_t)(acc[c] + val); val = acc[c]; }
                                if (dst) dst[v * ccnt + c] = val;
                            }
                        }
                    }
                }
                break;
            }
            case 22: { /* VertexBuffer */
                created = vm_new_object(t, class_load("javax/microedition/m3g/VertexBuffer"));
                m3g_parse_object3d_base(&or, created);
                if (created) {
                    Slot vba[1]; vba[0].ref = created; Slot vbr;
                    M3G_VertexBuffer_init(t, vba, &vbr);
                    uint32_t dcol = r_rgba(&or);
                    uint32_t pos_i = r_u32(&or);
                    float pb[3]; pb[0] = r_f32(&or); pb[1] = r_f32(&or); pb[2] = r_f32(&or);
                    float psc = r_f32(&or);
                    uint32_t norm_i = r_u32(&or);
                    uint32_t col_i  = r_u32(&or);
                    uint32_t n_tc   = r_u32(&or);
                    SET_INT(created, class_find_field(created->cls, "defaultColor", "I"), (jint)dcol);
                    if (pos_i < n_objs && objs[pos_i]) {
                        SET_REF(created, class_find_field(created->cls, "positions", "Ljavax/microedition/m3g/VertexArray;"), objs[pos_i]);
                        FIELDS(created)[class_find_field(created->cls, "posScale", "F")->offset].f = psc;
                        Object *pbo = GET_REF(created, class_find_field(created->cls, "posBias", "[F"));
                        if (pbo && ARRAY_LEN(pbo) >= 3) memcpy(ARRAY_DATA(pbo, float), pb, 3 * sizeof(float));
                    }
                    if (norm_i < n_objs) SET_REF(created, class_find_field(created->cls, "normals", "Ljavax/microedition/m3g/VertexArray;"), objs[norm_i]);
                    if (col_i < n_objs)  SET_REF(created, class_find_field(created->cls, "colors", "Ljavax/microedition/m3g/VertexArray;"), objs[col_i]);
                    for (uint32_t u = 0; u < n_tc; u++) {
                        uint32_t tci = r_u32(&or);
                        float tb0 = r_f32(&or), tb1 = r_f32(&or), tb2 = r_f32(&or);
                        (void)tb0; (void)tb1; (void)tb2;
                        float tsc = r_f32(&or);
                        if (u < 2 && tci < n_objs && objs[tci]) {
                            Slot tca[4]; tca[0].ref = created; tca[1].i = (jint)u; tca[2].ref = objs[tci]; tca[3].f = tsc;
                            M3G_VertexBuffer_setTexCoords(t, tca, &vbr);
                        }
                    }
                }
                break;
            }
            case 23: { /* World */
                created = vm_new_object(t, class_load("javax/microedition/m3g/World"));
                m3g_parse_node(&or, t, created);
                if (created) {
                    Object *vec = vm_new_object(t, class_load("java/util/Vector"));
                    Array *ed = vm_new_array(t, class_array_of(class_load("java/lang/Object")), 16);
                    if (vec && ed) {
                        SET_REF(vec, class_find_field(vec->cls, "elementData", "[Ljava/lang/Object;"), (Object *)ed);
                        SET_REF(created, class_find_field(created->cls, "children", "Ljava/util/Vector;"), vec);
                    }
                    uint32_t n_ch = r_u32(&or);
                    for (uint32_t c = 0; c < n_ch; c++) {
                        uint32_t cidx = r_u32(&or);
                        if (cidx < n_objs && objs[cidx]) {
                            Slot ga[2]; ga[0].ref = created; ga[1].ref = objs[cidx];
                            Slot gr;
                            M3G_Group_addChild(t, ga, &gr);
                        }
                    }
                    uint32_t cam_i = r_u32(&or);
                    uint32_t bg_i  = r_u32(&or);
                    if (cam_i < n_objs) SET_REF(created, class_find_field(created->cls, "activeCamera", "Ljavax/microedition/m3g/Camera;"), objs[cam_i]);
                    if (bg_i < n_objs)  SET_REF(created, class_find_field(created->cls, "background", "Ljavax/microedition/m3g/Background;"), objs[bg_i]);
                }
                break;
            }
            case 255: /* External Reference */
                r_skip_str(&or);
                break;
            default:
                break;
            }

            objs[n_objs++] = created;
            sec_r.pos = next_obj_pos;
        }
        if (uncomp_buf) nj_pal_free(uncomp_buf);
    }

    /* Collect top-level root Object3D instances (or World if present) */
    Object *roots[64];
    int n_roots = 0;
    Class *c_world = class_load("javax/microedition/m3g/World");
    Class *c_node  = class_load("javax/microedition/m3g/Node");
    for (uint32_t i = 1; i < n_objs && n_roots < 64; i++) {
        Object *o = objs[i];
        if (!o) continue;
        if (o->cls == c_world) {
            roots[n_roots++] = o;
        } else if (class_is_subclass(o->cls, c_node)) {
            if (!GET_REF(o, class_find_field(o->cls, "parent", "Ljavax/microedition/m3g/Node;")))
                roots[n_roots++] = o;
        }
    }
    if (n_roots == 0) {
        for (uint32_t i = 1; i < n_objs && n_roots < 64; i++) {
            if (objs[i]) roots[n_roots++] = objs[i];
        }
    }
    if (n_roots == 0) {
        Object *w = vm_new_object(t, c_world);
        if (w) roots[n_roots++] = w;
    }

    Class *c_obj3d = class_load("javax/microedition/m3g/Object3D");
    Array *res = vm_new_array(t, class_array_of(c_obj3d), n_roots);
    if (res) {
        for (int i = 0; i < n_roots; i++) ARRAY_DATA(res, Object *)[i] = roots[i];
    }
    nj_pal_free(objs);
    return res;
}

static int M3G_Loader_loadRes(Thread *t, Slot *args, Slot *ret)
{
    Object *name = A_REF(0);
    if (!name) { NPE(t); return NATIVE_OK; }
    char path[256];
    str_to_utf8(name, path, sizeof(path));
    uint32_t len = 0;
    uint8_t *raw = res_read_app(path, &len);
    if (!raw) {
        vm_throw_newf(t, "java/io/IOException", "M3G file not found: %s", path);
        return NATIVE_OK;
    }
    Array *res = m3g_parse_stream(t, raw, len, 0);
    nj_pal_free(raw);
    if (!res && !t->exception) {
        vm_throw_newf(t, "java/io/IOException", "Invalid M3G stream: %s", path);
        return NATIVE_OK;
    }
    ret->ref = (Object *)res;
    return NATIVE_OK;
}

static int M3G_Loader_loadBytes(Thread *t, Slot *args, Slot *ret)
{
    Object *arr = A_REF(0);
    int offset = A_INT(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    Array *res = m3g_parse_stream(t, ARRAY_DATA(arr, uint8_t), (uint32_t)ARRAY_LEN(arr), offset);
    if (!res && !t->exception) {
        vm_throw_new(t, "java/io/IOException", "Invalid M3G byte stream");
        return NATIVE_OK;
    }
    ret->ref = (Object *)res;
    return NATIVE_OK;
}

/* ------------------------------------------------------------------------ */
/* MascotCapsule Micro3D v3 Complete Runtime & Command-List Renderer        */
/* ------------------------------------------------------------------------ */
static int MC_Util3D_sqrt(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    int32_t x = A_INT(0);
    ret->i = x <= 0 ? 0 : (int32_t)sqrtf((float)x);
    return NATIVE_OK;
}

static int MC_Util3D_sin(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    float rad = (float)A_INT(0) * (6.283185307179586f / 4096.0f);
    ret->i = (int32_t)(sinf(rad) * 4096.0f);
    return NATIVE_OK;
}

static int MC_Util3D_cos(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    float rad = (float)A_INT(0) * (6.283185307179586f / 4096.0f);
    ret->i = (int32_t)(cosf(rad) * 4096.0f);
    return NATIVE_OK;
}

/* AffineTrans 3x4 4.12 fixed-point matrix */
static void mc_affine_get(Object *at, int32_t m[12])
{
    static const char *names[12] = {"m00","m01","m02","m03","m10","m11","m12","m13","m20","m21","m22","m23"};
    memset(m, 0, 12 * sizeof(int32_t));
    m[0] = m[5] = m[10] = 4096;
    if (!at || !at->cls) return;
    for (int i = 0; i < 12; i++) {
        Field *f = class_find_field(at->cls, names[i], "I");
        if (f) m[i] = GET_INT(at, f);
    }
}

static void mc_affine_set(Object *at, const int32_t m[12])
{
    static const char *names[12] = {"m00","m01","m02","m03","m10","m11","m12","m13","m20","m21","m22","m23"};
    if (!at || !at->cls) return;
    for (int i = 0; i < 12; i++) {
        Field *f = class_find_field(at->cls, names[i], "I");
        if (f) SET_INT(at, f, m[i]);
    }
}

static int MC_AffineTrans_setIdentity(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    int32_t m[12] = {4096,0,0,0, 0,4096,0,0, 0,0,4096,0};
    mc_affine_set(A_REF(0), m);
    return NATIVE_OK;
}

static int MC_AffineTrans_init12(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    int32_t m[12];
    for (int i = 0; i < 12; i++) m[i] = A_INT(i + 1);
    mc_affine_set(A_REF(0), m);
    return NATIVE_OK;
}

static int MC_AffineTrans_initArr(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    if (ARRAY_LEN(arr) >= 12) mc_affine_set(A_REF(0), ARRAY_DATA(arr, int32_t));
    return NATIVE_OK;
}

static int MC_AffineTrans_set(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *src = A_REF(1);
    if (!src) { NPE(t); return NATIVE_OK; }
    int32_t m[12];
    mc_affine_get(src, m);
    mc_affine_set(A_REF(0), m);
    return NATIVE_OK;
}

static int MC_AffineTrans_get(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *arr = A_REF(1);
    if (!arr) { NPE(t); return NATIVE_OK; }
    if (ARRAY_LEN(arr) >= 12) {
        int32_t m[12];
        mc_affine_get(A_REF(0), m);
        memcpy(ARRAY_DATA(arr, int32_t), m, 12 * sizeof(int32_t));
    }
    return NATIVE_OK;
}

static void mc_mul_raw(int32_t out[12], const int32_t a[12], const int32_t b[12])
{
    int32_t r[12];
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            r[row*4 + col] = (int32_t)(((int64_t)a[row*4+0]*b[0*4+col] +
                                        (int64_t)a[row*4+1]*b[1*4+col] +
                                        (int64_t)a[row*4+2]*b[2*4+col]) >> 12);
        }
        r[row*4 + 3] = (int32_t)(((int64_t)a[row*4+0]*b[3] +
                                  (int64_t)a[row*4+1]*b[7] +
                                  (int64_t)a[row*4+2]*b[11]) >> 12) + a[row*4 + 3];
    }
    memcpy(out, r, sizeof(r));
}

static int MC_AffineTrans_mul1(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    if (!A_REF(1)) { NPE(t); return NATIVE_OK; }
    int32_t a[12], b[12], r[12];
    mc_affine_get(A_REF(0), a);
    mc_affine_get(A_REF(1), b);
    mc_mul_raw(r, a, b);
    mc_affine_set(A_REF(0), r);
    return NATIVE_OK;
}

static int MC_AffineTrans_mul2(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    if (!A_REF(1) || !A_REF(2)) { NPE(t); return NATIVE_OK; }
    int32_t a[12], b[12], r[12];
    mc_affine_get(A_REF(1), a);
    mc_affine_get(A_REF(2), b);
    mc_mul_raw(r, a, b);
    mc_affine_set(A_REF(0), r);
    return NATIVE_OK;
}

static int MC_AffineTrans_rotX(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    float rad = (float)A_INT(1) * (6.283185307179586f / 4096.0f);
    int32_t s = (int32_t)(sinf(rad) * 4096.0f), c = (int32_t)(cosf(rad) * 4096.0f);
    int32_t m[12] = {4096,0,0,0, 0,c,-s,0, 0,s,c,0};
    mc_affine_set(A_REF(0), m);
    return NATIVE_OK;
}

static int MC_AffineTrans_rotY(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    float rad = (float)A_INT(1) * (6.283185307179586f / 4096.0f);
    int32_t s = (int32_t)(sinf(rad) * 4096.0f), c = (int32_t)(cosf(rad) * 4096.0f);
    int32_t m[12] = {c,0,s,0, 0,4096,0,0, -s,0,c,0};
    mc_affine_set(A_REF(0), m);
    return NATIVE_OK;
}

static int MC_AffineTrans_rotZ(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    float rad = (float)A_INT(1) * (6.283185307179586f / 4096.0f);
    int32_t s = (int32_t)(sinf(rad) * 4096.0f), c = (int32_t)(cosf(rad) * 4096.0f);
    int32_t m[12] = {c,-s,0,0, s,c,0,0, 0,0,4096,0};
    mc_affine_set(A_REF(0), m);
    return NATIVE_OK;
}

static int MC_AffineTrans_setViewTrans(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *pos = A_REF(1), *look = A_REF(2), *up = A_REF(3);
    if (!pos || !look || !up) { NPE(t); return NATIVE_OK; }
    float px = (float)GET_INT(pos, class_find_field(pos->cls, "x", "I"));
    float py = (float)GET_INT(pos, class_find_field(pos->cls, "y", "I"));
    float pz = (float)GET_INT(pos, class_find_field(pos->cls, "z", "I"));
    float zx = (float)GET_INT(look, class_find_field(look->cls, "x", "I"));
    float zy = (float)GET_INT(look, class_find_field(look->cls, "y", "I"));
    float zz = (float)GET_INT(look, class_find_field(look->cls, "z", "I"));
    float ux = (float)GET_INT(up, class_find_field(up->cls, "x", "I"));
    float uy = (float)GET_INT(up, class_find_field(up->cls, "y", "I"));
    float uz = (float)GET_INT(up, class_find_field(up->cls, "z", "I"));
    float zl = sqrtf(zx*zx + zy*zy + zz*zz);
    if (zl > 1e-5f) { zx /= zl; zy /= zl; zz /= zl; }
    float xx = uy*zz - uz*zy, xy = uz*zx - ux*zz, xz = ux*zy - uy*zx;
    float xl = sqrtf(xx*xx + xy*xy + xz*xz);
    if (xl > 1e-5f) { xx /= xl; xy /= xl; xz /= xl; }
    float yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
    int32_t m[12];
    m[0] = (int32_t)(xx * 4096); m[1] = (int32_t)(xy * 4096); m[2] = (int32_t)(xz * 4096);
    m[3] = -(int32_t)(xx*px + xy*py + xz*pz);
    m[4] = (int32_t)(yx * 4096); m[5] = (int32_t)(yy * 4096); m[6] = (int32_t)(yz * 4096);
    m[7] = -(int32_t)(yx*px + yy*py + yz*pz);
    m[8] = (int32_t)(zx * 4096); m[9] = (int32_t)(zy * 4096); m[10] = (int32_t)(zz * 4096);
    m[11] = -(int32_t)(zx*px + zy*py + zz*pz);
    mc_affine_set(A_REF(0), m);
    return NATIVE_OK;
}

static int MC_AffineTrans_transform(Thread *t, Slot *args, Slot *ret)
{
    Object *v = A_REF(1);
    if (!v) { NPE(t); return NATIVE_OK; }
    int32_t m[12];
    mc_affine_get(A_REF(0), m);
    int32_t x = GET_INT(v, class_find_field(v->cls, "x", "I"));
    int32_t y = GET_INT(v, class_find_field(v->cls, "y", "I"));
    int32_t z = GET_INT(v, class_find_field(v->cls, "z", "I"));
    Object *out = vm_new_object(t, v->cls);
    if (out) {
        SET_INT(out, class_find_field(out->cls, "x", "I"), (int32_t)(((int64_t)m[0]*x + (int64_t)m[1]*y + (int64_t)m[2]*z) >> 12) + m[3]);
        SET_INT(out, class_find_field(out->cls, "y", "I"), (int32_t)(((int64_t)m[4]*x + (int64_t)m[5]*y + (int64_t)m[6]*z) >> 12) + m[7]);
        SET_INT(out, class_find_field(out->cls, "z", "I"), (int32_t)(((int64_t)m[8]*x + (int64_t)m[9]*y + (int64_t)m[10]*z) >> 12) + m[11]);
    }
    ret->ref = out;
    return NATIVE_OK;
}

/* Texture BMP decoder */
typedef struct {
    uint16_t *px;
    uint8_t  *al;
    int       w;
} McBmpCtx;

static void mc_bmp_row_cb(void *ctx, int y, int x0, int step, const uint32_t *argb, int n)
{
    McBmpCtx *c = (McBmpCtx *)ctx;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * step;
        uint32_t p = argb[i];
        c->px[y * c->w + x] = gfx_pack(p);
        c->al[y * c->w + x] = (uint8_t)(p >> 24);
    }
}

static int MC_Texture_initBytes(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0), *data = A_REF(1);
    if (!data) { NPE(t); return NATIVE_OK; }
    const uint8_t *bytes = ARRAY_DATA(data, uint8_t);
    uint32_t len = (uint32_t)ARRAY_LEN(data);
    BmpInfo binfo;
    if (bmp_is_bmp(bytes, len) && bmp_info(bytes, len, &binfo)) {
        Array *px = vm_new_prim_array(t, 'S', binfo.width * binfo.height);
        Array *al = vm_new_prim_array(t, 'B', binfo.width * binfo.height);
        if (px && al) {
            McBmpCtx ctx = { ARRAY_DATA(px, uint16_t), ARRAY_DATA(al, uint8_t), binfo.width };
            bmp_decode(bytes, len, mc_bmp_row_cb, &ctx);
            SET_INT(self, class_find_field(self->cls, "width", "I"), binfo.width);
            SET_INT(self, class_find_field(self->cls, "height", "I"), binfo.height);
            SET_REF(self, class_find_field(self->cls, "pixels", "[S"), (Object *)px);
            SET_REF(self, class_find_field(self->cls, "alpha", "[B"), (Object *)al);
        }
    }
    return NATIVE_OK;
}

static int MC_Texture_initRes(Thread *t, Slot *args, Slot *ret)
{
    Object *self = A_REF(0), *name = A_REF(1);
    if (!name) { NPE(t); return NATIVE_OK; }
    char path[256];
    str_to_utf8(name, path, sizeof(path));
    uint32_t len = 0;
    uint8_t *raw = res_read_app(path, &len);
    if (raw) {
        Array *arr = vm_new_prim_array(t, 'B', (jint)len);
        if (arr) {
            memcpy(ARRAY_DATA(arr, uint8_t), raw, len);
            Slot sub[3]; sub[0].ref = self; sub[1].ref = (Object *)arr; sub[2].i = A_INT(2);
            MC_Texture_initBytes(t, sub, ret);
        }
        nj_pal_free(raw);
    }
    return NATIVE_OK;
}

/* FigureLayout setters/getters */
static int MC_FigureLayout_setAffineTrans(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "affine", "Lcom/mascotcapsule/micro3d/v3/AffineTrans;"), A_REF(1));
    return NATIVE_OK;
}
static int MC_FigureLayout_getAffineTrans(Thread *t, Slot *args, Slot *ret)
{
    (void)t;
    ret->ref = GET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "affine", "Lcom/mascotcapsule/micro3d/v3/AffineTrans;"));
    return NATIVE_OK;
}
static int MC_FigureLayout_setScale(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "scaleX", "I"), A_INT(1));
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "scaleY", "I"), A_INT(2));
    return NATIVE_OK;
}
static int MC_FigureLayout_setCenter(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "centerX", "I"), A_INT(1));
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "centerY", "I"), A_INT(2));
    return NATIVE_OK;
}
static int MC_FigureLayout_setPerspective(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "projMode", "I"), 2);
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "near", "I"), A_INT(1));
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "far", "I"), A_INT(2));
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "angle", "I"), A_INT(3));
    return NATIVE_OK;
}
static int MC_FigureLayout_setParallel(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "projMode", "I"), 1);
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "scaleX", "I"), A_INT(1));
    SET_INT(A_REF(0), class_find_field(A_REF(0)->cls, "scaleY", "I"), A_INT(2));
    return NATIVE_OK;
}

/* MascotCapsule Graphics3D.renderPrimitives & drawCommandList */
static int MC_G3D_bind(Thread *t, Slot *args, Slot *ret)
{
    (void)ret;
    Object *self = A_REF(0), *g = A_REF(1);
    if (!g) { NPE(t); return NATIVE_OK; }
    SET_REF(self, class_find_field(self->cls, "boundGraphics", "Ljavax/microedition/lcdui/Graphics;"), g);
    bind_graphics_target(g, true);
    s_vp_x = s_clip.x1; s_vp_y = s_clip.y1;
    s_vp_w = s_clip.x2 - s_clip.x1; s_vp_h = s_clip.y2 - s_clip.y1;
    if (s_zbuf) memset(s_zbuf, 0xFF, (size_t)s_zw * (size_t)s_zh * sizeof(uint16_t));
    return NATIVE_OK;
}

static int MC_G3D_release(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    SET_REF(A_REF(0), class_find_field(A_REF(0)->cls, "boundGraphics", "Ljavax/microedition/lcdui/Graphics;"), NULL);
    s_target.px = NULL;
    return NATIVE_OK;
}

static void mc_project_vertex(M3GVertex *out, const int32_t m[12], int32_t vx, int32_t vy, int32_t vz,
                              int cx, int cy, int sx, int sy, int proj_mode, int angle)
{
    int32_t tx = (int32_t)(((int64_t)m[0]*vx + (int64_t)m[1]*vy + (int64_t)m[2]*vz) >> 12) + m[3];
    int32_t ty = (int32_t)(((int64_t)m[4]*vx + (int64_t)m[5]*vy + (int64_t)m[6]*vz) >> 12) + m[7];
    int32_t tz = (int32_t)(((int64_t)m[8]*vx + (int64_t)m[9]*vy + (int64_t)m[10]*vz) >> 12) + m[11];

    if (proj_mode == 2 && tz > 1) {
        float fov_rad = (float)(angle > 0 ? angle : 512) * (6.283185307179586f / 4096.0f);
        float f = (s_vp_h * 0.5f) / tanf(fov_rad * 0.5f);
        out->x = (int32_t)((cx + ((float)tx * f) / (float)tz) * 65536.0f);
        out->y = (int32_t)((cy - ((float)ty * f) / (float)tz) * 65536.0f);
    } else {
        out->x = (int32_t)((cx + ((tx * sx) >> 12)) * 65536);
        out->y = (int32_t)((cy - ((ty * sy) >> 12)) * 65536);
    }
    int32_t zclamp = tz < 0 ? 0 : (tz > 65535 ? 65535 : tz);
    out->z = zclamp << 16;
}

static int MC_G3D_renderPrimitives(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)ret;
    Object *tex = A_REF(1);
    int off_x = A_INT(2), off_y = A_INT(3);
    Object *layout = A_REF(4);
    int command = A_INT(6), num_prims = A_INT(7);
    Object *coords = A_REF(8), *texcoords = A_REF(10), *colors = A_REF(11);
    if (!coords || num_prims <= 0) return NATIVE_OK;

    int32_t m[12] = {4096,0,0,0, 0,4096,0,0, 0,0,4096,0};
    int cx = off_x + s_vp_w / 2, cy = off_y + s_vp_h / 2;
    int sx = 4096, sy = 4096, proj_mode = 0, angle = 512;
    if (layout) {
        Object *at = GET_REF(layout, class_find_field(layout->cls, "affine", "Lcom/mascotcapsule/micro3d/v3/AffineTrans;"));
        if (at) mc_affine_get(at, m);
        cx = off_x + GET_INT(layout, class_find_field(layout->cls, "centerX", "I"));
        cy = off_y + GET_INT(layout, class_find_field(layout->cls, "centerY", "I"));
        int lx = GET_INT(layout, class_find_field(layout->cls, "scaleX", "I"));
        int ly = GET_INT(layout, class_find_field(layout->cls, "scaleY", "I"));
        if (lx) sx = lx;
        if (ly) sy = ly;
        proj_mode = GET_INT(layout, class_find_field(layout->cls, "projMode", "I"));
        angle = GET_INT(layout, class_find_field(layout->cls, "angle", "I"));
    }

    s_tex_px = NULL; s_tex_al = NULL; s_tex_w = 0; s_tex_h = 0;
    if (tex) {
        Object *ipx = GET_REF(tex, class_find_field(tex->cls, "pixels", "[S"));
        Object *ial = GET_REF(tex, class_find_field(tex->cls, "alpha", "[B"));
        if (ipx) {
            s_tex_px = ARRAY_DATA(ipx, uint16_t);
            s_tex_al = ial ? ARRAY_DATA(ial, uint8_t) : NULL;
            s_tex_w = GET_INT(tex, class_find_field(tex->cls, "width", "I"));
            s_tex_h = GET_INT(tex, class_find_field(tex->cls, "height", "I"));
        }
    }

    int prim_type = command & 0x0F000000;
    int blend = (command & 96) == 64 ? 65 : ((command & 96) == 32 ? 64 : 68);
    const int32_t *c_data = ARRAY_DATA(coords, int32_t);
    int c_len = ARRAY_LEN(coords);
    const int32_t *t_data = texcoords ? ARRAY_DATA(texcoords, int32_t) : NULL;
    int t_len = texcoords ? ARRAY_LEN(texcoords) : 0;
    const int32_t *col_data = colors ? ARRAY_DATA(colors, int32_t) : NULL;
    int col_len = colors ? ARRAY_LEN(colors) : 0;

    int verts_per_prim = (prim_type == 0x04000000) ? 4 : ((prim_type == 0x03000000) ? 3 : 2);
    for (int p = 0; p < num_prims; p++) {
        int base = p * verts_per_prim * 3;
        if (base + verts_per_prim * 3 > c_len) break;
        M3GVertex v[4];
        uint32_t pcol = (col_data && p < col_len) ? (0xFF000000u | (uint32_t)col_data[p]) : 0xFFFFFFFFu;
        for (int k = 0; k < verts_per_prim; k++) {
            mc_project_vertex(&v[k], m, c_data[base + k*3], c_data[base + k*3 + 1], c_data[base + k*3 + 2],
                              cx, cy, sx, sy, proj_mode, angle);
            v[k].color = pcol;
            int tbase = (p * verts_per_prim + k) * 2;
            if (t_data && tbase + 1 < t_len) {
                v[k].u = t_data[tbase] << 16;
                v[k].v = t_data[tbase + 1] << 16;
            } else {
                v[k].u = v[k].v = 0;
            }
        }
        if (verts_per_prim == 3) {
            rasterize_tri(&v[0], &v[1], &v[2], blend);
        } else if (verts_per_prim == 4) {
            rasterize_tri(&v[0], &v[1], &v[2], blend);
            rasterize_tri(&v[0], &v[2], &v[3], blend);
        }
    }
    return NATIVE_OK;
}

static int MC_Noop(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)args; (void)ret;
    return NATIVE_OK;
}

static int MC_ReturnZero(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)args;
    ret->i = 0;
    return NATIVE_OK;
}

static int MC_ReturnOne(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)args;
    ret->i = 1;
    return NATIVE_OK;
}

static int MC_ReturnFloatZero(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)args;
    ret->f = 0.0f;
    return NATIVE_OK;
}

static int MC_ReturnFloatOne(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)args;
    ret->f = 1.0f;
    return NATIVE_OK;
}

static int MC_ReturnNull(Thread *t, Slot *args, Slot *ret)
{
    (void)t; (void)args;
    ret->ref = NULL;
    return NATIVE_OK;
}

static const NativeEntry m3g_natives[] = {
    /* M3G Object3D */
    {"javax/microedition/m3g/Object3D", "duplicate", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/Object3D", "getReferences", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Object3D", "find", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/Object3D", "addAnimationTrack", NULL, MC_Noop},
    {"javax/microedition/m3g/Object3D", "removeAnimationTrack", NULL, MC_Noop},
    {"javax/microedition/m3g/Object3D", "getAnimationTrackCount", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Object3D", "getAnimationTrack", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/Object3D", "animate", NULL, MC_ReturnZero},

    /* M3G Transform */
    {"javax/microedition/m3g/Transform", "setIdentity", "()V", M3G_Transform_setIdentity},
    {"javax/microedition/m3g/Transform", "set", "([F)V", M3G_Transform_setArray},
    {"javax/microedition/m3g/Transform", "set", "(Ljavax/microedition/m3g/Transform;)V", M3G_Transform_setTransform},
    {"javax/microedition/m3g/Transform", "get", "([F)V", M3G_Transform_get},
    {"javax/microedition/m3g/Transform", "invert", "()V", M3G_Transform_invert},
    {"javax/microedition/m3g/Transform", "transpose", "()V", M3G_Transform_transpose},
    {"javax/microedition/m3g/Transform", "postMultiply", "(Ljavax/microedition/m3g/Transform;)V", M3G_Transform_postMultiply},
    {"javax/microedition/m3g/Transform", "postScale", "(FFF)V", M3G_Transform_postScale},
    {"javax/microedition/m3g/Transform", "postRotate", "(FFFF)V", M3G_Transform_postRotate},
    {"javax/microedition/m3g/Transform", "postRotateQuat", "(FFFF)V", M3G_Transform_postRotateQuat},
    {"javax/microedition/m3g/Transform", "postTranslate", "(FFF)V", M3G_Transform_postTranslate},
    {"javax/microedition/m3g/Transform", "transform", "([F)V", M3G_Transform_transformFloats},
    {"javax/microedition/m3g/Transform", "transform", "(Ljavax/microedition/m3g/VertexArray;[FZ)V", MC_Noop},

    /* M3G Transformable & Node & Group */
    {"javax/microedition/m3g/Transformable", "initDefaults", "()V", M3G_Transformable_initDefaults},
    {"javax/microedition/m3g/Transformable", "setTranslation", "(FFF)V", M3G_Transformable_setTranslation},
    {"javax/microedition/m3g/Transformable", "translate", "(FFF)V", M3G_Transformable_translate},
    {"javax/microedition/m3g/Transformable", "getTranslation", "([F)V", M3G_Transformable_getTranslation},
    {"javax/microedition/m3g/Transformable", "setScale", "(FFF)V", M3G_Transformable_setScale},
    {"javax/microedition/m3g/Transformable", "scale", "(FFF)V", M3G_Transformable_setScale},
    {"javax/microedition/m3g/Transformable", "getScale", "([F)V", M3G_Transformable_getScale},
    {"javax/microedition/m3g/Transformable", "setOrientation", "(FFFF)V", M3G_Transformable_setOrientation},
    {"javax/microedition/m3g/Transformable", "getOrientation", "([F)V", M3G_Transformable_getOrientation},
    {"javax/microedition/m3g/Transformable", "postRotate", "(FFFF)V", M3G_Transformable_postRotate},
    {"javax/microedition/m3g/Transformable", "preRotate", "(FFFF)V", M3G_Transformable_postRotate},
    {"javax/microedition/m3g/Transformable", "setTransform", "(Ljavax/microedition/m3g/Transform;)V", M3G_Transformable_setTransform},
    {"javax/microedition/m3g/Transformable", "getTransform", "(Ljavax/microedition/m3g/Transform;)V", M3G_Transformable_getTransform},
    {"javax/microedition/m3g/Transformable", "getCompositeTransform", "(Ljavax/microedition/m3g/Transform;)V", M3G_Transformable_getCompositeTransform},

    {"javax/microedition/m3g/Node", "getTransformTo", NULL, MC_ReturnOne},
    {"javax/microedition/m3g/Node", "setAlignment", NULL, MC_Noop},
    {"javax/microedition/m3g/Node", "align", NULL, MC_Noop},
    {"javax/microedition/m3g/Node", "setAlphaFactor", NULL, M3G_Node_setAlphaFactor},
    {"javax/microedition/m3g/Node", "getAlphaFactor", NULL, M3G_Node_getAlphaFactor},
    {"javax/microedition/m3g/Node", "setRenderingEnable", NULL, M3G_Node_setRenderingEnable},
    {"javax/microedition/m3g/Node", "isRenderingEnabled", NULL, M3G_Node_isRenderingEnabled},
    {"javax/microedition/m3g/Node", "setPickingEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/Node", "isPickingEnabled", NULL, MC_ReturnOne},
    {"javax/microedition/m3g/Node", "setScope", NULL, MC_Noop},
    {"javax/microedition/m3g/Node", "getScope", NULL, MC_ReturnZero},

    {"javax/microedition/m3g/Group", "addChild", NULL, M3G_Group_addChild},
    {"javax/microedition/m3g/Group", "removeChild", NULL, MC_Noop},
    {"javax/microedition/m3g/Group", "getChildCount", NULL, M3G_Group_getChildCount},
    {"javax/microedition/m3g/Group", "getChild", NULL, M3G_Group_getChild},
    {"javax/microedition/m3g/Group", "pick", NULL, MC_ReturnZero},

    /* M3G Camera & Background & Image2D & Texture2D */
    {"javax/microedition/m3g/Camera", "setPerspective", "(FFFF)V", M3G_Camera_setPerspective},
    {"javax/microedition/m3g/Camera", "setParallel", "(FFFF)V", M3G_Camera_setParallel},
    {"javax/microedition/m3g/Camera", "setGeneric", "(Ljavax/microedition/m3g/Transform;)V", M3G_Camera_setGeneric},
    {"javax/microedition/m3g/Camera", "getProjection", "(Ljavax/microedition/m3g/Transform;)I", M3G_Camera_getProjectionTransform},
    {"javax/microedition/m3g/Camera", "getProjection", "([F)I", MC_ReturnZero},

    {"javax/microedition/m3g/Background", "setColor", NULL, M3G_Background_setColor},
    {"javax/microedition/m3g/Background", "getColor", NULL, M3G_Background_getColor},
    {"javax/microedition/m3g/Background", "setImage", NULL, M3G_Background_setImage},
    {"javax/microedition/m3g/Background", "getImage", NULL, M3G_Background_getImage},
    {"javax/microedition/m3g/Background", "setImageMode", NULL, MC_Noop},
    {"javax/microedition/m3g/Background", "getImageModeX", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Background", "getImageModeY", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Background", "setColorClearEnable", NULL, M3G_Background_setColorClearEnable},
    {"javax/microedition/m3g/Background", "isColorClearEnabled", NULL, M3G_Background_isColorClearEnabled},
    {"javax/microedition/m3g/Background", "setDepthClearEnable", NULL, M3G_Background_setDepthClearEnable},
    {"javax/microedition/m3g/Background", "isDepthClearEnabled", NULL, M3G_Background_isDepthClearEnabled},
    {"javax/microedition/m3g/Background", "setCrop", NULL, MC_Noop},
    {"javax/microedition/m3g/Background", "getCropX", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Background", "getCropY", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Background", "getCropWidth", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Background", "getCropHeight", NULL, MC_ReturnZero},

    {"javax/microedition/m3g/Image2D", "initFromObj", NULL, M3G_Image2D_initFromObj},
    {"javax/microedition/m3g/Image2D", "initBlank", NULL, M3G_Image2D_initBlank},
    {"javax/microedition/m3g/Image2D", "initBytes", NULL, M3G_Image2D_initBytes},
    {"javax/microedition/m3g/Image2D", "initPal", NULL, M3G_Image2D_initPal},
    {"javax/microedition/m3g/Image2D", "set", NULL, MC_Noop},

    {"javax/microedition/m3g/Texture2D", "setFiltering", NULL, MC_Noop},
    {"javax/microedition/m3g/Texture2D", "getLevelFilter", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Texture2D", "getImageFilter", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Texture2D", "setWrapping", NULL, MC_Noop},
    {"javax/microedition/m3g/Texture2D", "getWrappingS", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Texture2D", "getWrappingT", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Texture2D", "setBlending", NULL, MC_Noop},
    {"javax/microedition/m3g/Texture2D", "getBlending", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Texture2D", "setBlendColor", NULL, MC_Noop},
    {"javax/microedition/m3g/Texture2D", "getBlendColor", NULL, MC_ReturnZero},

    /* M3G Appearance, PolygonMode, CompositingMode, Material, Fog, Light */
    {"javax/microedition/m3g/Appearance", "setLayer", NULL, MC_Noop},
    {"javax/microedition/m3g/Appearance", "getLayer", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Appearance", "setPolygonMode", NULL, M3G_Appearance_setPolygonMode},
    {"javax/microedition/m3g/Appearance", "getPolygonMode", NULL, M3G_Appearance_getPolygonMode},
    {"javax/microedition/m3g/Appearance", "setCompositingMode", NULL, M3G_Appearance_setCompositingMode},
    {"javax/microedition/m3g/Appearance", "getCompositingMode", NULL, M3G_Appearance_getCompositingMode},
    {"javax/microedition/m3g/Appearance", "setMaterial", NULL, M3G_Appearance_setMaterial},
    {"javax/microedition/m3g/Appearance", "getMaterial", NULL, M3G_Appearance_getMaterial},
    {"javax/microedition/m3g/Appearance", "setFog", NULL, MC_Noop},
    {"javax/microedition/m3g/Appearance", "getFog", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/Appearance", "setTexture", NULL, M3G_Appearance_setTexture},
    {"javax/microedition/m3g/Appearance", "getTexture", NULL, M3G_Appearance_getTexture},

    {"javax/microedition/m3g/PolygonMode", "setCulling", NULL, M3G_PolygonMode_setCulling},
    {"javax/microedition/m3g/PolygonMode", "getCulling", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/PolygonMode", "setWinding", NULL, M3G_PolygonMode_setWinding},
    {"javax/microedition/m3g/PolygonMode", "getWinding", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/PolygonMode", "setShading", NULL, MC_Noop},
    {"javax/microedition/m3g/PolygonMode", "getShading", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/PolygonMode", "setTwoSidedLightingEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/PolygonMode", "isTwoSidedLightingEnabled", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/PolygonMode", "setLocalCameraLightingEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/PolygonMode", "isLocalCameraLightingEnabled", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/PolygonMode", "setPerspectiveCorrectionEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/PolygonMode", "isPerspectiveCorrectionEnabled", NULL, MC_ReturnZero},

    {"javax/microedition/m3g/CompositingMode", "setBlending", NULL, M3G_CompositingMode_setBlending},
    {"javax/microedition/m3g/CompositingMode", "getBlending", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/CompositingMode", "setAlphaThreshold", NULL, MC_Noop},
    {"javax/microedition/m3g/CompositingMode", "getAlphaThreshold", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/CompositingMode", "setAlphaWriteEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/CompositingMode", "isAlphaWriteEnabled", NULL, MC_ReturnOne},
    {"javax/microedition/m3g/CompositingMode", "setColorWriteEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/CompositingMode", "isColorWriteEnabled", NULL, MC_ReturnOne},
    {"javax/microedition/m3g/CompositingMode", "setDepthWriteEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/CompositingMode", "isDepthWriteEnabled", NULL, MC_ReturnOne},
    {"javax/microedition/m3g/CompositingMode", "setDepthTestEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/CompositingMode", "isDepthTestEnabled", NULL, MC_ReturnOne},
    {"javax/microedition/m3g/CompositingMode", "setDepthOffset", NULL, MC_Noop},
    {"javax/microedition/m3g/CompositingMode", "getDepthOffsetFactor", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/CompositingMode", "getDepthOffsetUnits", NULL, MC_ReturnFloatZero},

    {"javax/microedition/m3g/Material", "setColor", NULL, MC_Noop},
    {"javax/microedition/m3g/Material", "getColor", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Material", "setShininess", NULL, MC_Noop},
    {"javax/microedition/m3g/Material", "getShininess", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Material", "setVertexColorTrackingEnable", NULL, MC_Noop},
    {"javax/microedition/m3g/Material", "isVertexColorTrackingEnabled", NULL, MC_ReturnZero},

    {"javax/microedition/m3g/Fog", "setMode", NULL, MC_Noop},
    {"javax/microedition/m3g/Fog", "getMode", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Fog", "setLinear", NULL, MC_Noop},
    {"javax/microedition/m3g/Fog", "getNearDistance", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Fog", "getFarDistance", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Fog", "setDensity", NULL, MC_Noop},
    {"javax/microedition/m3g/Fog", "getDensity", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Fog", "setColor", NULL, MC_Noop},
    {"javax/microedition/m3g/Fog", "getColor", NULL, MC_ReturnZero},

    {"javax/microedition/m3g/Light", "setMode", NULL, MC_Noop},
    {"javax/microedition/m3g/Light", "getMode", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Light", "setColor", NULL, MC_Noop},
    {"javax/microedition/m3g/Light", "getColor", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Light", "setIntensity", NULL, MC_Noop},
    {"javax/microedition/m3g/Light", "getIntensity", NULL, MC_ReturnFloatOne},
    {"javax/microedition/m3g/Light", "setSpotAngle", NULL, MC_Noop},
    {"javax/microedition/m3g/Light", "getSpotAngle", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Light", "setSpotExponent", NULL, MC_Noop},
    {"javax/microedition/m3g/Light", "getSpotExponent", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Light", "setAttenuation", NULL, MC_Noop},
    {"javax/microedition/m3g/Light", "getConstantAttenuation", NULL, MC_ReturnFloatOne},
    {"javax/microedition/m3g/Light", "getLinearAttenuation", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Light", "getQuadraticAttenuation", NULL, MC_ReturnFloatZero},

    /* M3G Geometry & Mesh */
    {"javax/microedition/m3g/VertexArray", "init", NULL, M3G_VertexArray_init},
    {"javax/microedition/m3g/VertexArray", "set", "(II[B)V", M3G_VertexArray_setBytes},
    {"javax/microedition/m3g/VertexArray", "set", "(II[S)V", M3G_VertexArray_setShorts},
    {"javax/microedition/m3g/VertexArray", "get", NULL, MC_Noop},

    {"javax/microedition/m3g/VertexBuffer", "init", NULL, M3G_VertexBuffer_init},
    {"javax/microedition/m3g/VertexBuffer", "getVertexCount", NULL, M3G_VertexBuffer_getVertexCount},
    {"javax/microedition/m3g/VertexBuffer", "setPositions", NULL, M3G_VertexBuffer_setPositions},
    {"javax/microedition/m3g/VertexBuffer", "getPositions", NULL, M3G_VertexBuffer_getPositions},
    {"javax/microedition/m3g/VertexBuffer", "setTexCoords", NULL, M3G_VertexBuffer_setTexCoords},
    {"javax/microedition/m3g/VertexBuffer", "getTexCoords", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/VertexBuffer", "setNormals", NULL, M3G_VertexBuffer_setNormals},
    {"javax/microedition/m3g/VertexBuffer", "getNormals", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/VertexBuffer", "setColors", NULL, M3G_VertexBuffer_setColors},
    {"javax/microedition/m3g/VertexBuffer", "getColors", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/VertexBuffer", "setDefaultColor", NULL, M3G_VertexBuffer_setDefaultColor},
    {"javax/microedition/m3g/VertexBuffer", "getDefaultColor", NULL, MC_ReturnZero},

    {"javax/microedition/m3g/IndexBuffer", "getIndexCount", NULL, M3G_IndexBuffer_getIndexCount},
    {"javax/microedition/m3g/IndexBuffer", "getIndices", NULL, MC_Noop},
    {"javax/microedition/m3g/TriangleStripArray", "initExplicit", NULL, M3G_TriangleStripArray_initExplicit},
    {"javax/microedition/m3g/TriangleStripArray", "initImplicit", NULL, M3G_TriangleStripArray_initImplicit},

    {"javax/microedition/m3g/Mesh", "initSingle", NULL, M3G_Mesh_initSingle},
    {"javax/microedition/m3g/Mesh", "initMulti", NULL, M3G_Mesh_initMulti},
    {"javax/microedition/m3g/Mesh", "getVertexBuffer", NULL, M3G_Mesh_getVertexBuffer},
    {"javax/microedition/m3g/Mesh", "getIndexBuffer", NULL, M3G_Mesh_getIndexBuffer},
    {"javax/microedition/m3g/Mesh", "setAppearance", NULL, M3G_Mesh_setAppearance},
    {"javax/microedition/m3g/Mesh", "getAppearance", NULL, M3G_Mesh_getAppearance},
    {"javax/microedition/m3g/Mesh", "getSubmeshCount", NULL, M3G_Mesh_getSubmeshCount},

    {"javax/microedition/m3g/SkinnedMesh", "addTransform", NULL, MC_Noop},
    {"javax/microedition/m3g/SkinnedMesh", "getBoneTransform", NULL, MC_Noop},
    {"javax/microedition/m3g/SkinnedMesh", "getBoneVertices", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/MorphingMesh", "setWeights", NULL, MC_Noop},
    {"javax/microedition/m3g/MorphingMesh", "getWeights", NULL, MC_Noop},
    {"javax/microedition/m3g/MorphingMesh", "getMorphTargetCount", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/MorphingMesh", "getMorphTarget", NULL, MC_ReturnNull},

    {"javax/microedition/m3g/Sprite3D", "setCrop", NULL, MC_Noop},
    {"javax/microedition/m3g/Sprite3D", "getCropX", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Sprite3D", "getCropY", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Sprite3D", "getCropWidth", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Sprite3D", "getCropHeight", NULL, MC_ReturnZero},

    /* M3G Graphics3D & Loader */
    {"javax/microedition/m3g/Graphics3D", "bindTarget", "(Ljava/lang/Object;)V", M3G_G3D_bindTarget},
    {"javax/microedition/m3g/Graphics3D", "bindTarget", "(Ljava/lang/Object;ZI)V", M3G_G3D_bindTargetEx},
    {"javax/microedition/m3g/Graphics3D", "releaseTarget", "()V", M3G_G3D_releaseTarget},
    {"javax/microedition/m3g/Graphics3D", "setViewport", "(IIII)V", M3G_G3D_setViewport},
    {"javax/microedition/m3g/Graphics3D", "getViewportX", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Graphics3D", "getViewportY", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Graphics3D", "getViewportWidth", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Graphics3D", "getViewportHeight", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Graphics3D", "setDepthRange", NULL, MC_Noop},
    {"javax/microedition/m3g/Graphics3D", "getDepthRangeNear", NULL, MC_ReturnFloatZero},
    {"javax/microedition/m3g/Graphics3D", "getDepthRangeFar", NULL, MC_ReturnFloatOne},
    {"javax/microedition/m3g/Graphics3D", "clear", NULL, M3G_G3D_clear},
    {"javax/microedition/m3g/Graphics3D", "render", "(Ljavax/microedition/m3g/World;)V", M3G_G3D_renderWorld},
    {"javax/microedition/m3g/Graphics3D", "render", "(Ljavax/microedition/m3g/Node;Ljavax/microedition/m3g/Transform;)V", M3G_G3D_renderNode},
    {"javax/microedition/m3g/Graphics3D", "render", "(Ljavax/microedition/m3g/VertexBuffer;Ljavax/microedition/m3g/IndexBuffer;Ljavax/microedition/m3g/Appearance;Ljavax/microedition/m3g/Transform;)V", M3G_G3D_renderSubmesh},
    {"javax/microedition/m3g/Graphics3D", "render", "(Ljavax/microedition/m3g/VertexBuffer;Ljavax/microedition/m3g/IndexBuffer;Ljavax/microedition/m3g/Appearance;Ljavax/microedition/m3g/Transform;I)V", M3G_G3D_renderSubmesh},
    {"javax/microedition/m3g/Graphics3D", "setCamera", NULL, M3G_G3D_setCamera},
    {"javax/microedition/m3g/Graphics3D", "getCamera", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/Graphics3D", "addLight", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Graphics3D", "setLight", NULL, MC_Noop},
    {"javax/microedition/m3g/Graphics3D", "resetLights", NULL, MC_Noop},
    {"javax/microedition/m3g/Graphics3D", "getLightCount", NULL, MC_ReturnZero},
    {"javax/microedition/m3g/Graphics3D", "getLight", NULL, MC_ReturnNull},
    {"javax/microedition/m3g/Graphics3D", "getProperties", NULL, MC_ReturnNull},

    {"javax/microedition/m3g/Loader", "load", "(Ljava/lang/String;)[Ljavax/microedition/m3g/Object3D;", M3G_Loader_loadRes},
    {"javax/microedition/m3g/Loader", "load", "([BI)[Ljavax/microedition/m3g/Object3D;", M3G_Loader_loadBytes},

    /* MascotCapsule Micro3D v3 */
    {"com/mascotcapsule/micro3d/v3/Util3D", "sqrt", NULL, MC_Util3D_sqrt},
    {"com/mascotcapsule/micro3d/v3/Util3D", "sin", NULL, MC_Util3D_sin},
    {"com/mascotcapsule/micro3d/v3/Util3D", "cos", NULL, MC_Util3D_cos},
    {"com/mascotcapsule/micro3d/v3/Vector3D", "unit", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Vector3D", "innerProduct", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/Vector3D", "outerProduct", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "(IIIIIIIIIIII)V", MC_AffineTrans_init12},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "([I)V", MC_AffineTrans_initArr},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "([[I)V", MC_AffineTrans_setIdentity},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "setIdentity", NULL, MC_AffineTrans_setIdentity},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", MC_AffineTrans_set},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "([I)V", MC_AffineTrans_initArr},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "([[I)V", MC_Noop},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "get", NULL, MC_AffineTrans_get},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "mul", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", MC_AffineTrans_mul1},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "mul", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", MC_AffineTrans_mul2},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "rotationX", NULL, MC_AffineTrans_rotX},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "rotationY", NULL, MC_AffineTrans_rotY},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "rotationZ", NULL, MC_AffineTrans_rotZ},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotationX", NULL, MC_AffineTrans_rotX},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotationY", NULL, MC_AffineTrans_rotY},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotationZ", NULL, MC_AffineTrans_rotZ},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotation", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "setViewTrans", NULL, MC_AffineTrans_setViewTrans},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "transform", NULL, MC_AffineTrans_transform},
    {"com/mascotcapsule/micro3d/v3/AffineTrans", "transPoint", NULL, MC_AffineTrans_transform},
    {"com/mascotcapsule/micro3d/v3/Texture", "initBytes", NULL, MC_Texture_initBytes},
    {"com/mascotcapsule/micro3d/v3/Texture", "initRes", NULL, MC_Texture_initRes},
    {"com/mascotcapsule/micro3d/v3/ActionTable", "initBytes", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/ActionTable", "initRes", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/ActionTable", "getNumAction", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/ActionTable", "getNumFrame", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/Figure", "initBytes", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Figure", "initRes", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Figure", "setPosture", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Figure", "getTexture", NULL, MC_ReturnNull},
    {"com/mascotcapsule/micro3d/v3/Figure", "setTexture", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Figure", "getNumTextures", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/Figure", "selectTexture", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Figure", "getNumPattern", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/Figure", "setPattern", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "<init>", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "setAffineTrans", NULL, MC_FigureLayout_setAffineTrans},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "getAffineTrans", NULL, MC_FigureLayout_getAffineTrans},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "setScale", NULL, MC_FigureLayout_setScale},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "getScaleX", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "getScaleY", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "setCenter", NULL, MC_FigureLayout_setCenter},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "getCenterX", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "getCenterY", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "setPerspective", NULL, MC_FigureLayout_setPerspective},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "setParallel", NULL, MC_FigureLayout_setParallel},
    {"com/mascotcapsule/micro3d/v3/FigureLayout", "setParallelSize", NULL, MC_FigureLayout_setParallel},
    {"com/mascotcapsule/micro3d/v3/Light", "<init>", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Light", "setDirection", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Light", "getDirection", NULL, MC_ReturnNull},
    {"com/mascotcapsule/micro3d/v3/Light", "setDirIntensity", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Light", "getDirIntensity", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/Light", "setAmbIntensity", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Light", "getAmbIntensity", NULL, MC_ReturnOne},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "<init>", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "setLight", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "getLight", NULL, MC_ReturnNull},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "setShading", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "getShading", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "setSemiTransparentEnabled", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "isSemiTransparentEnabled", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "setSphereMap", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "getSphereMap", NULL, MC_ReturnNull},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "setToonParams", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "getToonThreshold", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "getToonHigh", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/Effect3D", "getToonLow", NULL, MC_ReturnZero},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "bind", NULL, MC_G3D_bind},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "release", NULL, MC_G3D_release},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "flush", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "dispose", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "renderPrimitives", NULL, MC_G3D_renderPrimitives},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "drawCommandList", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "renderFigure", NULL, MC_Noop},
    {"com/mascotcapsule/micro3d/v3/Graphics3D", "drawFigure", NULL, MC_Noop},

    {NULL, NULL, NULL, NULL}
};

void midp_m3g_init(void)
{
    mat_identity(&s_proj);
    mat_identity(&s_mv);
    mat_identity(&s_mvp);
    native_register(m3g_natives);
}

void midp_m3g_shutdown(void)
{
    if (s_zbuf) {
        nj_pal_free(s_zbuf);
        s_zbuf = NULL;
        s_zw = s_zh = 0;
    }
    s_target.px = NULL;
}
