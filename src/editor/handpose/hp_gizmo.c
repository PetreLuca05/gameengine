// Hand pose editor gizmo / viewport math (docs/HANDPOSE.md).
#include "hp_gizmo.h"
#include <math.h>

static float focal(const HpCamera *c) { return (c->h * 0.5f) / tanf(c->fov_deg * DG_DEG2RAD * 0.5f); }

bool hpg_project(const HpCamera *c, vec3 p, vec2 *out) {
    vec3 v = quat_rotate(quat_conj(c->rot), v3_sub(p, c->pos));   // camera space, -z forward
    if (v.z > -1e-4f) return false;
    float f = focal(c);
    out->x = c->w * 0.5f + v.x / -v.z * f;
    out->y = c->h * 0.5f - v.y / -v.z * f;
    return true;
}

void hpg_ray(const HpCamera *c, vec2 px, vec3 *origin, vec3 *dir) {
    float f = focal(c);
    vec3 v = v3((px.x - c->w * 0.5f) / f, -(px.y - c->h * 0.5f) / f, -1.0f);
    *origin = c->pos;
    *dir = v3_norm(quat_rotate(c->rot, v));
}

bool hpg_ray_box(vec3 o, vec3 d, vec3 center, quat rot, vec3 half, float *t) {
    quat inv = quat_conj(rot);
    vec3 lo = quat_rotate(inv, v3_sub(o, center)), ld = quat_rotate(inv, d);
    float t0 = -1e30f, t1 = 1e30f;
    const float *po = &lo.x, *pd = &ld.x, *ph = &half.x;
    for (int i = 0; i < 3; i++) {
        if (fabsf(pd[i]) < 1e-9f) {
            if (po[i] < -ph[i] || po[i] > ph[i]) return false;
            continue;
        }
        float a = (-ph[i] - po[i]) / pd[i], b = (ph[i] - po[i]) / pd[i];
        if (a > b) { float s = a; a = b; b = s; }
        t0 = fmaxf(t0, a);
        t1 = fminf(t1, b);
        if (t0 > t1) return false;
    }
    if (t1 < 0) return false;
    *t = t0 >= 0 ? t0 : t1;
    return true;
}

void hpg_init(HpGizmo *g) {
    memset(g, 0, sizeof(*g));
    g->local = true;
    g->snap_move = 0.001f;
    g->snap_angle = 1.0f;
    g->size = 0.06f;
    g->hot = g->active = HPG_NONE;
}

Xform hpg_frame(const HpGizmo *g, Xform edited) { return xf_make(edited.t, g->local ? edited.r : quat_identity()); }

