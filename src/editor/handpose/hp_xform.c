// Blender-style modal transforms (docs/HANDPOSE.md "Transform tool").
#include "hp_xform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAP_T 0.005f   // m
#define SNAP_R 5.0f     // deg
#define SNAP_S 0.05f

static const char *k_mode[] = { "none", "Move", "Rotate", "Scale" };
static const char *k_orient[] = { "Global", "Local", "Parent", "Normal" };
static const char *k_pivot[] = { "Individual Origins", "Median Point", "Active Element" };
const char *hpx_mode_name(int m) { return m >= 0 && m <= HPX_SCALE ? k_mode[m] : "?"; }
const char *hpx_orient_name(int o) { return o >= 0 && o < HPX_ORIENT_COUNT ? k_orient[o] : "?"; }
const char *hpx_pivot_name(int p) { return p >= 0 && p < HPX_PIVOT_COUNT ? k_pivot[p] : "?"; }
bool hpx_active(const HpXform *x) { return x->mode != HPX_NONE; }

void hpx_begin(HpXform *x, int mode, vec2 mouse, int n, const int *ids, const Xform *start, int active, const quat basis[HPX_ORIENT_COUNT], int orient, int pivot_mode) {
    memset(x, 0, sizeof(*x));
    n = DG_CLAMP(n, 0, HPX_MAX);
    x->mode = n > 0 ? mode : HPX_NONE;
    x->count = n;
    x->active = DG_CLAMP(active, 0, n > 0 ? n - 1 : 0);
    x->axis = -1;
    x->orient = DG_CLAMP(orient, 0, HPX_ORIENT_COUNT - 1);
    x->pivot_mode = DG_CLAMP(pivot_mode, 0, HPX_PIVOT_COUNT - 1);
    x->mouse0 = mouse;
    x->factor = 1.0f;
    for (int i = 0; i < HPX_ORIENT_COUNT; i++) x->basis[i] = basis ? quat_norm(basis[i]) : quat_identity();
    x->basis[HPX_GLOBAL] = quat_identity();
    vec3 sum = v3(0, 0, 0);
    for (int i = 0; i < n; i++) {
        x->ids[i] = ids ? ids[i] : i;
        x->start[i] = x->out[i] = start[i];
        sum = v3_add(sum, start[i].t);
    }
    x->pivot = n ? (x->pivot_mode == HPX_PIVOT_ACTIVE ? start[x->active].t : v3_scale(sum, 1.0f / (float)n)) : v3(0, 0, 0);
}

void hpx_axis(HpXform *x, int axis, bool shift) {
    if (!hpx_active(x) || axis < 0 || axis > 2) return;
    if (x->axis != axis || x->plane != shift) {
        x->axis = axis;
        x->plane = shift;
        x->axis_presses = 1;
        return;
    }
    // Same key again: the other of global / the chosen orientation, then free.
    x->axis_presses++;
    if (x->axis_presses == 2) x->orient = x->orient == HPX_GLOBAL ? HPX_LOCAL : HPX_GLOBAL;
    else { x->axis = -1; x->plane = false; x->axis_presses = 0; }
}

bool hpx_char(HpXform *x, char c) {
    size_t l = strlen(x->num);
    if (c == '\b') { if (l) x->num[l - 1] = 0; return true; }
    if (c == '-') {
        if (x->num[0] == '-') memmove(x->num, x->num + 1, l);
        else if (l + 1 < sizeof(x->num)) { memmove(x->num + 1, x->num, l + 1); x->num[0] = '-'; }
        return true;
    }
    if ((c >= '0' && c <= '9') || c == '.') {
        if (c == '.' && strchr(x->num, '.')) return true;
        if (l + 1 < sizeof(x->num)) { x->num[l] = c; x->num[l + 1] = 0; }
        return true;
    }
    return false;
}

