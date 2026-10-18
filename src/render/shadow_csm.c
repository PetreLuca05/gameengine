// Cascaded sun shadow layout: splits, sector sphere fit, texel snapping, bias and kernel from the
// texel size, the viewmodel tile (shadow_csm.h, docs/SHADOWS.md "Cascades").
#include "render/shadow_csm.h"
#include <math.h>
#include <stdio.h>

u32 csm_vm_texels(u32 sun_tile) { return sun_tile < 1024u ? 1024u : (sun_tile > 2048u ? 2048u : sun_tile); }

void csm_sector_sphere(float s, float tan_corner, float *ahead, float *radius) {
    float th = atanf(fmaxf(tan_corner, 0.0f));
    if (th >= 0.25f * DG_PI) {
        // Wide: the cap circle's own sphere already holds the apex and the cap top.
        *ahead = s * cosf(th);
        *radius = s * sinf(th);
    } else {
        // Narrow: equidistant from the apex and the cap rim.
        *ahead = *radius = s / (2.0f * cosf(th));
    }
}

void csm_layout(const CsmParams *p, CsmLayout *out) {
    int n = DG_CLAMP(p->cascades, 1, CSM_MAX);
    float D = fmaxf(p->distance, CSM_NEAR * 2.0f);
    float tv = tanf(fmaxf(p->fov_y_deg, CSM_FIT_FOV_MIN) * 0.5f * DG_DEG2RAD);
    float th = tv * fmaxf(p->aspect, 0.1f);
    float tc = sqrtf(tv * tv + th * th);
    out->count = n;
    for (int k = 0; k < CSM_MAX; k++) {
        int i = DG_MIN(k, n - 1) + 1;
        float t = (float)i / (float)n;
        float lg = CSM_NEAR * powf(D / CSM_NEAR, t), un = CSM_NEAR + (D - CSM_NEAR) * t;
        float s = i == n ? D : CSM_LAMBDA * lg + (1.0f - CSM_LAMBDA) * un;
        if (p->splits && i < n) s = fminf(fmaxf(p->splits[i - 1], 0.5f), D);
        out->split[k] = s;
        float r;
        csm_sector_sphere(s, tc, &out->ahead[k], &r);
        // The tile is CSM_EDGE_TEXELS wider on each side than the sphere: the shader's 4-texel border
        // plus the snap (up to one texel per axis) never cut a point of the sphere off.
        float T = (float)DG_MAX(p->tile, 32u);
        out->radius[k] = r * T / (T - 2.0f * CSM_EDGE_TEXELS);
        out->texel_m[k] = 2.0f * out->radius[k] / (float)DG_MAX(p->tile, 1u);
    }
}

mat4 csm_light_view(vec3 to_light) {
    vec3 L = v3_norm(to_light);
    vec3 up = fabsf(L.y) > 0.99f ? v3(0, 0, 1) : v3(0, 1, 0);
    return mat4_look_at(v3(0, 0, 0), v3_neg(L), up);
}

void csm_fit(const mat4 *lview, vec3 center, float radius, u32 texels, float back, CsmTile *out) { csm_fit_reach(lview, center, radius, texels, back, CSM_REACH, out); }

void csm_fit_reach(const mat4 *lview, vec3 center, float radius, u32 texels, float back, float reach, CsmTile *out) {
    vec3 cl = mat4_mul_point(*lview, center);
    float texel = 2.0f * radius / (float)DG_MAX(texels, 1u);
    // Whole texels: the grid origin (cl - radius) moves by whole texels with the camera.
    cl.x = floorf(cl.x / texel) * texel;
    cl.y = floorf(cl.y / texel) * texel;
    out->view = *lview;
    out->proj = mat4_ortho(cl.x - radius, cl.x + radius, cl.y - radius, cl.y + radius, -cl.z - reach, -cl.z + back);
    out->vp = mat4_mul(out->proj, out->view);
    out->center = mat4_mul_point(mat4_inverse_rigid(*lview), cl);
    out->radius = radius;
    out->texel_m = texel;
    out->span = reach + back;
}

