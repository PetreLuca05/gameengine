// Curves and seeded random ranges (pure).
#ifndef DG_VFX_CURVE_H
#define DG_VFX_CURVE_H

#include "vfx_types.h"

// Constant curve with value v (all channels).
void vfx_curve_const(VfxCurve *c, vec4 v, int channels);
// Evaluates at t (clamped to [0, 1]). Linear: piecewise linear. Bezier: cubic Hermite with the
// keys' tangents (auto Catmull-Rom tangents when the JSON gives none).
vec4 vfx_curve_eval(const VfxCurve *c, float t);
float vfx_curve_eval1(const VfxCurve *c, float t);
// Bakes VFX_LUT_SAMPLES samples (GPU lookup table, sampled linearly).
void vfx_curve_bake(const VfxCurve *c, vec4 out[VFX_LUT_SAMPLES]);
// Fills auto tangents for bezier curves without explicit ones.
void vfx_curve_auto_tangents(VfxCurve *c);

// Deterministic hash RNG (same functions in shaders/vfx_common.glsl).
DG_INLINE u32 vfx_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
DG_INLINE float vfx_rand01(u32 *state) {
    *state = vfx_hash(*state + 0x9e3779b9U);
    return (float)(*state >> 8) * (1.0f / 16777216.0f);
}
DG_INLINE float vfx_range(VfxRange r, u32 *state) { return r.min + (r.max - r.min) * vfx_rand01(state); }
DG_INLINE u32 vfx_str_hash(const char *s) {
    u32 h = 2166136261u;
    while (*s) { h ^= (u8)*s++; h *= 16777619u; }
    return h;
}

#endif
