#include "vgeo_procgen_foliage.h"
#include "foliage_wind.h"
#include "vgeo_impostor.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define MAT_TEXTURED_FLAG (1u << 6)   // render.h MAT_TEXTURED: the lit resolve multiplies the albedo texture in

static u32 g_rng;
static float rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 16777216.0f;
}

static u32 rgba(float r, float g, float b, float a) {
    int c[4] = { (int)lrintf(r * 255), (int)lrintf(g * 255), (int)lrintf(b * 255), (int)lrintf(a * 255) };
    for (int k = 0; k < 4; k++) c[k] = c[k] < 0 ? 0 : (c[k] > 255 ? 255 : c[k]);
    return (u32)c[0] | (u32)c[1] << 8 | (u32)c[2] << 16 | (u32)c[3] << 24;
}

static u32 add_vertex(VgFoliageMesh *m, const float p[3], const float n[3], float u, float v, u32 col) {
    if (m->vertex_count == m->vcap) {
        m->vcap = m->vcap ? m->vcap * 2 : 256;
        m->pos = (float *)realloc(m->pos, sizeof(float) * 3 * m->vcap);
        m->normal = (float *)realloc(m->normal, sizeof(float) * 3 * m->vcap);
        m->uv = (float *)realloc(m->uv, sizeof(float) * 2 * m->vcap);
        m->color = (u32 *)realloc(m->color, sizeof(u32) * m->vcap);
    }
    u32 i = m->vertex_count++;
    memcpy(&m->pos[i * 3], p, 12);
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; k++) m->normal[i * 3 + k] = l > 0 ? n[k] / l : (k == 1);
    m->uv[i * 2] = u;
    m->uv[i * 2 + 1] = v;
    m->color[i] = col;
    return i;
}

static void add_tri(VgFoliageMesh *m, u32 a, u32 b, u32 c, u16 mat) {
    if (m->index_count + 3 > m->icap) {
        m->icap = m->icap ? m->icap * 2 : 768;
        m->indices = (u32 *)realloc(m->indices, sizeof(u32) * m->icap);
        m->tri_material = (u16 *)realloc(m->tri_material, sizeof(u16) * (m->icap / 3));
    }
    m->tri_material[m->index_count / 3] = mat;
    m->indices[m->index_count++] = a;
    m->indices[m->index_count++] = b;
    m->indices[m->index_count++] = c;
}

// A card: 2 x 3 quads (12 triangles) spanning centre +- right * w/2, from base to base + up * h,
// bent along `bend` (m at the top, quadratic), CCW seen from the front (normal side).
typedef float (*WeightFn)(float u, float v, const float p[3]);
static float g_card_phase;   // the card being added: its branch phase (G), from its base position
static float pos_phase(const float p[3]) {
    u32 h = (u32)(i32)floorf(p[0] * 7.0f) * 0x8DA6B343u ^ (u32)(i32)floorf(p[1] * 7.0f) * 0xD8163841u ^ (u32)(i32)floorf(p[2] * 7.0f) * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (float)(h & 0xFFFFu) / 65536.0f;
}
static void add_card(VgFoliageMesh *m, const float base[3], const float right[3], const float up[3], float w, float h, const float bend[3],
                     const float nrm_blend[3], float blend, u16 mat, u32 (*color)(float u, float v, const float p[3])) {
    float n[3] = { right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2], right[0] * up[1] - right[1] * up[0] };
    u32 first = m->vertex_count;
    g_card_phase = pos_phase(base);
    for (int j = 0; j <= 3; j++)
        for (int i = 0; i <= 2; i++) {
            float u = (float)i / 2.0f, v = (float)j / 3.0f, p[3], nn[3];
            for (int k = 0; k < 3; k++) {
                p[k] = base[k] + right[k] * (u - 0.5f) * w + up[k] * v * h + bend[k] * v * v;
                nn[k] = n[k] * (1.0f - blend) + nrm_blend[k] * blend;
            }
            add_vertex(m, p, nn, u, 1.0f - v, color(u, v, p));
        }
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 2; i++) {
            u32 a = first + (u32)(j * 3 + i), b = a + 1, c = a + 3, d = a + 4;
            add_tri(m, a, b, d, mat);
            add_tri(m, a, d, c, mat);
        }
}

// Wind weights in the canonical layout (foliage_wind.h): R flutter (0 stem .. tip), G branch phase,
// B branch bend weight (0 on the trunk). Trunk bending comes from the height (the material's H).
static u32 tuft_color(float u, float v, const float p[3]) {
    (void)u;
    (void)p;
    return rgba(v, g_card_phase, 0.0f, 1.0f);   // blades: flutter towards the tip, no branches
}