vec3 hpx_axis_world(const HpXform *x, int axis) {
    static const vec3 e[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    if (axis < 0 || axis > 2) return v3(0, 0, 0);
    return quat_rotate(x->basis[x->orient], e[axis]);
}

static bool typed(const HpXform *x, float *v) {
    if (!x->num[0] || !strcmp(x->num, "-") || !strcmp(x->num, ".") || !strcmp(x->num, "-.")) return false;
    *v = (float)atof(x->num);
    return true;
}

static float snapf(float v, float step) { return roundf(v / step) * step; }

// Screen Jacobian at p: J[i] = d(screen) / d(world axis i).
static bool jacobian(const HpxView *v, vec3 p, vec2 J[3], vec2 *ps) {
    if (!v->project(v->ctx, p, ps)) return false;
    float eps = 0.001f * fmaxf(1.0f, v3_dist(p, v->cam_pos));
    static const vec3 e[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int i = 0; i < 3; i++) {
        vec2 q;
        if (!v->project(v->ctx, v3_madd(p, e[i], eps), &q)) return false;
        J[i] = v2((q.x - ps->x) / eps, (q.y - ps->y) / eps);
    }
    return true;
}
static vec2 jmul(const vec2 J[3], vec3 w) { return v2(J[0].x * w.x + J[1].x * w.y + J[2].x * w.z, J[0].y * w.x + J[1].y * w.y + J[2].y * w.z); }

void hpx_update(HpXform *x, vec2 mouse, const HpxView *v, bool snap) {
    if (!hpx_active(x) || !v || !v->project) return;
    vec2 J[3], ps;
    if (!jacobian(v, x->pivot, J, &ps)) return;
    float tv_val;
    bool has_num = typed(x, &tv_val);
    vec3 axis = hpx_axis_world(x, x->axis);
    if (x->mode == HPX_GRAB) {
        vec2 d = v2(mouse.x - x->mouse0.x, mouse.y - x->mouse0.y);
        vec3 w = v3(0, 0, 0);
        if (has_num) {
            vec3 a = x->axis >= 0 ? (x->plane ? hpx_axis_world(x, (x->axis + 1) % 3) : axis) : hpx_axis_world(x, 0);
            w = v3_scale(a, tv_val);
        } else if (x->axis >= 0 && !x->plane) {
            vec2 ja = jmul(J, axis);
            float l2 = ja.x * ja.x + ja.y * ja.y;
            float t = l2 > 1e-9f ? (d.x * ja.x + d.y * ja.y) / l2 : 0.0f;
            if (snap) t = snapf(t, SNAP_T);
            w = v3_scale(axis, t);
        } else if (x->axis >= 0) {
            vec3 b1 = hpx_axis_world(x, (x->axis + 1) % 3), b2 = hpx_axis_world(x, (x->axis + 2) % 3);
            vec2 m1 = jmul(J, b1), m2 = jmul(J, b2);
            float det = m1.x * m2.y - m2.x * m1.y;
            if (fabsf(det) > 1e-9f) {
                float c1 = (d.x * m2.y - m2.x * d.y) / det, c2 = (m1.x * d.y - d.x * m1.y) / det;
                if (snap) { c1 = snapf(c1, SNAP_T); c2 = snapf(c2, SNAP_T); }
                w = v3_add(v3_scale(b1, c1), v3_scale(b2, c2));
            }
        } else {
            // View plane: the smallest world move that moves the pivot by d on screen.
            float a = J[0].x * J[0].x + J[1].x * J[1].x + J[2].x * J[2].x;
            float b = J[0].x * J[0].y + J[1].x * J[1].y + J[2].x * J[2].y;
            float c = J[0].y * J[0].y + J[1].y * J[1].y + J[2].y * J[2].y;
            float det = a * c - b * b;
            if (fabsf(det) > 1e-12f) {
                float l0 = (c * d.x - b * d.y) / det, l1 = (a * d.y - b * d.x) / det;
                w = v3(J[0].x * l0 + J[0].y * l1, J[1].x * l0 + J[1].y * l1, J[2].x * l0 + J[2].y * l1);
                if (snap) w = v3(snapf(w.x, SNAP_T), snapf(w.y, SNAP_T), snapf(w.z, SNAP_T));
            }
        }
        x->delta = w;
        for (int i = 0; i < x->count; i++) { x->out[i] = x->start[i]; x->out[i].t = v3_add(x->start[i].t, w); }
        return;
    }
    vec2 v0 = v2(x->mouse0.x - ps.x, x->mouse0.y - ps.y), v1 = v2(mouse.x - ps.x, mouse.y - ps.y);
    if (x->mode == HPX_ROTATE) {
        vec3 to_eye = v3_sub(v->cam_pos, x->pivot);
        float el = v3_len(to_eye);
        to_eye = el > 1e-6f ? v3_scale(to_eye, 1.0f / el) : v3(0, 0, 1);
        // Screen y points down: a visually counter-clockwise drag is a negative atan2 angle.
        float ang = -atan2f(v0.x * v1.y - v0.y * v1.x, v0.x * v1.x + v0.y * v1.y) * DG_RAD2DEG;
        vec3 ax = to_eye;
        if (x->axis >= 0) {
            ax = axis;
            float s = v3_dot(axis, to_eye);
            ang *= s < 0 ? -1.0f : 1.0f;
        }
        if (has_num) ang = tv_val;
        else if (snap) ang = snapf(ang, SNAP_R);
        x->angle_deg = ang;
        quat q = quat_axis_angle(ax, ang * DG_DEG2RAD);
        for (int i = 0; i < x->count; i++) {
            Xform o = x->start[i];
            o.r = quat_norm(quat_mul(q, o.r));
            if (x->pivot_mode != HPX_PIVOT_INDIVIDUAL) o.t = v3_add(x->pivot, quat_rotate(q, v3_sub(o.t, x->pivot)));
            x->out[i] = o;
        }
        return;
    }
    if (x->mode == HPX_SCALE) {
        float l0 = sqrtf(v0.x * v0.x + v0.y * v0.y), l1 = sqrtf(v1.x * v1.x + v1.y * v1.y);
        float f = l0 > 1.0f ? l1 / l0 : 1.0f;
        if (has_num) f = tv_val;
        else if (snap) f = snapf(f, SNAP_S);
        x->factor = f;
        for (int i = 0; i < x->count; i++) {
            Xform o = x->start[i];
            float s = (o.s.x > 0 ? o.s.x : 1.0f) * f;
            o.s = v3s(s);
            if (x->pivot_mode != HPX_PIVOT_INDIVIDUAL) o.t = v3_add(x->pivot, v3_scale(v3_sub(o.t, x->pivot), f));
            x->out[i] = o;
        }
    }
}

void hpx_cancel(HpXform *x) {
    for (int i = 0; i < x->count; i++) x->out[i] = x->start[i];
    x->mode = HPX_NONE;
}

void hpx_end(HpXform *x) { x->mode = HPX_NONE; }

void hpx_status(const HpXform *x, char *out, int size) {
    static const char *an[3] = { "X", "Y", "Z" };
    char ax[48] = "";
    if (x->axis >= 0) snprintf(ax, sizeof(ax), " %s%s (%s)", x->plane ? "plane without " : "", an[x->axis], hpx_orient_name(x->orient));
    char val[64];
    if (x->mode == HPX_GRAB) snprintf(val, sizeof(val), "D: %.4f %.4f %.4f m (%.1f mm)", x->delta.x, x->delta.y, x->delta.z, v3_len(x->delta) * 1000.0f);
    else if (x->mode == HPX_ROTATE) snprintf(val, sizeof(val), "%.2f deg", x->angle_deg);
    else if (x->mode == HPX_SCALE) snprintf(val, sizeof(val), "x %.3f (uniform)", x->factor);
    else val[0] = 0;
    snprintf(out, (size_t)size, "%s%s  %s%s%s", hpx_mode_name(x->mode), ax, val, x->num[0] ? "  typed: " : "", x->num);
}
