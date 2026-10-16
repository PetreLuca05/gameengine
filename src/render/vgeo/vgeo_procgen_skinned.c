#include "vgeo_procgen_skinned.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ column-major mat4 helpers

static void m4_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
static void m4_mul(const float a[16], const float b[16], float o[16]) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, sizeof(r));
}
static void m4_translate(float m[16], float x, float y, float z) {
    m4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}
// Rotation about a unit axis, times a uniform scale.
static void m4_rot_scale(float m[16], const float ax[3], float angle, float s) {
    float c = cosf(angle), sn = sinf(angle), t = 1.0f - c, x = ax[0], y = ax[1], z = ax[2];
    m4_identity(m);
    m[0] = (t * x * x + c) * s;
    m[1] = (t * x * y + sn * z) * s;
    m[2] = (t * x * z - sn * y) * s;
    m[4] = (t * x * y - sn * z) * s;
    m[5] = (t * y * y + c) * s;
    m[6] = (t * y * z + sn * x) * s;
    m[8] = (t * x * z + sn * y) * s;
    m[9] = (t * y * z - sn * x) * s;
    m[10] = (t * z * z + c) * s;
}

static void reserve(VgGenSkinned *s, u32 verts, u32 idx) {
    VgGenMesh *m = &s->mesh;
    if (m->vertex_count + verts > m->vcap) {
        while (m->vertex_count + verts > m->vcap) m->vcap = m->vcap ? m->vcap * 2 : 1024;
        m->pos = (float *)realloc(m->pos, sizeof(float) * 3 * m->vcap);
    }
    if (m->index_count + idx > m->icap) {
        while (m->index_count + idx > m->icap) m->icap = m->icap ? m->icap * 2 : 3072;
        m->indices = (u32 *)realloc(m->indices, sizeof(u32) * m->icap);
    }
    s->joints = (u8 *)realloc(s->joints, (size_t)m->vcap * 4);
    s->weights = (float *)realloc(s->weights, sizeof(float) * 4 * m->vcap);
}

static void perp(const float d[3], float e1[3], float e2[3]) {
    float a[3] = { fabsf(d[1]) < 0.9f ? 0.0f : 1.0f, fabsf(d[1]) < 0.9f ? 1.0f : 0.0f, 0.0f };
    float x[3] = { a[1] * d[2] - a[2] * d[1], a[2] * d[0] - a[0] * d[2], a[0] * d[1] - a[1] * d[0] };
    float l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int k = 0; k < 3; k++) e1[k] = x[k] / l;
    e2[0] = d[1] * e1[2] - d[2] * e1[1];
    e2[1] = d[2] * e1[0] - d[0] * e1[2];
    e2[2] = d[0] * e1[1] - d[1] * e1[0];
}

// Signed volume of triangles [first, end) (x 6); flips them when negative (CCW outward).
static void orient_outward(VgGenMesh *m, u32 first) {
    double v = 0;
    for (u32 i = first; i < m->index_count; i += 3) {
        const float *a = &m->pos[m->indices[i] * 3], *b = &m->pos[m->indices[i + 1] * 3], *c = &m->pos[m->indices[i + 2] * 3];
        v += (double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) - (double)a[1] * ((double)b[0] * c[2] - (double)b[2] * c[0]) +
             (double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0]);
    }
    if (v >= 0) return;
    for (u32 i = first; i < m->index_count; i += 3) {
        u32 t = m->indices[i + 1];
        m->indices[i + 1] = m->indices[i + 2];
        m->indices[i + 2] = t;
    }
}

// Weights of a tentacle vertex at bone coordinate x (0 = base, bones_per = tip): a Gaussian over the
// bone segment centres, the 4 strongest kept; "bone -1" is the body (bone 0).
static void tentacle_skin(const VgGenSkinned *s, u32 k, float x, u8 j[4], float w[4]) {
    i32 i0 = (i32)floorf(x);
    float best_w[4] = { 0, 0, 0, 0 };
    u8 best_j[4] = { 0, 0, 0, 0 };
    for (i32 i = i0 - 2; i <= i0 + 2; i++) {
        if (i < -1 || i >= (i32)s->bones_per) continue;
        float d = x - ((float)i + 0.5f);
        float ww = expf(-d * d / 0.45f);
        u8 id = i < 0 ? 0 : (u8)(1 + k * s->bones_per + (u32)i);
        for (int q = 0; q < 4; q++)
            if (ww > best_w[q]) {
                for (int r = 3; r > q; r--) { best_w[r] = best_w[r - 1]; best_j[r] = best_j[r - 1]; }
                best_w[q] = ww;
                best_j[q] = id;
                break;
            }
    }
    float sum = best_w[0] + best_w[1] + best_w[2] + best_w[3];
    for (int q = 0; q < 4; q++) { j[q] = best_j[q]; w[q] = best_w[q] / sum; }
}