float csm_low_sun(float sun_elev) {
    return 1.0f + 2.0f * (1.0f - smoothstepf(3.0f * DG_DEG2RAD, 15.0f * DG_DEG2RAD, sun_elev));
}

CsmBias csm_bias(float texel_m, float sun_elev) {
    float k = csm_low_sun(sun_elev);
    CsmBias b;
    b.normal_m = 1.5f * texel_m * k;
    b.const_m = 0.75f * texel_m * k;
    return b;
}

CsmBias csm_bias_vm(float texel_m, float sun_elev) {
    CsmBias b = csm_bias(texel_m, sun_elev);
    float k = csm_low_sun(sun_elev);
    b.normal_m = fmaxf(b.normal_m, CSM_VM_NORMAL_M * k);
    b.const_m = fmaxf(b.const_m, CSM_VM_CONST_M * k);
    return b;
}

float csm_kernel_texels(float pen_m, float texel_m, float min_tex, float max_tex) {
    return clampf(pen_m / fmaxf(texel_m, 1e-6f), min_tex, fmaxf(max_tex, min_tex));
}

int csm_select(const CsmLayout *l, float d, int first, float *blend, float *fade) {
    int n = l->count;
    float D = l->split[n - 1];
    *fade = 1.0f - smoothstepf(D * (1.0f - CSM_FADE), D, d);
    *blend = 0.0f;
    for (int c = DG_MIN(first, n - 1); c < n; c++) {
        if (d > l->split[c] && c < n - 1) continue;
        if (c < n - 1) *blend = smoothstepf(l->split[c] * (1.0f - CSM_BLEND), l->split[c], d);
        return c;
    }
    return n - 1;
}

void csm_fit_close(const mat4 *lview, vec3 cam, vec3 fwd, vec3 right, vec3 up, float tan_v, float tan_h, float d0, float d1, u32 texels, float reach,
                   CsmTile *out) {
    vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < 8; i++) {
        float d = (i & 4) ? d1 : fmaxf(d0, 0.05f);
        float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
        vec3 p = v3_add(cam, v3_add(v3_scale(fwd, d), v3_add(v3_scale(right, sx * tan_h * d), v3_scale(up, sy * tan_v * d))));
        vec3 l = mat4_mul_point(*lview, p);
        lo = v3_min(lo, l);
        hi = v3_max(hi, l);
    }
    // Square tile (one texel size), the shader's border around it; no snap (Unity's Close Fit swims).
    float r = 0.5f * fmaxf(hi.x - lo.x, hi.y - lo.y);
    float T = (float)DG_MAX(texels, 32u);
    r *= T / (T - 2.0f * CSM_EDGE_TEXELS);
    vec3 c = v3_scale(v3_add(lo, hi), 0.5f);
    float back = (hi.z - lo.z) * 0.5f + 2.0f;
    out->view = *lview;
    out->proj = mat4_ortho(c.x - r, c.x + r, c.y - r, c.y + r, -c.z - back - reach, -c.z + back);
    out->vp = mat4_mul(out->proj, out->view);
    out->center = mat4_mul_point(mat4_inverse_rigid(*lview), c);
    out->radius = r;
    out->texel_m = 2.0f * r / T;
    out->span = reach + 2.0f * back;
}

void csm_fit_vm(const mat4 *lview, vec3 rig_center, u32 texels, CsmTile *out) {
    csm_fit(lview, rig_center, CSM_VM_RADIUS, texels, CSM_VM_RADIUS + 0.5f, out);
}

static float hull_cross(const float *o, const float *a, const float *b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); }