static u32 bush_trunk_color(float u, float v, const float p[3]) {
    (void)u;
    (void)v;
    (void)p;
    return rgba(0.0f, 0.0f, 0.0f, 1.0f);   // the stem: main bending only
}
static u32 bush_leaf_color(float u, float v, const float p[3]) {
    (void)u;
    (void)v;
    float r = sqrtf(p[0] * p[0] + p[2] * p[2]);
    return rgba(0.8f * v, g_card_phase, fminf(r / 0.7f, 1.0f), 1.0f);
}

static void gen_tuft(VgFoliageMesh *m) {
    const int cards = 5;
    for (int k = 0; k < cards; k++) {
        float a = (float)k * PI / (float)cards + rnd() * 0.3f;
        float right[3] = { cosf(a), 0.0f, sinf(a) }, n[3] = { -sinf(a), 0.0f, cosf(a) };
        float lean = (rnd() - 0.5f) * 0.25f;
        float up[3] = { n[0] * lean, 1.0f, n[2] * lean };
        float base[3] = { (rnd() - 0.5f) * 0.08f, 0.0f, (rnd() - 0.5f) * 0.08f };
        float bend[3] = { n[0] * (rnd() - 0.5f) * 0.15f, 0.0f, n[2] * (rnd() - 0.5f) * 0.15f };
        float upn[3] = { 0, 1, 0 };
        add_card(m, base, right, up, 0.45f + rnd() * 0.1f, 0.5f + rnd() * 0.15f, bend, upn, 0.45f, 0, tuft_color);
    }
    m->material_count = 1;
}

static void gen_bush(VgFoliageMesh *m) {
    // trunk: tapered tube, 6 sides x 4 rings (48 triangles), uv along it
    const int sides = 6, rings = 4;
    const float height = 0.8f;
    u32 first = m->vertex_count;
    for (int j = 0; j <= rings; j++)
        for (int i = 0; i <= sides; i++) {
            float v = (float)j / (float)rings, a = (float)i / (float)sides * 2.0f * PI, r = 0.07f * (1.0f - 0.6f * v);
            float p[3] = { cosf(a) * r + sinf(v * 2.1f) * 0.03f, v * height, sinf(a) * r }, n[3] = { cosf(a), 0.15f, sinf(a) };
            add_vertex(m, p, n, (float)i / (float)sides, v, bush_trunk_color(0, 0, p));
        }
    for (int j = 0; j < rings; j++)
        for (int i = 0; i < sides; i++) {
            u32 a = first + (u32)(j * (sides + 1) + i), b = a + 1, c = a + (u32)(sides + 1), d = c + 1;
            add_tri(m, a, c, d, 0);   // CCW outward
            add_tri(m, a, d, b, 0);
        }
    // leaf cards on a squashed sphere around the crown
    const float cc[3] = { 0.0f, 1.0f, 0.0f };
    for (int k = 0; k < 46; k++) {
        float z = rnd() * 2.0f - 1.0f, a = rnd() * 2.0f * PI, s = sqrtf(1.0f - z * z);
        float dir[3] = { s * cosf(a), z * 0.7f + 0.1f, s * sinf(a) };
        float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int q = 0; q < 3; q++) dir[q] /= dl;
        float rr = 0.25f + rnd() * 0.4f;
        float c[3] = { cc[0] + dir[0] * rr, cc[1] + dir[1] * rr * 0.8f, cc[2] + dir[2] * rr };
        // card facing roughly outwards, random roll
        float t0[3] = { -dir[2], 0.0f, dir[0] };
        float tl = sqrtf(t0[0] * t0[0] + t0[2] * t0[2]);
        if (tl < 1e-3f) { t0[0] = 1; t0[2] = 0; tl = 1; }
        t0[0] /= tl;
        t0[2] /= tl;
        float b0[3] = { dir[1] * t0[2] - dir[2] * t0[1], dir[2] * t0[0] - dir[0] * t0[2], dir[0] * t0[1] - dir[1] * t0[0] };
        float roll = (rnd() - 0.5f) * 1.6f, cr = cosf(roll), sr = sinf(roll);
        float right[3], up[3];
        for (int q = 0; q < 3; q++) {
            right[q] = t0[q] * cr + b0[q] * sr;
            up[q] = -t0[q] * sr + b0[q] * cr;
        }
        float w = 0.38f + rnd() * 0.14f, h = 0.38f + rnd() * 0.14f;
        float base[3] = { c[0] - up[0] * h * 0.5f, c[1] - up[1] * h * 0.5f, c[2] - up[2] * h * 0.5f };
        float bend[3] = { dir[0] * 0.06f, dir[1] * 0.06f, dir[2] * 0.06f };
        add_card(m, base, right, up, w, h, bend, dir, 0.6f, 1, bush_leaf_color);
    }
    m->material_count = 2;
}

