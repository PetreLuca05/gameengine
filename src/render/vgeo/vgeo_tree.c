// Imported tree / bush foliage (vgeo_tree.h, docs/VGEO.md "Tree models").
#include "vgeo_tree.h"
#include "vgeo_impostor.h"
#include "core/log.h"
#include "core/platform.h"
#include "stb_image.h"
#include "ufbx.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAT_TEXTURED_FLAG (1u << 6)   // render.h MAT_TEXTURED

// The species (assets/treefbx, Tree It exports: 0.85-14 m models in centimetres, docs/VGEO.md "Tree
// models"). Each has a target height range in metres, from published mature heights (sources in the
// doc): the mesh is scaled to the middle of the range (instance scale 1) and the scatter picks an
// instance height in the range (vgeo_tree_scale_range). Forest trees sit in the lower-middle part of the
// mature range (stands, not record trees). Conifers replace the conifer role, broadleaves the broadleaf
// role, the old rooted snag the dead role; juniper, bamboo and a small acacia are the underbrush / meadow
// bushes, daisies the meadow flowers. Trees switch to their impostor at 6x r_vgeo_impostor_px (96 px
// across by default: their cards do not coarsen, a distant tree would stay thousands of triangles),
// bushes at 2x.
static const VgTreeSpecies k_species[] = {
    //  name           file          role               w     height (m)     fade      shadow atlas imp. cap
    { "tree_pine",     "pine.fbx",   VG_TREE_CONIFER,   1.0f, 18.0f, 32.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_fir",      "fir.fbx",    VG_TREE_CONIFER,   1.0f, 20.0f, 36.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_spruce",   "spruce.fbx", VG_TREE_CONIFER,   1.0f, 20.0f, 36.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_oak",      "oak.fbx",    VG_TREE_BROADLEAF, 1.0f, 16.0f, 28.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_beech",    "beech.fbx",  VG_TREE_BROADLEAF, 1.0f, 20.0f, 34.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_birch",    "birch.fbx",  VG_TREE_BROADLEAF, 0.8f, 14.0f, 24.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_elm",      "elm.fbx",    VG_TREE_BROADLEAF, 0.8f, 16.0f, 27.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_maple",    "maple.fbx",  VG_TREE_BROADLEAF, 1.0f, 14.0f, 24.0f,  850, 1100, 70, 512, 6.0f, true },
    { "tree_old",      "old.fbx",    VG_TREE_DEAD,      1.0f, 10.0f, 17.0f,  850, 1100, 70, 512, 1.0f, true },
    { "bush_juniper",  "juniper.fbx", VG_TREE_BUSH,     1.0f, 3.0f,  7.0f,   150, 210, 40, 512, 2.0f, true },
    { "bush_bamboo",   "bamboo.obj", VG_TREE_BUSH,      0.4f, 2.5f,  5.0f,   150, 210, 40, 512, 2.0f, true },
    { "bush_acacia",   "tree1.obj",  VG_TREE_BUSH,      0.4f, 4.0f,  8.0f,   150, 210, 40, 512, 2.0f, true },
    { "flower_daisy",  "daisy.fbx",  VG_TREE_FLOWER,    1.0f, 0.35f, 0.75f,  50, 80, 15, 0, 1.0f, false },
};

// Wind tuning per species (docs/FOLIAGE_WIND.md "Parameters"; 0 = the preset's value). Old oaks are
// stiff, birch slender with a quick flutter, conifers have drooping branches and stiff needles, bamboo
// is very flexible and settles quickly. Fields: trunk / branch / leaf stiffness, full wind, main /
// detail / flutter Hz, max bend (deg), settle time (s), response.
typedef struct TreeWind { const char *name; float trunk, branch, leaf, full, main_hz, detail_hz, flutter_hz, max_bend, settle, response; } TreeWind;
static const TreeWind k_wind[] = {
    { "tree_pine",    0.045f, 0.35f, 0.03f,  0, 0.16f, 0, 4.0f, 0, 0, 0 },
    { "tree_fir",     0.045f, 0.38f, 0.03f,  0, 0.16f, 0, 4.0f, 0, 0, 0 },
    { "tree_spruce",  0.045f, 0.40f, 0.03f,  0, 0.16f, 0, 4.0f, 0, 0, 0 },
    { "tree_oak",     0.035f, 0.30f, 0.05f,  0, 0.14f, 0, 0, 5.0f, 0, 0 },
    { "tree_beech",   0.045f, 0.30f, 0.055f, 0, 0.17f, 0, 0, 0, 0, 0 },
    { "tree_birch",   0.07f,  0.35f, 0.07f,  18.0f, 0.22f, 0, 7.0f, 9.0f, 0, 0 },
    { "tree_elm",     0.05f,  0.28f, 0.055f, 0, 0, 0, 0, 0, 0, 0 },
    { "tree_maple",   0.045f, 0.30f, 0.06f,  0, 0.17f, 0, 0, 0, 0, 0 },
    { "tree_old",     0.03f,  0.25f, 0.04f,  0, 0.15f, 0, 0, 5.0f, 0, 0 },
    { "bush_juniper", 0.055f, 0.25f, 0.025f, 0, 0.20f, 0, 4.5f, 0, 0, 0 },
    { "bush_bamboo",  0.12f,  0.30f, 0.06f,  16.0f, 0.30f, 0, 0, 14.0f, 1.6f, 1.0f },
    { "bush_acacia",  0.04f,  0.30f, 0.05f,  0, 0, 0, 0, 0, 0, 0 },
    { "flower_daisy", 0.35f,  0, 0.03f,      10.0f, 0, 0, 0, 25.0f, 0, 0 },
};

u32 vgeo_tree_species_count(void) { return (u32)(sizeof(k_species) / sizeof(k_species[0])); }
const VgTreeSpecies *vgeo_tree_species(u32 i) { return i < vgeo_tree_species_count() ? &k_species[i] : NULL; }
float vgeo_tree_height(const VgTreeSpecies *sp) { return 0.5f * (sp->height_min + sp->height_max); }

void vgeo_tree_wind(const VgTreeSpecies *sp, VgWindParams *out) {
    vgeo_wind_defaults(out, sp->role == VG_TREE_FLOWER ? VGWP_GRASS : VGWP_TREE, vgeo_tree_height(sp));
    for (u32 i = 0; i < sizeof(k_wind) / sizeof(k_wind[0]); i++) {
        const TreeWind *w = &k_wind[i];
        if (strcmp(w->name, sp->name)) continue;
        if (w->trunk > 0) out->trunk_stiffness = w->trunk;
        if (w->branch > 0) out->branch_stiffness = w->branch;
        if (w->leaf > 0) out->leaf_stiffness = w->leaf;
        if (w->full > 0) out->full_wind = w->full;
        if (w->main_hz > 0) out->main_freq = w->main_hz;
        if (w->detail_hz > 0) out->detail_freq = w->detail_hz;
        if (w->flutter_hz > 0) out->flutter_freq = w->flutter_hz;
        if (w->max_bend > 0) out->max_bend_deg = w->max_bend;
        if (w->settle > 0) out->settle_time = w->settle;
        if (w->response > 0) out->response = w->response;
        break;
    }
}

bool vgeo_tree_scale_range(const char *name, float *lo, float *hi) {
    for (u32 i = 0; i < vgeo_tree_species_count(); i++) {
        const VgTreeSpecies *sp = &k_species[i];
        if (strcmp(sp->name, name)) continue;
        float h = vgeo_tree_height(sp);
        *lo = sp->height_min / h;
        *hi = sp->height_max / h;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures

bool vgeo_tree_texture_load(const char *path, u32 max_side, VgTreeTexture *out) {
    memset(out, 0, sizeof(*out));
    int w, h, comp;
    u8 *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px) return false;
    u32 cw = (u32)w, ch = (u32)h;
    u8 *cur = (u8 *)malloc((size_t)cw * ch * 4);
    memcpy(cur, px, (size_t)cw * ch * 4);
    stbi_image_free(px);
    // halve (2 x 2 box, colour weighted by alpha so the cut-out texels do not bleed in) until it fits
    while ((cw > max_side || ch > max_side) && cw > 1 && ch > 1) {
        u32 nw = cw / 2, nh = ch / 2;
        u8 *nx = (u8 *)malloc((size_t)nw * nh * 4);
        for (u32 y = 0; y < nh; y++)
            for (u32 x = 0; x < nw; x++) {
                float c[3] = { 0, 0, 0 }, a = 0, cs[3] = { 0, 0, 0 };
                for (u32 k = 0; k < 4; k++) {
                    const u8 *s = cur + ((size_t)(y * 2 + k / 2) * cw + x * 2 + k % 2) * 4;
                    float wa = (float)s[3] + 1e-3f;
                    for (int i = 0; i < 3; i++) { c[i] += (float)s[i] * wa; cs[i] += (float)s[i]; }
                    a += wa;
                }
                u8 *d = nx + ((size_t)y * nw + x) * 4;
                for (int i = 0; i < 3; i++) d[i] = (u8)(a > 0.01f ? c[i] / a + 0.5f : cs[i] / 4.0f + 0.5f);
                d[3] = (u8)((a - 4e-3f) / 4.0f + 0.5f);
            }
        free(cur);
        cur = nx;
        cw = nw;
        ch = nh;
    }
    out->rgba = cur;
    out->w = cw;
    out->h = ch;
    return true;
}

void vgeo_tree_mip_coverage(u8 *mips, u32 w, u32 h, u32 levels, float cutoff) {
    u8 ref = (u8)DG_CLAMP(cutoff * 255.0f + 0.5f, 1.0f, 255.0f);
    size_t n0 = (size_t)w * h, c0 = 0;
    for (size_t i = 0; i < n0; i++) c0 += mips[i * 4 + 3] >= ref;
    double target = (double)c0 / (double)n0;
    u8 *lv = mips + n0 * 4;
    for (u32 l = 1; l < levels; l++) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        size_t n = (size_t)w * h;
        // the alpha scale whose coverage at the cutoff is nearest the base level's (bisection)
        float lo = 0.25f, hi = 16.0f;
        for (int it = 0; it < 20; it++) {
            float sc = 0.5f * (lo + hi);
            size_t c = 0;
            for (size_t i = 0; i < n; i++) c += (float)lv[i * 4 + 3] * sc >= (float)ref;
            if ((double)c / (double)n < target) lo = sc;
            else hi = sc;
        }
        float sc = 0.5f * (lo + hi);
        for (size_t i = 0; i < n; i++) lv[i * 4 + 3] = (u8)DG_MIN((float)lv[i * 4 + 3] * sc + 0.5f, 255.0f);
        lv += n * 4;
    }
}

// ------------------------------------------------------------------ import

typedef struct TreeVert { float p[3], n[3], uv[2]; u32 col; } TreeVert;   // col: the canonical wind weights (part of the dedup key)

// "oak leaf_2" -> "oak leaf" (Tree It numbers its FBX models)
static void strip_suffix(const char *in, char *out, size_t n) {
    dg_strcpy(out, n, in);
    char *u = strrchr(out, '_');
    if (!u || !u[1]) return;
    for (const char *c = u + 1; *c; c++)
        if (*c < '0' || *c > '9') return;
    *u = 0;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static u32 uf_find(u32 *p, u32 x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

// Every triangle corner of the source (metres at the species height, the trunk base at the origin) with
// its source colour (RGBA 0..1) and connected piece (a branch, a card: corners sharing file vertices).
typedef struct TreeCorners {
    TreeVert *v;
    float *col;                  // 4 per corner
    u32 *piece;
    u16 *tmat;                   // per triangle
    size_t n;
    u32 pieces;
    bool has_color;
} TreeCorners;

static void corners_free(TreeCorners *c) {
    free(c->v), free(c->col), free(c->piece), free(c->tmat);
    memset(c, 0, sizeof(*c));
}

// Loads the source mesh: the corners, the materials (textures loaded, masked = the texture cuts out),
// card winding, metres, the trunk capsule.
static bool tree_load(const VgTreeSpecies *sp, const char *dir, TreeCorners *c, VgTree *meta, char *err, int err_size) {
    memset(c, 0, sizeof(*c));
    char path[600];
    snprintf(path, sizeof(path), "%s%s", dir, sp->file);
    ufbx_load_opts o;
    memset(&o, 0, sizeof(o));
    o.generate_missing_normals = true;
    ufbx_error e;
    ufbx_scene *s = ufbx_load_file(path, &o, &e);
    if (!s) {
        char b[256];
        ufbx_format_error(b, sizeof(b), &e);
        snprintf(err, (size_t)err_size, "%s: %s", path, b);
        return false;
    }
    // every triangle of every mesh node, one corner per vertex (deduplicated by the import)
    size_t cap = 0, total_pos = 0;
    for (size_t i = 0; i < s->nodes.count; i++)
        if (s->nodes.data[i]->mesh) cap += s->nodes.data[i]->mesh->num_triangles * 3, total_pos += s->nodes.data[i]->mesh->num_vertices;
    TreeVert *v = (TreeVert *)calloc(cap ? cap : 1, sizeof(TreeVert));
    float *col = (float *)malloc(sizeof(float) * 4 * (cap ? cap : 1));
    u32 *posid = (u32 *)malloc(sizeof(u32) * (cap ? cap : 1));
    u32 *uf = (u32 *)malloc(sizeof(u32) * (total_pos ? total_pos : 1));
    for (size_t i = 0; i < total_pos; i++) uf[i] = (u32)i;
    u16 *tmat = (u16 *)malloc(sizeof(u16) * (cap / 3 + 1));
    size_t nv = 0;
    u32 vbase = 0;
    meta->material_count = 0;
    u32 tri_idx[64];
    for (size_t i = 0; i < s->nodes.count; i++) {
        const ufbx_node *node = s->nodes.data[i];
        const ufbx_mesh *m = node->mesh;
        if (!m) continue;
        if (m->vertex_color.exists) c->has_color = true;
        ufbx_matrix nm = ufbx_matrix_for_normals(&node->geometry_to_world);
        for (size_t f = 0; f < m->faces.count; f++) {
            char name[48];
            if (m->face_material.count && m->materials.count) {
                u32 fm = m->face_material.data[f];
                strip_suffix(m->materials.data[fm < m->materials.count ? fm : 0]->name.data, name, sizeof(name));
            } else strip_suffix(node->name.data, name, sizeof(name));
            u32 mi = 0;
            while (mi < meta->material_count && strcmp(meta->tex[mi], name)) mi++;
            if (mi == meta->material_count) {
                if (mi >= VG_TREE_MAX_MATERIALS) mi = VG_TREE_MAX_MATERIALS - 1;   // more textures than slots: share the last
                else dg_strcpy(meta->tex[meta->material_count++], sizeof(meta->tex[0]), name);
            }
            u32 nt = ufbx_triangulate_face(tri_idx, 64, m, m->faces.data[f]);
            for (u32 t = 0; t < nt; t++) {
                for (u32 k = 0; k < 3; k++) {
                    u32 ix = tri_idx[t * 3 + k];
                    TreeVert *q = &v[nv];
                    ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, ufbx_get_vertex_vec3(&m->vertex_position, ix));
                    ufbx_vec3 n = m->vertex_normal.exists ? ufbx_transform_direction(&nm, ufbx_get_vertex_vec3(&m->vertex_normal, ix)) : (ufbx_vec3){ 0, 1, 0 };
                    double nl = sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    if (nl < 1e-12) n = (ufbx_vec3){ 0, 1, 0 }, nl = 1;
                    q->p[0] = (float)p.x, q->p[1] = (float)p.y, q->p[2] = (float)p.z;
                    q->n[0] = (float)(n.x / nl), q->n[1] = (float)(n.y / nl), q->n[2] = (float)(n.z / nl);
                    if (m->vertex_uv.exists) {
                        ufbx_vec2 uv = ufbx_get_vertex_vec2(&m->vertex_uv, ix);
                        q->uv[0] = (float)uv.x;
                        q->uv[1] = 1.0f - (float)uv.y;   // files: v up; textures: row 0 on top
                    }
                    ufbx_vec4 cl = m->vertex_color.exists ? ufbx_get_vertex_vec4(&m->vertex_color, ix) : (ufbx_vec4){ 0, 0, 0, 1 };
                    col[nv * 4] = (float)cl.x, col[nv * 4 + 1] = (float)cl.y, col[nv * 4 + 2] = (float)cl.z, col[nv * 4 + 3] = (float)cl.w;
                    posid[nv] = vbase + m->vertex_indices.data[ix];
                    nv++;
                }
                for (int k = 1; k < 3; k++) {   // the triangle's file vertices are one piece
                    u32 a = uf_find(uf, posid[nv - 3]), b = uf_find(uf, posid[nv - 3 + (size_t)k]);
                    if (a != b) uf[a] = b;
                }
                tmat[nv / 3 - 1] = (u16)mi;
            }
        }
        vbase += (u32)m->num_vertices;
    }
    ufbx_free_scene(s);
    if (nv < 3) {
        snprintf(err, (size_t)err_size, "%s: no triangles", path);
        free(v), free(col), free(posid), free(uf), free(tmat);
        return false;
    }
    // pieces, numbered densely
    u32 *piece = (u32 *)malloc(sizeof(u32) * nv), *remap = (u32 *)malloc(sizeof(u32) * (total_pos ? total_pos : 1));
    for (size_t i = 0; i < total_pos; i++) remap[i] = ~0u;
    u32 pieces = 0;
    for (size_t i = 0; i < nv; i++) {
        u32 r = uf_find(uf, posid[i]);
        if (remap[r] == ~0u) remap[r] = pieces++;
        piece[i] = remap[r];
    }
    free(remap), free(uf), free(posid);
    // textures: "<material>.png"; a material whose texture cuts out (> 2 % of the texels below half
    // alpha) is a masked, two-sided card
    for (u32 k = 0; k < meta->material_count; k++) {
        char tp[600];
        snprintf(tp, sizeof(tp), "%s%s.png", dir, meta->tex[k]);
        dg_strcpy(meta->tex[k], sizeof(meta->tex[0]), tp + strlen(dir));
        meta->masked[k] = false;
        if (!meta->texture[k].rgba && !vgeo_tree_texture_load(tp, VG_TREE_TEX_MAX, &meta->texture[k])) {
            log_warn("vgeo_tree: %s: texture %s missing", sp->name, tp);
            continue;
        }
        const VgTreeTexture *tx = &meta->texture[k];
        size_t cut = 0, n = (size_t)tx->w * tx->h;
        for (size_t i = 0; i < n; i++) cut += tx->rgba[i * 4 + 3] < 128;
        meta->masked[k] = cut > n / 50;
    }
    // Tree It card normals point up and outwards (soft crown lighting), whatever the winding: wind
    // every card triangle towards its normal so the front face is the side it points to (the resolve
    // flips the normal of back faces only)
    for (size_t t = 0; t + 2 < nv; t += 3) {
        if (!meta->masked[tmat[t / 3]]) continue;
        const float *a = v[t].p, *b = v[t + 1].p, *cc = v[t + 2].p;
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] };
        float g[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float d = 0;
        for (int q = 0; q < 3; q++) d += g[q] * (v[t].n[q] + v[t + 1].n[q] + v[t + 2].n[q]);
        if (d < 0) {
            TreeVert x = v[t + 1];
            v[t + 1] = v[t + 2];
            v[t + 2] = x;
            float xc[4];
            memcpy(xc, col + (t + 1) * 4, 16);
            memcpy(col + (t + 1) * 4, col + (t + 2) * 4, 16);
            memcpy(col + (t + 2) * 4, xc, 16);
        }
    }
    // metres at the species height, ground (file y = 0) kept, the trunk base centred
    float ymax = -1e30f;
    for (size_t i = 0; i < nv; i++) ymax = DG_MAX(ymax, v[i].p[1]);
    const float height = vgeo_tree_height(sp);
    meta->src_height = ymax;
    meta->scale = ymax > 0 ? height / ymax : 1.0f;
    for (size_t i = 0; i < nv; i++)
        for (int a = 0; a < 3; a++) v[i].p[a] *= meta->scale;
    double cx = 0, cz = 0, cn = 0;
    for (int pass = 0; pass < 2 && cn == 0; pass++)
        for (size_t i = 0; i < nv; i++) {
            if (v[i].p[1] < 0 || v[i].p[1] > 1.5f || (!pass && meta->masked[tmat[i / 3]])) continue;
            cx += v[i].p[0], cz += v[i].p[2], cn += 1;
        }
    if (cn > 0)
        for (size_t i = 0; i < nv; i++) v[i].p[0] -= (float)(cx / cn), v[i].p[2] -= (float)(cz / cn);
    // trunk capsule: the 25th percentile of the bark's distance from the axis between 0.4 and 1.6 m (low
    // branches and root flares lie further out), 10 % over
    float *rs = (float *)malloc(sizeof(float) * nv);
    size_t nr = 0;
    for (size_t i = 0; i < nv; i++) {
        float r = sqrtf(v[i].p[0] * v[i].p[0] + v[i].p[2] * v[i].p[2]);
        if (!meta->masked[tmat[i / 3]] && v[i].p[1] >= 0.4f * DG_MIN(1.0f, height / 4.0f) && v[i].p[1] <= 1.6f * DG_MIN(1.0f, height / 4.0f)) rs[nr++] = r;
    }
    if (nr) {
        qsort(rs, nr, sizeof(float), cmp_float);
        meta->cap_r = DG_CLAMP(rs[nr / 4] * 1.1f, 0.05f, 1.0f);
    } else meta->cap_r = 0.1f;
    meta->cap_h = DG_MIN(0.5f * height, 5.0f);
    free(rs);
    c->v = v, c->col = col, c->piece = piece, c->tmat = tmat, c->n = nv, c->pieces = pieces;
    return true;
}

// ------------------------------------------------------------------ wind channels (docs/FOLIAGE_WIND.md)

typedef struct Acc { double n, sx, sy, sxx, syy, sxy; } Acc;
static void acc_add(Acc *a, double x, double y) { a->n += 1, a->sx += x, a->sy += y, a->sxx += x * x, a->syy += y * y, a->sxy += x * y; }
static double acc_corr(const Acc *a) {
    if (a->n < 2) return 0;
    double cx = a->sxx - a->sx * a->sx / a->n, cy = a->syy - a->sy * a->sy / a->n, cxy = a->sxy - a->sx * a->sy / a->n;
    return cx > 1e-12 && cy > 1e-12 ? cxy / sqrt(cx * cy) : 0;
}

typedef struct ChStats {
    double within, total;        // pooled within-piece / total sums of squares
    int distinct;
    float bark_max, max;
    float card_zero;             // fraction of card pieces whose min is ~0
    double uv_corr, radial_corr;
    bool varies;
} ChStats;

static void channel_stats(const TreeCorners *c, const VgTree *meta, ChStats st[4], bool *have_bark, bool *have_leaf) {
    memset(st, 0, sizeof(ChStats) * 4);
    double *pm = (double *)calloc(c->pieces, sizeof(double) * 4), *pn = (double *)calloc(c->pieces, sizeof(double));
    float *pmin = (float *)malloc(sizeof(float) * 4 * c->pieces);
    u8 *pleaf = (u8 *)calloc(c->pieces, 1);
    for (u32 i = 0; i < c->pieces * 4; i++) pmin[i] = 1e9f;
    double mean[4] = { 0 };
    *have_bark = *have_leaf = false;
    for (size_t i = 0; i < c->n; i++) {
        u32 p = c->piece[i];
        pn[p] += 1;
        pleaf[p] = meta->masked[c->tmat[i / 3]];
        if (pleaf[p]) *have_leaf = true;
        else *have_bark = true;
        for (int k = 0; k < 4; k++) {
            float x = c->col[i * 4 + (size_t)k];
            pm[p * 4 + (u32)k] += x;
            mean[k] += x;
            if (x < pmin[p * 4 + (u32)k]) pmin[p * 4 + (u32)k] = x;
        }
    }
    for (int k = 0; k < 4; k++) mean[k] /= (double)c->n;
    Acc uv[4], rad[4];
    memset(uv, 0, sizeof(uv));
    memset(rad, 0, sizeof(rad));
    u8 seen[4][256];
    memset(seen, 0, sizeof(seen));
    for (size_t i = 0; i < c->n; i++) {
        u32 p = c->piece[i];
        bool leaf = meta->masked[c->tmat[i / 3]];
        const float *q = c->v[i].p;
        double rr = sqrt((double)q[0] * q[0] + (double)q[2] * q[2]);
        for (int k = 0; k < 4; k++) {
            double x = c->col[i * 4 + (size_t)k], pmean = pm[p * 4 + (u32)k] / pn[p];
            st[k].within += (x - pmean) * (x - pmean);
            st[k].total += (x - mean[k]) * (x - mean[k]);
            if (x > st[k].max) st[k].max = (float)x;
            if (!leaf && x > st[k].bark_max) st[k].bark_max = (float)x;
            int b = (int)lround(x * 255.0);
            seen[k][b < 0 ? 0 : b > 255 ? 255 : b] = 1;
            if (leaf) acc_add(&uv[k], x, c->v[i].uv[1]);
            else acc_add(&rad[k], x, rr);
        }
    }
    u32 nleaf = 0, zero[4] = { 0 };
    for (u32 p = 0; p < c->pieces; p++) {
        if (!pleaf[p] || !pn[p]) continue;
        nleaf++;
        for (int k = 0; k < 4; k++) zero[k] += pmin[p * 4 + (u32)k] <= 2.5f / 255.0f;
    }
    for (int k = 0; k < 4; k++) {
        for (int b = 0; b < 256; b++) st[k].distinct += seen[k][b];
        st[k].card_zero = nleaf ? (float)zero[k] / (float)nleaf : 0;
        st[k].uv_corr = acc_corr(&uv[k]);
        st[k].radial_corr = acc_corr(&rad[k]);
        st[k].varies = st[k].total > 1e-6 * (double)c->n && st[k].distinct >= 2;
    }
    free(pm), free(pn), free(pmin), free(pleaf);
}

static bool detect(const TreeCorners *c, const VgTree *meta, VgTreeChannelReport *rep) {
    memset(rep, 0, sizeof(*rep));
    rep->flutter = rep->phase = rep->branch = -1;
    rep->has_color = c->has_color;
    if (!c->has_color) {
        dg_strcpy(rep->note, sizeof(rep->note), "no vertex colours");
        return false;
    }
    ChStats st[4];
    bool have_bark, have_leaf;
    channel_stats(c, meta, st, &have_bark, &have_leaf);
    // phase: the most per-piece-constant varying channel
    double best = 1e9;
    for (int k = 0; k < 4; k++) {
        if (!st[k].varies || st[k].distinct < 3) continue;
        double f = st[k].within / st[k].total;
        if (f < best) best = f, rep->phase = k;
    }
    // flutter: ~0 on the bark, ~0 at the stem end of most cards. When two channels pass (a plant whose
    // only bark is the trunk, where the branch weight is 0 too: bamboo) the data cannot tell them
    // apart: the Tree It order (R) wins and the report says so.
    double bf = -1, score[4] = { -1, -1, -1, -1 };
    for (int k = 0; k < 4; k++) {
        if (k == rep->phase || !st[k].varies) continue;
        if (have_bark && st[k].bark_max > 3.0f / 255.0f) continue;
        score[k] = st[k].card_zero + 0.25 * fabs(st[k].uv_corr);
        if (score[k] > bf) bf = score[k], rep->flutter = k;
    }
    int passing = 0;
    for (int k = 0; k < 4; k++) passing += score[k] >= 0.8 * bf && bf > 0;
    bool ambiguous = passing > 1;
    if (ambiguous && score[0] >= 0.8 * bf && rep->phase != 0) rep->flutter = 0;
    // branch: the remaining colour channel (prefer RGB, the one that varies most)
    double bv = -1;
    for (int k = 0; k < 4; k++) {
        if (k == rep->phase || k == rep->flutter) continue;
        double x = st[k].total + (k < 3 ? 1e-3 : 0);
        if (x > bv) bv = x, rep->branch = k;
    }
    if (rep->flutter >= 0) {
        rep->flutter_bark_max = st[rep->flutter].bark_max;
        rep->flutter_uv_corr = -(float)st[rep->flutter].uv_corr;   // the file's v (stored flipped)
        rep->flutter_card_zero = st[rep->flutter].card_zero;
    }
    if (rep->phase >= 0) rep->phase_const = (float)(1.0 - st[rep->phase].within / st[rep->phase].total);
    if (rep->branch >= 0) {
        rep->branch_radial_corr = (float)st[rep->branch].radial_corr;
        rep->branch_max = st[rep->branch].max;
    }
    snprintf(rep->note, sizeof(rep->note), "%s%s%s", have_bark ? "bark" : "no bark", have_leaf ? " + cards" : ", no cards",
             ambiguous ? "; flutter ambiguous from the data (two channels ~0 on bark and card stems): Tree It order" : "");
    return rep->flutter >= 0 && rep->phase >= 0 && rep->branch >= 0;
}

bool vgeo_tree_detect_channels(const VgTreeSpecies *sp, const char *dir, VgTreeChannelReport *r, char *err, int err_size) {
    TreeCorners c;
    VgTree meta;
    memset(&meta, 0, sizeof(meta));
    meta.sp = sp;
    bool ok = tree_load(sp, dir, &c, &meta, err, err_size);
    if (ok) detect(&c, &meta, r);
    corners_free(&c);
    vgeo_tree_free(&meta);
    return ok;
}

static float frac_hash(u32 h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return (float)(h >> 8) / 16777216.0f;
}

static u32 unorm8(float x) { return (u32)lroundf((x < 0 ? 0 : x > 1 ? 1 : x) * 255.0f); }

// The canonical wind weights of every corner (TreeVert.col, foliage_wind.h): Tree It's vertex colours
// (R flutter, G branch phase, B branch bend: the layout tests/test_wind.c detects on every export), the
// branch channel normalised to 1 (Tree It's maximum differs per tree: pine 0.10, beech 1.0). Without
// colours they are inferred: flutter along each card from its trunk-side end (0 .. 0.8), a phase per
// ~1.5 m of crown (a branch and its cards), branch = the distance from the trunk axis / the crown radius.
static void tree_wind_weights(TreeCorners *c, VgTree *meta, bool infer) {
    size_t n = c->n;
    float rmax = 1e-3f;
    for (size_t i = 0; i < n; i++) rmax = DG_MAX(rmax, sqrtf(c->v[i].p[0] * c->v[i].p[0] + c->v[i].p[2] * c->v[i].p[2]));
    meta->wind_inferred = infer || !c->has_color;
    float *att = NULL, *ext = NULL, *ph = NULL;
    if (meta->wind_inferred) {
        att = (float *)malloc(sizeof(float) * 4 * c->pieces);   // xyz, r of the attachment corner
        ext = (float *)calloc(c->pieces, sizeof(float));
        ph = (float *)malloc(sizeof(float) * c->pieces);
        double *cen = (double *)calloc(c->pieces, sizeof(double) * 4);
        for (u32 p = 0; p < c->pieces; p++) att[p * 4 + 3] = 1e30f;
        for (size_t i = 0; i < n; i++) {
            u32 p = c->piece[i];
            const float *q = c->v[i].p;
            float rr = sqrtf(q[0] * q[0] + q[2] * q[2]);
            if (rr < att[p * 4 + 3]) memcpy(&att[p * 4], q, 12), att[p * 4 + 3] = rr;
            for (int k = 0; k < 3; k++) cen[p * 4 + (u32)k] += q[k];
            cen[p * 4 + 3] += 1;
        }
        for (size_t i = 0; i < n; i++) {
            u32 p = c->piece[i];
            const float *q = c->v[i].p;
            float dx = q[0] - att[p * 4], dy = q[1] - att[p * 4 + 1], dz = q[2] - att[p * 4 + 2];
            ext[p] = DG_MAX(ext[p], sqrtf(dx * dx + dy * dy + dz * dz));
        }
        for (u32 p = 0; p < c->pieces; p++) {
            i32 g[3];
            for (int k = 0; k < 3; k++) g[k] = (i32)floor(cen[p * 4 + (u32)k] / fmax(cen[p * 4 + 3], 1.0) / 1.5);
            ph[p] = frac_hash((u32)g[0] * 73856093u ^ (u32)g[1] * 19349663u ^ (u32)g[2] * 83492791u);
        }
        free(cen);
    }
    float *w = (float *)malloc(sizeof(float) * 3 * n), bmax = 0;
    for (size_t i = 0; i < n; i++) {
        const float *q = c->v[i].p;
        if (!meta->wind_inferred) {
            w[i * 3] = c->col[i * 4], w[i * 3 + 1] = c->col[i * 4 + 1], w[i * 3 + 2] = c->col[i * 4 + 2];
        } else {
            u32 p = c->piece[i];
            float dx = q[0] - att[p * 4], dy = q[1] - att[p * 4 + 1], dz = q[2] - att[p * 4 + 2];
            w[i * 3] = meta->masked[c->tmat[i / 3]] && ext[p] > 1e-4f ? 0.8f * sqrtf(dx * dx + dy * dy + dz * dz) / ext[p] : 0.0f;
            w[i * 3 + 1] = ph[p];
            w[i * 3 + 2] = sqrtf(q[0] * q[0] + q[2] * q[2]) / rmax;
        }
        bmax = DG_MAX(bmax, w[i * 3 + 2]);
    }
    meta->branch_scale = bmax > 1e-3f ? 1.0f / bmax : 1.0f;
    for (size_t i = 0; i < n; i++)
        c->v[i].col = unorm8(w[i * 3]) | unorm8(w[i * 3 + 1]) << 8 | unorm8(w[i * 3 + 2] * meta->branch_scale) << 16 | 0xFF000000u;
    free(w), free(att), free(ext), free(ph);
}

bool vgeo_tree_import(const VgTreeSpecies *sp, const char *dir, VgFoliageMesh *out, VgTree *meta, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
    TreeCorners c;
    if (!tree_load(sp, dir, &c, meta, err, err_size)) return false;
    tree_wind_weights(&c, meta, sp->infer_wind);
    // deduplicate the corners (position, normal, uv and wind weights)
    size_t nv = c.n;
    ufbx_vertex_stream st = { c.v, nv, sizeof(TreeVert) };
    u32 *idx = (u32 *)malloc(sizeof(u32) * nv);
    ufbx_error ge;
    size_t nu = ufbx_generate_indices(&st, 1, idx, nv, NULL, &ge);
    out->vertex_count = (u32)nu;
    out->index_count = (u32)nv;
    out->material_count = meta->material_count;
    out->pos = (float *)malloc(sizeof(float) * 3 * nu);
    out->normal = (float *)malloc(sizeof(float) * 3 * nu);
    out->uv = (float *)malloc(sizeof(float) * 2 * nu);
    out->color = (u32 *)malloc(sizeof(u32) * nu);
    out->indices = idx;
    out->tri_material = c.tmat;
    c.tmat = NULL;
    for (size_t i = 0; i < nu; i++) {
        memcpy(out->pos + i * 3, c.v[i].p, 12);
        memcpy(out->normal + i * 3, c.v[i].n, 12);
        memcpy(out->uv + i * 2, c.v[i].uv, 8);
        out->color[i] = c.v[i].col;
    }
    corners_free(&c);
    out->vcap = out->vertex_count;
    out->icap = out->index_count;
    return true;
}

// ------------------------------------------------------------------ cache

typedef struct VgTreeCacheHeader {
    u32 magic, version, key, material_count;
    float cap_r, cap_h, src_height, scale;
    char tex[VG_TREE_MAX_MATERIALS][48];
    u8 masked[VG_TREE_MAX_MATERIALS];
    u32 vgeo_size, pad[3];
} VgTreeCacheHeader;
#define VG_TREE_MAGIC 0x52544744u   // "DGTR"

u32 vgeo_tree_key(const VgTreeSpecies *sp, const char *dir) {
    char path[600];
    snprintf(path, sizeof(path), "%s%s", dir, sp->file);
    size_t size = 0;
    char *f = platform_read_file(path, &size);
    if (!f) return 0;
    u32 h = vgeo_fnv1a(f, size, 2166136261u);
    free(f);
    u32 v[4] = { VG_TREE_VERSION, VGEO_BUILD_VERSION, sp->atlas, VG_TREE_TEX_MAX };
    h = vgeo_fnv1a(v, sizeof(v), h);
    float hr[2] = { sp->height_min, sp->height_max };
    h = vgeo_fnv1a(hr, sizeof(hr), h);
    return h ? h : 1;
}

bool vgeo_tree_write(const VgTree *t, u32 key, const char *path) {
    u8 *data = NULL;
    size_t size = 0;
    if (!vgeo_serialize(&t->b, key, &data, &size)) return false;
    VgTreeCacheHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = VG_TREE_MAGIC;
    h.version = VG_TREE_VERSION;
    h.key = key;
    h.material_count = t->material_count;
    h.cap_r = t->cap_r, h.cap_h = t->cap_h, h.src_height = t->src_height, h.scale = t->scale;
    memcpy(h.tex, t->tex, sizeof(h.tex));
    for (u32 k = 0; k < VG_TREE_MAX_MATERIALS; k++) h.masked[k] = t->masked[k];
    h.vgeo_size = (u32)size;
    u8 *file = (u8 *)malloc(sizeof(h) + size);
    memcpy(file, &h, sizeof(h));
    memcpy(file + sizeof(h), data, size);
    bool ok = platform_write_file(path, file, sizeof(h) + size);
    free(file);
    free(data);
    return ok;
}

static bool tree_read(const char *path, u32 key, VgTree *t) {
    size_t size = 0;
    u8 *d = (u8 *)platform_read_file(path, &size);
    if (!d) return false;
    VgTreeCacheHeader h;
    bool ok = size >= sizeof(h);
    if (ok) memcpy(&h, d, sizeof(h));
    ok = ok && h.magic == VG_TREE_MAGIC && h.version == VG_TREE_VERSION && h.key == key && h.material_count <= VG_TREE_MAX_MATERIALS && sizeof(h) + h.vgeo_size <= size;
    ok = ok && vgeo_deserialize(d + sizeof(h), h.vgeo_size, &t->b);
    free(d);
    if (!ok) return false;
    t->material_count = h.material_count;
    t->cap_r = h.cap_r, t->cap_h = h.cap_h, t->src_height = h.src_height, t->scale = h.scale;
    memcpy(t->tex, h.tex, sizeof(h.tex));
    for (u32 k = 0; k < VG_TREE_MAX_MATERIALS; k++) {
        t->tex[k][47] = 0;
        t->masked[k] = h.masked[k] != 0;
    }
    return true;
}

static void impostor_materials(const VgTree *t, VgImpostorMaterial im[VG_TREE_MAX_MATERIALS]) {
    memset(im, 0, sizeof(VgImpostorMaterial) * VG_TREE_MAX_MATERIALS);
    for (u32 k = 0; k < t->material_count; k++) {
        im[k].color[0] = im[k].color[1] = im[k].color[2] = im[k].color[3] = 1.0f;
        im[k].rgba = t->texture[k].rgba;
        im[k].w = t->texture[k].w;
        im[k].h = t->texture[k].h;
        im[k].tex_scale = 1.0f;
        im[k].masked = t->masked[k];
        im[k].cutoff = 0.5f;
    }
}

bool vgeo_tree_load_or_build(const VgTreeSpecies *sp, const char *dir, const char *built_dir, const char *cache_dir, VgTree *out, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
    out->sp = sp;
    u32 key = vgeo_tree_key(sp, dir);
    if (!key) { snprintf(err, (size_t)err_size, "%s%s: cannot read", dir, sp->file); return false; }
    char bpath[700] = "", cpath[700] = "";
    if (built_dir && built_dir[0]) snprintf(bpath, sizeof(bpath), "%s%s.dgtree", built_dir, sp->name);
    if (cache_dir && cache_dir[0]) snprintf(cpath, sizeof(cpath), "%s%s.tree.%08x.dgc", cache_dir, sp->name, key);
    if ((bpath[0] && tree_read(bpath, key, out)) || (cpath[0] && tree_read(cpath, key, out))) {
        out->from_cache = true;
        for (u32 k = 0; k < out->material_count; k++) {
            char tp[700];
            snprintf(tp, sizeof(tp), "%s%s", dir, out->tex[k]);
            if (!vgeo_tree_texture_load(tp, VG_TREE_TEX_MAX, &out->texture[k])) log_warn("vgeo_tree: %s: texture %s missing", sp->name, tp);
        }
        return true;
    }
    VgFoliageMesh fm;
    if (!vgeo_tree_import(sp, dir, &fm, out, err, err_size)) { vgeo_tree_free(out); return false; }
    VgSource src;
    memset(&src, 0, sizeof(src));
    src.vertex_count = fm.vertex_count;
    src.pos = fm.pos;
    src.normal = fm.normal;
    src.uv = fm.uv;
    src.color = fm.color;
    src.index_count = fm.index_count;
    src.indices = fm.indices;
    src.tri_material = fm.tri_material;
    src.material_count = fm.material_count;
    src.material_names = (const char(*)[48])out->tex;
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.threads = 1;   // the callers build species in parallel
    bool ok = vgeo_build(&src, &opt, &out->b, err, err_size);
    vgeo_foliage_mesh_free(&fm);
    if (!ok) { vgeo_tree_free(out); return false; }
    if (sp->atlas) {
        VgImpostorMaterial im[VG_TREE_MAX_MATERIALS];
        impostor_materials(out, im);
        VgImpostorOptions io = { sp->atlas, 8, im, out->material_count };
        vgeo_impostor_bake(&out->b, &io);
    }
    out->b.hdr.key = key;
    if (cpath[0]) {
        SDL_CreateDirectory(cache_dir);
        if (!vgeo_tree_write(out, key, cpath)) log_warn("vgeo_tree: cannot write cache %s", cpath);
    }
    return true;
}

void vgeo_tree_materials(const VgTree *t, VgMaterial out[VG_TREE_MAX_MATERIALS]) {
    const VgTreeSpecies *sp = t->sp;
    memset(out, 0, sizeof(VgMaterial) * VG_TREE_MAX_MATERIALS);
    VgWindParams w;
    vgeo_tree_wind(sp, &w);
    for (u32 k = 0; k < VG_TREE_MAX_MATERIALS; k++) {
        VgMaterial *m = &out[k];
        m->color[0] = m->color[1] = m->color[2] = m->color[3] = 1.0f;   // the textures carry the colour
        m->tex_scale = 1.0f;
        m->roughness = t->masked[k] ? 0.7f : 0.92f;
        m->flags = VGM_FOLIAGE | VGM_DITHER_FADE | MAT_TEXTURED_FLAG;
        if (t->masked[k]) {
            m->flags |= VGM_MASKED | VGM_TWO_SIDED;
            m->alpha_cutoff = 0.5f;
        }
        // flowers are walked through (the displacement buffer first); trees and bushes have trunk capsules
        m->wpo = sp->role == VG_TREE_FLOWER ? (WPO_GRASS | WPO_DISPLACE) : WPO_TREE;
        vgeo_wind_pack(&w, m->wpo_params, m->wind_params);
        m->fade_start = sp->fade_start;
        m->fade_end = sp->fade_end;
        m->shadow_max_dist = sp->shadow_max;
    }
}

void vgeo_tree_free(VgTree *t) {
    vgeo_free(&t->b);
    for (u32 k = 0; k < VG_TREE_MAX_MATERIALS; k++) free(t->texture[k].rgba);
    const VgTreeSpecies *sp = t->sp;
    memset(t, 0, sizeof(*t));
    t->sp = sp;
}