int csm_receiver_planes(const mat4 *vp, const vec3 *pts, int n, float margin_m, vec4 out[CSM_RCV_MAX]) {
    if (n < 3 || n > CSM_RCV_MAX) return 0;
    const float *m = vp->m;   // column-major: rows 0 / 1 give the light-space x / y (ortho: w = 1)
    float q[CSM_RCV_MAX][2];
    for (int i = 0; i < n; i++) {
        q[i][0] = m[0] * pts[i].x + m[4] * pts[i].y + m[8] * pts[i].z + m[12];
        q[i][1] = m[1] * pts[i].x + m[5] * pts[i].y + m[9] * pts[i].z + m[13];
    }
    // Andrew's monotone chain (insertion sort: at most 16 points), counter-clockwise.
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && (q[j][0] < q[j - 1][0] || (q[j][0] == q[j - 1][0] && q[j][1] < q[j - 1][1])); j--) {
            float t0 = q[j][0], t1 = q[j][1];
            q[j][0] = q[j - 1][0], q[j][1] = q[j - 1][1];
            q[j - 1][0] = t0, q[j - 1][1] = t1;
        }
    float h[2 * CSM_RCV_MAX][2];
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && hull_cross(h[k - 2], h[k - 1], q[i]) <= 0) k--;
        h[k][0] = q[i][0], h[k][1] = q[i][1], k++;
    }
    for (int i = n - 2, lo = k + 1; i >= 0; i--) {
        while (k >= lo && hull_cross(h[k - 2], h[k - 1], q[i]) <= 0) k--;
        h[k][0] = q[i][0], h[k][1] = q[i][1], k++;
    }
    int nh = k - 1;   // the last point repeats the first
    if (nh < 3 || nh > CSM_RCV_MAX) return 0;
    for (int i = 0; i < nh; i++) {
        const float *a = h[i], *b = h[i + 1];
        float nx = -(b[1] - a[1]), ny = b[0] - a[0];   // inward normal of a counter-clockwise edge (NDC)
        // nx (x - ax) + ny (y - ay) >= 0 with x / y the rows of vp: a world plane parallel to the light
        vec3 wn = v3(nx * m[0] + ny * m[1], nx * m[4] + ny * m[5], nx * m[8] + ny * m[9]);
        float w = nx * (m[12] - a[0]) + ny * (m[13] - a[1]);
        float l = v3_len(wn);
        if (l < 1e-12f) return 0;
        out[i] = v4(wn.x / l, wn.y / l, wn.z / l, w / l + margin_m);
    }
    return nh;
}

bool csm_validate(const CsmParams *p, float min_tpm, float max_ratio, char *err, int err_size) {
    CsmLayout l;
    csm_layout(p, &l);
    int n = l.count;
    if (fabsf(l.split[n - 1] - p->distance) > 1e-3f * p->distance) {
        snprintf(err, (size_t)err_size, "last split %.2f m != distance %.2f m", l.split[n - 1], p->distance);
        return false;
    }
    for (int k = 0; k < n; k++) {
        if (k && l.split[k] <= l.split[k - 1]) { snprintf(err, (size_t)err_size, "split %d not increasing", k); return false; }
        if (l.radius[k] < 0.5f * l.split[k] || l.ahead[k] < 0.0f) { snprintf(err, (size_t)err_size, "cascade %d sphere does not hold its sector", k); return false; }
        if (k && l.texel_m[k] > max_ratio * l.texel_m[k - 1]) {
            snprintf(err, (size_t)err_size, "cascade %d texel %.4f m > %.1f x cascade %d (%.4f m)", k, l.texel_m[k], max_ratio, k - 1, l.texel_m[k - 1]);
            return false;
        }
        CsmBias b = csm_bias(l.texel_m[k], DG_PI * 0.5f);
        if (fabsf(b.normal_m / l.texel_m[k] - 1.5f) > 1e-4f || fabsf(b.const_m / l.texel_m[k] - 0.75f) > 1e-4f) {
            snprintf(err, (size_t)err_size, "cascade %d bias not from its texel size", k);
            return false;
        }
    }
    if (1.0f / l.texel_m[0] < min_tpm) {
        snprintf(err, (size_t)err_size, "cascade 0: %.1f texels / m < %.1f", 1.0f / l.texel_m[0], min_tpm);
        return false;
    }
    return true;
}