static vec3 axis_dir(Xform frame, int axis) {
    static const vec3 ax[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    return quat_rotate(frame.r, ax[axis % 3]);
}

#define RING_SEGS 48

int hpg_lines(const HpGizmo *g, Xform frame, vec3 *a, vec3 *b, int *axis, int max) {
    int n = 0;
    if (g->mode == HPG_TRANSLATE) {
        for (int k = 0; k < 3 && n < max; k++, n++) {
            a[n] = frame.t;
            b[n] = v3_madd(frame.t, axis_dir(frame, k), g->size);
            axis[n] = k;
        }
        return n;
    }
    for (int k = 0; k < 3; k++) {
        vec3 u = axis_dir(frame, (k + 1) % 3), v = axis_dir(frame, (k + 2) % 3);
        float r = g->size * 0.8f;
        for (int i = 0; i < RING_SEGS && n < max; i++, n++) {
            float t0 = (float)i / RING_SEGS * DG_PI * 2, t1 = (float)(i + 1) / RING_SEGS * DG_PI * 2;
            a[n] = v3_add(frame.t, v3_add(v3_scale(u, cosf(t0) * r), v3_scale(v, sinf(t0) * r)));
            b[n] = v3_add(frame.t, v3_add(v3_scale(u, cosf(t1) * r), v3_scale(v, sinf(t1) * r)));
            axis[n] = HPG_RX + k;
        }
    }
    return n;
}

static float seg_dist_px(vec2 p, vec2 a, vec2 b) {
    vec2 ab = v2_sub(b, a), ap = v2_sub(p, a);
    float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 1e-6f ? clampf((ap.x * ab.x + ap.y * ab.y) / l2, 0, 1) : 0;
    vec2 q = v2_add(a, v2_scale(ab, t));
    return v2_len(v2_sub(p, q));
}

int hpg_pick(const HpGizmo *g, const HpCamera *c, Xform frame, vec2 mouse, float radius_px) {
    vec3 a[3 * RING_SEGS], b[3 * RING_SEGS];
    int ax[3 * RING_SEGS];
    int n = hpg_lines(g, frame, a, b, ax, 3 * RING_SEGS);
    int best = HPG_NONE;
    float bd = radius_px;
    for (int i = 0; i < n; i++) {
        vec2 pa, pb;
        if (!hpg_project(c, a[i], &pa) || !hpg_project(c, b[i], &pb)) continue;
        float d = seg_dist_px(mouse, pa, pb);
        if (d < bd) { bd = d; best = ax[i]; }
    }
    return best;
}

// Translate: the point on the axis line closest to the mouse ray.
static bool axis_point(vec3 p0, vec3 dir, vec3 ro, vec3 rd, vec3 *out) {
    vec3 w0 = v3_sub(p0, ro);
    float a = v3_dot(dir, dir), b = v3_dot(dir, rd), cc = v3_dot(rd, rd), d = v3_dot(dir, w0), e = v3_dot(rd, w0);
    float den = a * cc - b * b;
    if (fabsf(den) < 1e-6f) return false;   // ray parallel to the axis
    float s = (b * e - cc * d) / den;
    *out = v3_madd(p0, dir, s);
    return true;
}

// Rotate: the angle of the mouse ray's hit on the ring plane, around the axis.
static bool ring_angle(Xform frame, int k, vec3 ro, vec3 rd, float *ang) {
    vec3 n = axis_dir(frame, k), u = axis_dir(frame, (k + 1) % 3), v = axis_dir(frame, (k + 2) % 3);
    float den = v3_dot(rd, n);
    if (fabsf(den) < 1e-4f) return false;   // edge-on
    float t = v3_dot(v3_sub(frame.t, ro), n) / den;
    vec3 p = v3_sub(v3_madd(ro, rd, t), frame.t);
    *ang = atan2f(v3_dot(p, v), v3_dot(p, u));
    return true;
}

bool hpg_begin(HpGizmo *g, const HpCamera *c, Xform edited, int axis, vec2 mouse) {
    if (axis == HPG_NONE) return false;
    vec3 ro, rd;
    hpg_ray(c, mouse, &ro, &rd);
    Xform frame = hpg_frame(g, edited);
    bool ok = axis <= HPG_Z ? axis_point(frame.t, axis_dir(frame, axis), ro, rd, &g->start_point) : ring_angle(frame, axis - HPG_RX, ro, rd, &g->start_angle);
    if (!ok) return false;
    g->active = axis;
    g->start = edited;
    return true;
}

static float snapf(float v, float step) { return step > 0 ? roundf(v / step) * step : v; }

Xform hpg_drag(const HpGizmo *g, const HpCamera *c, vec2 mouse) {
    Xform out = g->start;
    if (g->active == HPG_NONE) return out;
    vec3 ro, rd;
    hpg_ray(c, mouse, &ro, &rd);
    Xform frame = hpg_frame(g, g->start);
    if (g->active <= HPG_Z) {
        vec3 dir = axis_dir(frame, g->active), p;
        if (!axis_point(frame.t, dir, ro, rd, &p)) return out;
        float d = v3_dot(v3_sub(p, g->start_point), dir);
        if (g->snap) d = snapf(d, g->snap_move);
        out.t = v3_madd(g->start.t, dir, d);
        return out;
    }
    int k = g->active - HPG_RX;
    float a;
    if (!ring_angle(frame, k, ro, rd, &a)) return out;
    float d = wrap_angle(a - g->start_angle) * DG_RAD2DEG;
    if (g->snap) d = snapf(d, g->snap_angle);
    out.r = quat_norm(quat_mul(quat_axis_angle(axis_dir(frame, k), d * DG_DEG2RAD), g->start.r));
    return out;
}

void hpg_end(HpGizmo *g) { g->active = HPG_NONE; }

int hpg_pick_point(const HpCamera *c, const vec3 *pts, int n, vec2 mouse, float radius_px) {
    int best = -1;
    float bd = radius_px;
    for (int i = 0; i < n; i++) {
        vec2 p;
        if (!hpg_project(c, pts[i], &p)) continue;
        float d = v2_len(v2_sub(p, mouse));
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
