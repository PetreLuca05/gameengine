// Procedural building for the vgeo test scene (docs/VGEO.md "Test content"): four brick walls of a
// square footprint, each one continuous grid displaced by a bevelled brick relief (running bond,
// mortar grooves), so the DAG can simplify across bricks. Triangles wind counter-clockwise seen from
// outside (glTF convention).
#include "vgeo_procgen.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BRICK_W 0.24f
#define BRICK_H 0.075f
#define MORTAR 0.010f    // groove width
#define RELIEF 0.008f    // groove depth

// Outward offset of the wall surface at (u along the wall, v up): 0 in the grooves, RELIEF on brick
// faces, smoothstep bevels in between.
static float brick_relief(float u, float v, u32 seed) {
    float row = floorf(v / BRICK_H);
    float off = fmodf(row, 2.0f) * 0.5f * BRICK_W + (float)(seed % 7u) * 0.013f;
    float bu = fmodf(u + off, BRICK_W), bv = v - row * BRICK_H;
    if (bu < 0) bu += BRICK_W;
    float du = fminf(bu, BRICK_W - bu), dv = fminf(bv, BRICK_H - bv);   // distance to the brick edge
    float d = fminf(du, dv);
    float t = (d - MORTAR * 0.5f) / (MORTAR * 0.6f);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    t = t * t * (3.0f - 2.0f * t);
    // per-brick variation (a few mm)
    u32 h = (u32)(i32)floorf((u + off) / BRICK_W) * 73856093u ^ (u32)(i32)row * 19349663u ^ seed * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    float var = (float)(h & 0xFFu) / 255.0f * 0.003f;
    return t * (RELIEF + var);
}

void vgeo_gen_building(VgGenMesh *m, u32 seed, u64 tris) {
    const float side = 16.0f, height = 8.0f;
    // cell size from the triangle budget: 4 walls x (side / c) x (height / c) x 2
    float cell = sqrtf(4.0f * side * height * 2.0f / (float)(tris ? tris : 2000000ull));
    u32 nu = (u32)ceilf(side / cell), nv = (u32)ceilf(height / cell);
    u32 base_v = m->vertex_count;
    u32 vcount = 4u * (nu + 1) * (nv + 1), icount = 4u * nu * nv * 6u;
    if (m->vertex_count + vcount > m->vcap) {
        m->vcap = m->vertex_count + vcount;
        m->pos = (float *)realloc(m->pos, sizeof(float) * 3 * m->vcap);
    }
    if (m->index_count + icount > m->icap) {
        m->icap = m->index_count + icount;
        m->indices = (u32 *)realloc(m->indices, sizeof(u32) * m->icap);
    }
    // walls: origin corner, along direction (unit), outward normal
    static const float corner[4][2] = { { -8, 8 }, { 8, 8 }, { 8, -8 }, { -8, -8 } };
    static const float along[4][2] = { { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 } };
    static const float out[4][2] = { { 0, 1 }, { 1, 0 }, { 0, -1 }, { -1, 0 } };
    for (u32 w = 0; w < 4; w++) {
        u32 wb = m->vertex_count;
        for (u32 j = 0; j <= nv; j++)
            for (u32 i = 0; i <= nu; i++) {
                float u = side * (float)i / (float)nu, v = height * (float)j / (float)nv;
                float r = brick_relief(u, v, seed + w);
                float *p = &m->pos[m->vertex_count * 3];
                p[0] = corner[w][0] + along[w][0] * u + out[w][0] * r;
                p[1] = v;
                p[2] = corner[w][1] + along[w][1] * u + out[w][1] * r;
                m->vertex_count++;
            }
        for (u32 j = 0; j < nv; j++)
            for (u32 i = 0; i < nu; i++) {
                u32 a = wb + j * (nu + 1) + i, b = a + 1, c = a + (nu + 1), d = c + 1;
                // along x up = out: (a, b, d) and (a, d, c) are counter-clockwise from outside
                u32 *t = &m->indices[m->index_count];
                t[0] = a; t[1] = b; t[2] = d;
                t[3] = a; t[4] = d; t[5] = c;
                m->index_count += 6;
            }
    }
    (void)base_v;
}
