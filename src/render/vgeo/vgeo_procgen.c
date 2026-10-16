#include "vgeo_procgen.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void vgeo_gen_free(VgGenMesh *m) {
    free(m->pos);
    free(m->indices);
    memset(m, 0, sizeof(*m));
}

static void reserve(VgGenMesh *m, u32 verts, u32 idx) {
    if (m->vertex_count + verts > m->vcap) {
        while (m->vertex_count + verts > m->vcap) m->vcap = m->vcap ? m->vcap * 2 : 1024;
        m->pos = (float *)realloc(m->pos, sizeof(float) * 3 * m->vcap);
    }
    if (m->index_count + idx > m->icap) {
        while (m->index_count + idx > m->icap) m->icap = m->icap ? m->icap * 2 : 3072;
        m->indices = (u32 *)realloc(m->indices, sizeof(u32) * m->icap);
    }
}

// ------------------------------------------------------------------ noise

static u32 hash3(i32 x, i32 y, i32 z, u32 seed) {
    u32 h = seed * 0x9E3779B1u ^ (u32)x * 0x85EBCA77u ^ (u32)y * 0xC2B2AE3Du ^ (u32)z * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

static float vnoise(float x, float y, float z, u32 seed) {
    float fx = floorf(x), fy = floorf(y), fz = floorf(z);
    i32 ix = (i32)fx, iy = (i32)fy, iz = (i32)fz;
    float tx = x - fx, ty = y - fy, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    tz = tz * tz * (3 - 2 * tz);
    float v[8];
    for (int i = 0; i < 8; i++) v[i] = (float)(hash3(ix + (i & 1), iy + ((i >> 1) & 1), iz + (i >> 2), seed) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
    float a = v[0] + (v[1] - v[0]) * tx, b = v[2] + (v[3] - v[2]) * tx, c = v[4] + (v[5] - v[4]) * tx, d = v[6] + (v[7] - v[6]) * tx;
    float e = a + (b - a) * ty, f = c + (d - c) * ty;
    return e + (f - e) * tz;
}

static float fbm(const float p[3], u32 seed) {
    float s = 0, amp = 0.5f, fr = 1.0f;
    for (int o = 0; o < 5; o++) {
        s += amp * vnoise(p[0] * fr, p[1] * fr, p[2] * fr, seed + (u32)o * 101u);
        amp *= 0.5f;
        fr *= 2.03f;
    }
    return s;
}

// ------------------------------------------------------------------ shapes

static const float k_ico_v[12][3] = {
    { -1, 1.6180340f, 0 }, { 1, 1.6180340f, 0 }, { -1, -1.6180340f, 0 }, { 1, -1.6180340f, 0 },
    { 0, -1, 1.6180340f }, { 0, 1, 1.6180340f }, { 0, -1, -1.6180340f }, { 0, 1, -1.6180340f },
    { 1.6180340f, 0, -1 }, { 1.6180340f, 0, 1 }, { -1.6180340f, 0, -1 }, { -1.6180340f, 0, 1 },
};
static const u8 k_ico_f[20][3] = {
    { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
    { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 },
};

// Point on the subdivided face: weights over the face corners summed in ascending corner-index order,
// so every face sharing an edge / corner produces the same bits.
static void ico_point(const u8 f[3], u32 i, u32 j, u32 n, float out[3]) {
    u32 w[3] = { n - i - j, i, j };
    u8 order[3] = { 0, 1, 2 };
    for (int a = 1; a < 3; a++)
        for (int b = a; b > 0 && f[order[b]] < f[order[b - 1]]; b--) { u8 t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
    double p[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        u8 c = order[k];
        if (!w[c]) continue;
        for (int d = 0; d < 3; d++) p[d] += (double)k_ico_v[f[c]][d] * (double)w[c];
    }
    double l = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    for (int d = 0; d < 3; d++) out[d] = (float)(p[d] / l);
}

void vgeo_gen_rock(VgGenMesh *m, u32 seed, u32 n, float r, const float c[3], float displace) {
    if (n < 1) n = 1;
    u32 per_face_v = (n + 1) * (n + 2) / 2;
    reserve(m, 20 * per_face_v, 20 * n * n * 3);
    for (int f = 0; f < 20; f++) {
        u32 base = m->vertex_count;
        // Vertex (i, j) of the face grid at row-major index.
        for (u32 i = 0; i <= n; i++)
            for (u32 j = 0; j + i <= n; j++) {
                float d[3];
                ico_point(k_ico_f[f], i, j, n, d);
                float q[3] = { d[0] * 2.3f, d[1] * 2.3f, d[2] * 2.3f };
                float h = 1.0f + displace * fbm(q, seed);
                float *p = &m->pos[m->vertex_count * 3];
                for (int k = 0; k < 3; k++) p[k] = c[k] + d[k] * r * h;
                m->vertex_count++;
            }
        // row i starts at sum_{a<i} (n + 1 - a)
        #define VI(i, j) (base + (i) * (n + 1) - (i) * ((i) - 1) / 2 + (j))
        for (u32 i = 0; i < n; i++)
            for (u32 j = 0; j + i < n; j++) {
                u32 *t = &m->indices[m->index_count];
                t[0] = VI(i, j);
                t[1] = VI(i + 1, j);
                t[2] = VI(i, j + 1);
                m->index_count += 3;
                if (j + i + 1 < n) {
                    t = &m->indices[m->index_count];
                    t[0] = VI(i + 1, j);
                    t[1] = VI(i + 1, j + 1);
                    t[2] = VI(i, j + 1);
                    m->index_count += 3;
                }
            }
        #undef VI
    }
}

void vgeo_gen_torus(VgGenMesh *m, u32 seed, float R, float r, u32 su, u32 sv, const float c[3], float displace) {
    reserve(m, su * sv, su * sv * 6);
    u32 base = m->vertex_count;
    for (u32 u = 0; u < su; u++)
        for (u32 v = 0; v < sv; v++) {
            double a = 6.283185307179586 * (double)u / (double)su, b = 6.283185307179586 * (double)v / (double)sv;
            float dir[3] = { (float)(cos(a) * cos(b)), (float)sin(b), (float)(sin(a) * cos(b)) };
            float q[3] = { (float)cos(a) * 3.0f, (float)sin(b) * 3.0f, (float)sin(a) * 3.0f };
            float rr = r * (1.0f + displace * fbm(q, seed));
            float *p = &m->pos[m->vertex_count * 3];
            p[0] = c[0] + (float)(cos(a) * R) + dir[0] * rr;
            p[1] = c[1] + dir[1] * rr;
            p[2] = c[2] + (float)(sin(a) * R) + dir[2] * rr;
            m->vertex_count++;
        }
    for (u32 u = 0; u < su; u++)
        for (u32 v = 0; v < sv; v++) {
            u32 a = base + u * sv + v, b = base + ((u + 1) % su) * sv + v, cc = base + ((u + 1) % su) * sv + (v + 1) % sv, d = base + u * sv + (v + 1) % sv;
            u32 *t = &m->indices[m->index_count];
            t[0] = a; t[1] = d; t[2] = b;
            t[3] = b; t[4] = d; t[5] = cc;
            m->index_count += 6;
        }
}

void vgeo_gen_rock_field(VgGenMesh *m, u32 seed, u64 tris) {
    // Rocks of ~80k triangles (n = 63 -> 79 380), as many as needed, on a jittered grid.
    u32 n = 63;
    u64 per = 20ull * n * n;
    u32 count = (u32)((tris + per - 1) / per);
    if (count < 1) count = 1;
    u32 side = (u32)ceil(sqrt((double)count));
    for (u32 i = 0; i < count; i++) {
        u32 h = hash3((i32)i, 7, 13, seed);
        float r = 1.5f + (float)(h & 0xFF) / 255.0f * 2.5f;
        float c[3] = { ((float)(i % side) - (float)side * 0.5f) * 9.0f + (float)((h >> 8) & 0xFF) / 255.0f * 3.0f, r * 0.6f,
                       ((float)(i / side) - (float)side * 0.5f) * 9.0f + (float)((h >> 16) & 0xFF) / 255.0f * 3.0f };
        vgeo_gen_rock(m, seed + i * 31u, n, r, c, 0.35f);
    }
}

void vgeo_gen_statue(VgGenMesh *m, u32 seed, u64 tris) {
    // ~55 % body, ~15 % head, ~30 % in three rings.
    u32 nb = (u32)sqrt((double)tris * 0.55 / 20.0), nh = (u32)sqrt((double)tris * 0.15 / 20.0);
    float body[3] = { 0, 1.4f, 0 }, head[3] = { 0, 3.0f, 0 };
    vgeo_gen_rock(m, seed, nb, 1.0f, body, 0.18f);
    vgeo_gen_rock(m, seed + 1, nh, 0.45f, head, 0.12f);
    u32 ring = (u32)sqrt((double)tris * 0.10 / 2.0);
    for (int k = 0; k < 3; k++) {
        float c[3] = { 0, 0.5f + (float)k * 0.9f, 0 };
        vgeo_gen_torus(m, seed + 7u + (u32)k, 1.25f - (float)k * 0.15f, 0.12f, ring * 2, ring / 2 + 3, c, 0.25f);
    }
}