void vgeo_gen_creature(VgGenSkinned *s, u32 seed, u32 tentacles, u32 bones_per, u64 tris) {
    memset(s, 0, sizeof(*s));
    if (1 + tentacles * bones_per > VGEO_GEN_MAX_BONES) bones_per = (VGEO_GEN_MAX_BONES - 1) / (tentacles ? tentacles : 1);
    s->tentacles = tentacles;
    s->bones_per = bones_per;
    s->bone_count = 1 + tentacles * bones_per;
    s->seed = seed;
    const float center[3] = { 0, 1.0f, 0 };
    const float body_r = 0.45f, len = 1.6f, r0 = 0.12f, r1 = 0.04f;
    // body: ~20 % of the triangles, bone 0
    u32 n = (u32)sqrt((double)tris * 0.2 / 20.0);
    if (n < 3) n = 3;
    VgGenMesh *m = &s->mesh;
    vgeo_gen_rock(m, seed, n, body_r, center, 0.05f);
    reserve(s, 0, 0);
    for (u32 v = 0; v < m->vertex_count; v++) {
        memset(&s->joints[v * 4], 0, 4);
        s->weights[v * 4] = 1.0f;
        s->weights[v * 4 + 1] = s->weights[v * 4 + 2] = s->weights[v * 4 + 3] = 0.0f;
    }
    s->shells = 1;
    s->parent[0] = -1;
    memcpy(s->head[0], center, 12);
    s->dir[0][1] = 1.0f;
    // tentacles: tapered capsules (tube rings + hemispherical caps + poles)
    u64 per = tentacles ? (u64)((double)tris * 0.8 / tentacles) : 0;
    u32 seg = (u32)sqrt((double)per / 4.0);
    seg = seg < 8 ? 8 : (seg > 64 ? 64 : seg);
    u32 caps = 4;
    u32 rings = (u32)(per / (2ull * seg));
    rings = rings > 2 * caps + 2 ? rings - 2 * caps : 2;
    for (u32 k = 0; k < tentacles; k++) {
        float a = 6.2831853f * (float)k / (float)tentacles + 0.3f, tilt = -0.45f;
        float d[3] = { cosf(a) * cosf(tilt), sinf(tilt), sinf(a) * cosf(tilt) }, e1[3], e2[3];
        perp(d, e1, e2);
        float p0[3] = { center[0] + d[0] * 0.3f, center[1] + d[1] * 0.3f, center[2] + d[2] * 0.3f };
        for (u32 i = 0; i < bones_per; i++) {
            u32 b = 1 + k * bones_per + i;
            s->parent[b] = i ? (i32)(b - 1) : 0;
            for (int c = 0; c < 3; c++) {
                s->head[b][c] = p0[c] + d[c] * len * (float)i / (float)bones_per;
                s->dir[b][c] = d[c];
            }
        }
        // profile: (axial s, radius) per ring, base pole .. tip pole
        u32 nr = caps - 1 + rings + 1 + caps - 1;
        float *ps = (float *)malloc(sizeof(float) * 2 * nr);
        u32 q = 0;
        for (u32 c = 1; c < caps; c++) {
            float ph = 1.5707963f * (float)c / (float)caps;
            ps[q * 2] = -r0 * cosf(ph);
            ps[q * 2 + 1] = r0 * sinf(ph);
            q++;
        }
        for (u32 j = 0; j <= rings; j++) {
            float u = (float)j / (float)rings;
            ps[q * 2] = len * u;
            ps[q * 2 + 1] = r0 + (r1 - r0) * u;
            q++;
        }
        for (u32 c = caps - 1; c >= 1; c--) {
            float ph = 1.5707963f * (float)c / (float)caps;
            ps[q * 2] = len + r1 * cosf(ph);
            ps[q * 2 + 1] = r1 * sinf(ph);
            q++;
        }
        u32 first_idx = m->index_count;
        reserve(s, nr * seg + 2, (nr - 1) * seg * 6 + 2 * seg * 3);
        u32 base = m->vertex_count;
        for (u32 j = 0; j < nr + 2; j++) {
            u32 cnt = (j == 0 || j == nr + 1) ? 1 : seg;
            float sx = j == 0 ? -r0 : (j == nr + 1 ? len + r1 : ps[(j - 1) * 2]), rr = (j == 0 || j == nr + 1) ? 0.0f : ps[(j - 1) * 2 + 1];
            for (u32 t = 0; t < cnt; t++) {
                float th = 6.2831853f * (float)t / (float)seg, cs = cosf(th), sn = sinf(th);
                float *p = &m->pos[m->vertex_count * 3];
                for (int c = 0; c < 3; c++) p[c] = p0[c] + d[c] * sx + (e1[c] * cs + e2[c] * sn) * rr;
                float x = sx / len * (float)bones_per;
                x = x < 0 ? 0 : (x > (float)bones_per ? (float)bones_per : x);
                tentacle_skin(s, k, x, &s->joints[m->vertex_count * 4], &s->weights[m->vertex_count * 4]);
                m->vertex_count++;
            }
        }
        // ring j (1..nr) vertex t at base + 1 + (j - 1) * seg + t; poles at base and base + 1 + nr * seg
        u32 tip = base + 1 + nr * seg;
        for (u32 t = 0; t < seg; t++) {
            u32 t1 = (t + 1) % seg;
            u32 *ix = &m->indices[m->index_count];
            ix[0] = base;
            ix[1] = base + 1 + t1;
            ix[2] = base + 1 + t;
            m->index_count += 3;
            for (u32 j = 1; j < nr; j++) {
                u32 a0 = base + 1 + (j - 1) * seg + t, a1 = base + 1 + (j - 1) * seg + t1, b0 = a0 + seg, b1 = a1 + seg;
                ix = &m->indices[m->index_count];
                ix[0] = a0; ix[1] = a1; ix[2] = b0;
                ix[3] = a1; ix[4] = b1; ix[5] = b0;
                m->index_count += 6;
            }
            ix = &m->indices[m->index_count];
            ix[0] = tip;
            ix[1] = base + 1 + (nr - 1) * seg + t;
            ix[2] = base + 1 + (nr - 1) * seg + t1;
            m->index_count += 3;
        }
        orient_outward(m, first_idx);
        free(ps);
        s->shells++;
    }
}