// ------------------------------------------------------------------ map kinds (crops)

// Crop clump: 6 crossed tall cards (wheat stalks with heads in the mask).
static void gen_crop(VgFoliageMesh *m) {
    for (int k = 0; k < 6; k++) {
        float ang = (float)k * PI / 6.0f + rnd() * 0.3f;
        float right[3] = { cosf(ang), 0.0f, sinf(ang) }, n[3] = { -sinf(ang), 0.0f, cosf(ang) };
        float lean = (rnd() - 0.5f) * 0.15f;
        float up[3] = { n[0] * lean, 1.0f, n[2] * lean };
        float base[3] = { (rnd() - 0.5f) * 0.2f, 0.0f, (rnd() - 0.5f) * 0.2f };
        float bend[3] = { n[0] * (rnd() - 0.5f) * 0.2f, 0.0f, n[2] * (rnd() - 0.5f) * 0.2f };
        float upn[3] = { 0, 1, 0 };
        add_card(m, base, right, up, 0.6f + rnd() * 0.1f, 1.25f + rnd() * 0.2f, bend, upn, 0.4f, 0, tuft_color);
    }
    m->material_count = 1;
}

void vgeo_gen_foliage(VgFoliageMesh *m, int kind, u32 seed) {
    memset(m, 0, sizeof(*m));
    g_rng = seed * 2654435761u + 17u;
    switch (kind) {
    case VG_FOLIAGE_BUSH: gen_bush(m); break;
    case VG_FOLIAGE_CROP: gen_crop(m); break;
    default: gen_tuft(m); break;
    }
}

bool vgeo_foliage_capsule(int kind, float *radius, float *height) {
    switch (kind) {
    case VG_FOLIAGE_BUSH: *radius = 0.08f; *height = 0.8f; return true;
    default: return false;
    }
}

void vgeo_foliage_mesh_free(VgFoliageMesh *m) {
    free(m->pos);
    free(m->normal);
    free(m->uv);
    free(m->color);
    free(m->indices);
    free(m->tri_material);
    memset(m, 0, sizeof(*m));
}

void vgeo_foliage_source(const VgFoliageMesh *m, VgSource *s) {
    static const char names[2][48] = { "foliage_a", "foliage_b" };
    memset(s, 0, sizeof(*s));
    s->vertex_count = m->vertex_count;
    s->pos = m->pos;
    s->normal = m->normal;
    s->uv = m->uv;
    s->color = m->color;
    s->index_count = m->index_count;
    s->indices = m->indices;
    s->tri_material = m->tri_material;
    s->material_count = m->material_count;
    s->material_names = names;
}

