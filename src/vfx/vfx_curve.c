#include "vfx_curve.h"
#include <string.h>

void vfx_curve_const(VfxCurve *c, vec4 v, int channels) {
    memset(c, 0, sizeof(*c));
    c->type = VFX_CURVE_CONST;
    c->channels = (u8)channels;
    c->key_count = 1;
    c->t[0] = 0;
    c->v[0] = v;
}

static vec4 v4_lerp4(vec4 a, vec4 b, float t) {
    return v4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void vfx_curve_auto_tangents(VfxCurve *c) {
    if (c->type != VFX_CURVE_BEZIER) return;
    int n = c->key_count;
    for (int i = 0; i < n; i++) {
        int a = i > 0 ? i - 1 : i, b = i < n - 1 ? i + 1 : i;
        float dt = c->t[b] - c->t[a];
        vec4 d = dt > 1e-6f ? v4((c->v[b].x - c->v[a].x) / dt, (c->v[b].y - c->v[a].y) / dt, (c->v[b].z - c->v[a].z) / dt, (c->v[b].w - c->v[a].w) / dt)
                            : v4(0, 0, 0, 0);
        c->tan_in[i] = c->tan_out[i] = d;
    }
}

vec4 vfx_curve_eval(const VfxCurve *c, float t) {
    if (c->key_count == 0) return v4(1, 1, 1, 1);
    if (c->type == VFX_CURVE_CONST || c->key_count == 1) return c->v[0];
    if (t <= c->t[0]) return c->v[0];
    int n = c->key_count;
    if (t >= c->t[n - 1]) return c->v[n - 1];
    int i = 0;
    while (i < n - 2 && t > c->t[i + 1]) i++;
    float span = c->t[i + 1] - c->t[i];
    float u = span > 1e-6f ? (t - c->t[i]) / span : 0.0f;
    if (c->type == VFX_CURVE_LINEAR) return v4_lerp4(c->v[i], c->v[i + 1], u);
    // Cubic Hermite.
    float u2 = u * u, u3 = u2 * u;
    float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    vec4 p0 = c->v[i], p1 = c->v[i + 1], m0 = c->tan_out[i], m1 = c->tan_in[i + 1];
    return v4(h00 * p0.x + h10 * span * m0.x + h01 * p1.x + h11 * span * m1.x,
              h00 * p0.y + h10 * span * m0.y + h01 * p1.y + h11 * span * m1.y,
              h00 * p0.z + h10 * span * m0.z + h01 * p1.z + h11 * span * m1.z,
              h00 * p0.w + h10 * span * m0.w + h01 * p1.w + h11 * span * m1.w);
}

float vfx_curve_eval1(const VfxCurve *c, float t) { return vfx_curve_eval(c, t).x; }

void vfx_curve_bake(const VfxCurve *c, vec4 out[VFX_LUT_SAMPLES]) {
    for (int i = 0; i < VFX_LUT_SAMPLES; i++) out[i] = vfx_curve_eval(c, (float)i / (float)(VFX_LUT_SAMPLES - 1));
}