void vgeo_gen_skinned_free(VgGenSkinned *s) {
    vgeo_gen_free(&s->mesh);
    free(s->joints);
    free(s->weights);
    memset(s, 0, sizeof(*s));
}

void vgeo_gen_creature_pose(const VgGenSkinned *s, float t, float phase, float *pal) {
    float world[VGEO_GEN_MAX_BONES][16], a[16], b[16];
    t += phase;
    for (u32 i = 0; i < s->bone_count; i++) {
        const float *h = s->head[i];
        if (s->parent[i] < 0) {
            // root: wanders and turns about y, bobbing
            const float up[3] = { 0, 1, 0 };
            m4_translate(a, h[0] + 2.0f * sinf(0.3f * t), h[1] + 0.2f * sinf(1.1f * t), h[2] + 1.5f * cosf(0.25f * t));
            m4_rot_scale(b, up, 0.6f * sinf(0.5f * t), 1.0f);
            m4_mul(a, b, world[i]);
        } else {
            u32 p = (u32)s->parent[i], k = (i - 1) / s->bones_per, j = (i - 1) % s->bones_per;
            const float *hp = s->head[p];
            m4_translate(a, h[0] - hp[0], h[1] - hp[1], h[2] - hp[2]);
            m4_mul(world[p], a, world[i]);
            // swing about an axis perpendicular to the tentacle that turns slowly, pulsing scale
            float e1[3], e2[3], ax[3], ph = 0.4f * t + (float)k;
            perp(s->dir[i], e1, e2);
            for (int c = 0; c < 3; c++) ax[c] = e1[c] * cosf(ph) + e2[c] * sinf(ph);
            float ang = 0.45f * sinf(1.6f * t + 0.8f * (float)j + 1.3f * (float)k), sc = 1.0f + 0.06f * sinf(2.1f * t + (float)j + (float)k);
            m4_rot_scale(b, ax, ang, sc);
            m4_mul(world[i], b, world[i]);
        }
        m4_translate(a, -h[0], -h[1], -h[2]);   // inverse bind (identity bind rotations)
        m4_mul(world[i], a, &pal[i * 16]);
    }
}