u32 vgeo_foliage_materials(int kind, VgMaterial out[2]) {
    memset(out, 0, sizeof(VgMaterial) * 2);
    for (int i = 0; i < 2; i++) {
        out[i].tex_scale = 1.0f;
        out[i].roughness = 0.95f;   // grass / wheat / bush leaves: matte (no sky sheen on the cards)
        out[i].color[3] = 1.0f;
    }
    if (kind == VG_FOLIAGE_BUSH) {
        VgMaterial *t = &out[0], *l = &out[1];
        t->color[0] = 0.32f, t->color[1] = 0.22f, t->color[2] = 0.13f;
        t->roughness = 0.9f;
        t->flags = VGM_FOLIAGE | VGM_DITHER_FADE;
        l->color[0] = l->color[1] = l->color[2] = 1.0f;   // the leaf texture carries the colour
        l->flags = VGM_FOLIAGE | VGM_MASKED | VGM_TWO_SIDED | VGM_DITHER_FADE | MAT_TEXTURED_FLAG;
        l->alpha_cutoff = 0.5f;
        VgWindParams w;
        vgeo_wind_defaults(&w, VGWP_BUSH, 1.6f);
        for (int i = 0; i < 2; i++) {
            out[i].wpo = WPO_BUSH | WPO_DISPLACE;   // pushed through: the displacement buffer first, then the wind
            vgeo_wind_pack(&w, out[i].wpo_params, out[i].wind_params);
            out[i].fade_start = 100.0f;
            out[i].fade_end = 150.0f;
            out[i].shadow_max_dist = 40.0f;
            vg_material_set_ao(&out[i], 0.0f, MAT_AO_COMBINE_AUTO, false);   // leaves 0.3 (thin), the trunk full
        }
        return 2;
    }
    VgMaterial *g = &out[0];
    g->color[0] = g->color[1] = g->color[2] = 1.0f;
    g->flags = VGM_FOLIAGE | VGM_MASKED | VGM_TWO_SIDED | VGM_DITHER_FADE | MAT_TEXTURED_FLAG;
    g->alpha_cutoff = 0.5f;
    g->wpo = WPO_GRASS | WPO_DISPLACE;
    VgWindParams w;
    vgeo_wind_defaults(&w, VGWP_GRASS, kind == VG_FOLIAGE_CROP ? 1.45f : 0.65f);   // blade height (the main bend's H)
    if (kind == VG_FOLIAGE_CROP) w.trunk_stiffness = 0.35f, w.main_freq = 0.28f, w.max_bend_deg = 25.0f;   // heavy ears: slower, stiffer
    vgeo_wind_pack(&w, g->wpo_params, g->wind_params);
    g->fade_start = 40.0f;
    g->fade_end = 65.0f;
    g->shadow_max_dist = 15.0f;
    if (kind == VG_FOLIAGE_CROP) {
        g->fade_start = 60.0f;
        g->fade_end = 95.0f;
        g->shadow_max_dist = 20.0f;
    }
    vg_material_set_ao(g, MATERIAL_AO_FOLIAGE, MAT_AO_COMBINE_MIN, false);   // thin cards (render/material_ao.h)
    return 1;
}

// Smooth value noise in [0, 1] (lattice of `cells` per side, wrapping).
static float vnoise(float x, float y, u32 cells, u32 seed) {
    float fx = floorf(x), fy = floorf(y), tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    float v[4];
    for (int k = 0; k < 4; k++) {
        u32 ix = ((u32)((int)fx + (k & 1)) % cells), iy = ((u32)((int)fy + (k >> 1)) % cells);
        u32 h = (ix * 73856093u) ^ (iy * 19349663u) ^ (seed * 83492791u);
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        v[k] = (float)(h & 0xFFFFu) / 65535.0f;
    }
    return (v[0] * (1 - tx) + v[1] * tx) * (1 - ty) + (v[2] * (1 - tx) + v[3] * tx) * ty;
}

static float srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

