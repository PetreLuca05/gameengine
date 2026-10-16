// Byte-oriented message writer/reader with a few quantized encodings.
#ifndef DG_BITBUF_H
#define DG_BITBUF_H

#include "core/base.h"
#include "core/dgmath.h"

typedef struct Writer { u8 *buf; u32 cap, len; bool overflow; } Writer;
typedef struct Reader { const u8 *buf; u32 len, pos; bool overflow; } Reader;

DG_INLINE Writer writer_make(u8 *buf, u32 cap) { Writer w = { buf, cap, 0, false }; return w; }
DG_INLINE Reader reader_make(const u8 *buf, u32 len) { Reader r = { buf, len, 0, false }; return r; }

DG_INLINE void w_bytes(Writer *w, const void *p, u32 n) {
    if (w->len + n > w->cap) { w->overflow = true; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}
DG_INLINE void w_u8(Writer *w, u8 v) { w_bytes(w, &v, 1); }
DG_INLINE void w_u16(Writer *w, u16 v) { w_bytes(w, &v, 2); }
DG_INLINE void w_u32(Writer *w, u32 v) { w_bytes(w, &v, 4); }
DG_INLINE void w_i32(Writer *w, i32 v) { w_bytes(w, &v, 4); }
DG_INLINE void w_f32(Writer *w, float v) { w_bytes(w, &v, 4); }
DG_INLINE void w_vec3(Writer *w, vec3 v) { w_f32(w, v.x); w_f32(w, v.y); w_f32(w, v.z); }
DG_INLINE void w_str(Writer *w, const char *s) {
    u32 n = (u32)strlen(s);
    if (n > 255) n = 255;
    w_u8(w, (u8)n);
    w_bytes(w, s, n);
}
// Smallest-three quaternion: 2 bits index + 3 x 10 bits.
DG_INLINE void w_quat(Writer *w, quat q) {
    float c[4] = { q.x, q.y, q.z, q.w };
    int big = 0;
    for (int i = 1; i < 4; i++) if (fabsf(c[i]) > fabsf(c[big])) big = i;
    float sign = c[big] < 0 ? -1.0f : 1.0f;
    u32 packed = (u32)big << 30;
    int shift = 20;
    for (int i = 0; i < 4; i++) {
        if (i == big) continue;
        float v = c[i] * sign * 0.70710678f; // [-1/sqrt2, 1/sqrt2] -> [-0.5, 0.5]
        u32 qv = (u32)DG_CLAMP((int)lrintf((v + 0.5f) * 1023.0f), 0, 1023);
        packed |= qv << shift;
        shift -= 10;
    }
    w_u32(w, packed);
}

DG_INLINE void r_bytes(Reader *r, void *p, u32 n) {
    if (r->pos + n > r->len) { r->overflow = true; memset(p, 0, n); return; }
    memcpy(p, r->buf + r->pos, n);
    r->pos += n;
}
DG_INLINE u8 r_u8(Reader *r) { u8 v; r_bytes(r, &v, 1); return v; }
DG_INLINE u16 r_u16(Reader *r) { u16 v; r_bytes(r, &v, 2); return v; }
DG_INLINE u32 r_u32(Reader *r) { u32 v; r_bytes(r, &v, 4); return v; }
DG_INLINE i32 r_i32(Reader *r) { i32 v; r_bytes(r, &v, 4); return v; }
DG_INLINE float r_f32(Reader *r) { float v; r_bytes(r, &v, 4); return v; }
DG_INLINE vec3 r_vec3(Reader *r) { vec3 v; v.x = r_f32(r); v.y = r_f32(r); v.z = r_f32(r); return v; }
DG_INLINE void r_str(Reader *r, char *out, u32 size) {
    u32 n = r_u8(r);
    char tmp[256];
    r_bytes(r, tmp, n);
    tmp[n] = 0;
    dg_strcpy(out, size, tmp);
}
DG_INLINE quat r_quat(Reader *r) {
    u32 packed = r_u32(r);
    int big = (int)(packed >> 30);
    float c[4];
    float sum = 0;
    int shift = 20;
    for (int i = 0; i < 4; i++) {
        if (i == big) continue;
        u32 qv = (packed >> shift) & 1023u;
        shift -= 10;
        c[i] = ((float)qv / 1023.0f - 0.5f) * 1.41421356f;
        sum += c[i] * c[i];
    }
    c[big] = sqrtf(fmaxf(0.0f, 1.0f - sum));
    quat q = { c[0], c[1], c[2], c[3] };
    return quat_norm(q);
}

#endif