bool vgeo_gen_foliage_mask(u8 *px, u32 size, int kind, u32 material, u32 seed) {
    bool masked = kind == VG_FOLIAGE_TUFT || kind == VG_FOLIAGE_CROP || (kind == VG_FOLIAGE_BUSH && material == 1);
    if (!masked) return false;
    g_rng = seed * 747796405u + 1u;
    float *a = (float *)calloc((size_t)size * size, sizeof(float));
    float *shade = (float *)calloc((size_t)size * size, sizeof(float));
    if (kind == VG_FOLIAGE_CROP) {
    } else if (kind == VG_FOLIAGE_CROP) {
        // wheat: thin stalks from the bottom edge, each topped by an ear
        for (int b = 0; b < 16; b++) {
            float cx = (float)(b + 0.5f) / 16.0f + (rnd() - 0.5f) * 0.04f, hgt = 0.72f + rnd() * 0.25f, lean = (rnd() - 0.5f) * 0.12f;
            float tone = 0.8f + rnd() * 0.2f;
            for (u32 y = 0; y < size; y++) {
                float v = 1.0f - ((float)y + 0.5f) / (float)size;
                if (v > hgt) continue;
                float t = v / hgt, c = cx + lean * t * t;
                bool ear = t > 0.78f;
                float hw = ear ? 0.016f * sinf((t - 0.78f) / 0.22f * PI) + 0.004f : 0.0045f;
                for (u32 x = 0; x < size; x++) {
                    float u = ((float)x + 0.5f) / (float)size, d = fabsf(u - c) / hw;
                    if (d >= 1.0f) continue;
                    size_t i = (size_t)y * size + x;
                    a[i] = 2.0f;
                    shade[i] = tone * (ear ? 1.15f : 0.85f);
                }
            }
        }
    } else if (kind == VG_FOLIAGE_TUFT) {
        // grass blades: tapered, leaning strips from the bottom edge, widths broken up by noise
        for (int b = 0; b < 14; b++) {
            float cx = (float)(b + 0.5f) / 14.0f + (rnd() - 0.5f) * 0.05f, hgt = 0.65f + rnd() * 0.35f, wid = 0.025f + rnd() * 0.02f, lean = (rnd() - 0.5f) * 0.25f;
            float tone = 0.75f + rnd() * 0.25f;
            for (u32 y = 0; y < size; y++) {
                float v = 1.0f - ((float)y + 0.5f) / (float)size;   // 0 bottom, 1 top
                if (v > hgt) continue;
                float t = v / hgt, c = cx + lean * t * t, hw = wid * (1.0f - t) + 0.002f;
                for (u32 x = 0; x < size; x++) {
                    float u = ((float)x + 0.5f) / (float)size;
                    float d = fabsf(u - c) / hw;
                    if (d >= 1.0f) continue;
                    float n = vnoise(u * 32.0f, v * 8.0f, 32, seed + 3);
                    float val = (1.0f - d) * 2.2f * (0.75f + 0.5f * n);
                    size_t i = (size_t)y * size + x;
                    if (val > a[i]) { a[i] = val; shade[i] = tone * (0.55f + 0.45f * t); }
                }
            }
        }
    } else {
        // leaves: a few overlapping ellipses with a midrib, noisy edges
        for (int l = 0; l < 7; l++) {
            float cx = 0.2f + rnd() * 0.6f, cy = 0.2f + rnd() * 0.6f, rx = 0.12f + rnd() * 0.08f, ry = 0.2f + rnd() * 0.1f, ang = rnd() * PI;
            float ca = cosf(ang), sa = sinf(ang), tone = 0.7f + rnd() * 0.3f;
            for (u32 y = 0; y < size; y++)
                for (u32 x = 0; x < size; x++) {
                    float u = ((float)x + 0.5f) / (float)size - cx, v = ((float)y + 0.5f) / (float)size - cy;
                    float lu = (u * ca + v * sa) / rx, lv = (-u * sa + v * ca) / ry;
                    float d = lu * lu + lv * lv;
                    float n = vnoise(((float)x + 0.5f) / (float)size * 24.0f, ((float)y + 0.5f) / (float)size * 24.0f, 24, seed + 7);
                    float val = (1.0f - d) * 3.0f + (n - 0.5f) * 1.2f;
                    size_t i = (size_t)y * size + x;
                    if (val > a[i]) { a[i] = val; shade[i] = tone * (fabsf(lu) < 0.06f ? 0.7f : 1.0f); }
                }
        }
    }
    for (u32 y = 0; y < size; y++)
        for (u32 x = 0; x < size; x++) {
            size_t i = (size_t)y * size + x;
            float al = a[i] < 0 ? 0 : (a[i] > 1 ? 1 : a[i]);
            float s = shade[i] > 0 ? shade[i] : 0.6f;
            float r = kind == VG_FOLIAGE_TUFT ? 0.20f * s : 0.16f * s, g = kind == VG_FOLIAGE_TUFT ? 0.42f * s : 0.36f * s, bb = 0.08f * s;
            if (kind == VG_FOLIAGE_CROP) r = 0.55f * s, g = 0.45f * s, bb = 0.2f * s;
            px[i * 4] = (u8)lrintf(srgb(r) * 255);
            px[i * 4 + 1] = (u8)lrintf(srgb(g) * 255);
            px[i * 4 + 2] = (u8)lrintf(srgb(bb) * 255);
            px[i * 4 + 3] = (u8)lrintf(al * 255);
        }
    free(a);
    free(shade);
    return true;
}

void vgeo_foliage_impostor_materials(int kind, const u8 *const masks[2], u32 size, VgImpostorMaterial out[2]) {
    VgMaterial m[2];
    u32 n = vgeo_foliage_materials(kind, m);
    memset(out, 0, sizeof(VgImpostorMaterial) * 2);
    for (u32 i = 0; i < n; i++) {
        memcpy(out[i].color, m[i].color, 16);
        out[i].tex_scale = m[i].tex_scale;
        out[i].masked = (m[i].flags & VGM_MASKED) != 0;
        out[i].cutoff = m[i].alpha_cutoff;
        if (masks && masks[i]) {
            out[i].rgba = masks[i];
            out[i].w = out[i].h = size;
        }
    }
}

const char *vgeo_foliage_name(int kind) {
    static const char *const names[VG_FOLIAGE_ALL_KINDS] = { "test_tuft", "test_bush", "crop_wheat" };
    return kind >= 0 && kind < VG_FOLIAGE_ALL_KINDS ? names[kind] : "test_tuft";
}
