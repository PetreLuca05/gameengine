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

// The species (assets/treefbx, Tree It exports). Heights are the scale-1 instance heights (the
// scatter varies them by 0.72-1.27); the files are 8-14 m trees in centimetres. Conifers replace the
// conifer role, broadleaves the broadleaf role, the old rooted snag the dead role; juniper, bamboo and a
// small acacia are the underbrush / meadow bushes, daisies the meadow flowers. Trees switch to their
// impostor at 4x r_vgeo_impostor_px (64 px across by default, the atlas frames' own size: their cards
// do not coarsen, a distant tree would stay thousands of triangles), bushes at 2x.
static const VgTreeSpecies k_species[] = {
    //  name           file          role               w     h      sway  branch flutter fade      shadow atlas imp. cap
    { "tree_pine",     "pine.fbx",   VG_TREE_CONIFER,   1.0f, 17.0f, 0.20f, 0.12f, 0.02f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_fir",      "fir.fbx",    VG_TREE_CONIFER,   1.0f, 15.0f, 0.18f, 0.12f, 0.02f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_spruce",   "spruce.fbx", VG_TREE_CONIFER,   1.0f, 18.0f, 0.20f, 0.12f, 0.02f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_oak",      "oak.fbx",    VG_TREE_BROADLEAF, 1.0f, 16.0f, 0.18f, 0.16f, 0.04f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_beech",    "beech.fbx",  VG_TREE_BROADLEAF, 1.0f, 17.0f, 0.22f, 0.16f, 0.04f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_birch",    "birch.fbx",  VG_TREE_BROADLEAF, 0.8f, 14.0f, 0.26f, 0.18f, 0.05f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_elm",      "elm.fbx",    VG_TREE_BROADLEAF, 0.8f, 15.0f, 0.22f, 0.16f, 0.04f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_maple",    "maple.fbx",  VG_TREE_BROADLEAF, 1.0f, 14.0f, 0.20f, 0.16f, 0.04f, 850, 1100, 70, 512, 6.0f, true },
    { "tree_old",      "old.fbx",    VG_TREE_DEAD,      1.0f, 11.0f, 0.08f, 0.05f, 0.02f, 850, 1100, 70, 512, 1.0f, true },
    { "bush_juniper",  "juniper.fbx", VG_TREE_BUSH,     1.0f, 3.2f,  0.05f, 0.05f, 0.02f, 150, 210, 40, 512, 2.0f, true },
    { "bush_bamboo",   "bamboo.obj", VG_TREE_BUSH,      0.4f, 3.6f,  0.08f, 0.08f, 0.03f, 150, 210, 40, 512, 2.0f, true },
    { "bush_acacia",   "tree1.obj",  VG_TREE_BUSH,      0.4f, 4.5f,  0.08f, 0.08f, 0.03f, 150, 210, 40, 512, 2.0f, true },
    { "flower_daisy",  "daisy.fbx",  VG_TREE_FLOWER,    1.0f, 0.7f,  0.04f, 0.05f, 0.02f, 50, 80, 15, 0, 1.0f, false },
};

u32 vgeo_tree_species_count(void) { return (u32)(sizeof(k_species) / sizeof(k_species[0])); }
const VgTreeSpecies *vgeo_tree_species(u32 i) { return i < vgeo_tree_species_count() ? &k_species[i] : NULL; }

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

typedef struct TreeVert { float p[3], n[3], uv[2]; } TreeVert;

// "oak leaf_2" -> "oak leaf" (Tree It numbers its FBX models)
static void strip_suffix(const char *in, char *out, size_t n) {
    dg_strcpy(out, n, in);
    char *u = strrchr(out, '_');
    if (!u || !u[1]) return;
    for (const char *c = u + 1; *c; c++)
        if (*c < '0' || *c > '9') return;
    *u = 0;
}

static float smooth01(float x) {
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    return x * x * (3 - 2 * x);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

bool vgeo_tree_import(const VgTreeSpecies *sp, const char *dir, VgFoliageMesh *out, VgTree *meta, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
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
    // every triangle of every mesh node, one corner per vertex (deduplicated below)
    size_t cap = 0;
    for (size_t i = 0; i < s->nodes.count; i++)
        if (s->nodes.data[i]->mesh) cap += s->nodes.data[i]->mesh->num_triangles * 3;
    TreeVert *v = (TreeVert *)calloc(cap ? cap : 1, sizeof(TreeVert));
    u16 *tmat = (u16 *)malloc(sizeof(u16) * (cap / 3 + 1));
    size_t nv = 0;
    meta->material_count = 0;
    u32 tri_idx[64];
    for (size_t i = 0; i < s->nodes.count; i++) {
        const ufbx_node *node = s->nodes.data[i];
        const ufbx_mesh *m = node->mesh;
        if (!m) continue;
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
                for (u32 c = 0; c < 3; c++) {
                    u32 ix = tri_idx[t * 3 + c];
                    TreeVert *q = &v[nv++];
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
                }
                tmat[nv / 3 - 1] = (u16)mi;
            }
        }
    }
    ufbx_free_scene(s);
    if (nv < 3) {
        snprintf(err, (size_t)err_size, "%s: no triangles", path);
        free(v);
        free(tmat);
        return false;
    }
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
        const float *a = v[t].p, *b = v[t + 1].p, *c = v[t + 2].p;
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float g[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float d = 0;
        for (int q = 0; q < 3; q++) d += g[q] * (v[t].n[q] + v[t + 1].n[q] + v[t + 2].n[q]);
        if (d < 0) {
            TreeVert x = v[t + 1];
            v[t + 1] = v[t + 2];
            v[t + 2] = x;
        }
    }
    // metres at the species height, ground (file y = 0) kept, the trunk base centred
    float ymax = -1e30f;
    for (size_t i = 0; i < nv; i++) ymax = DG_MAX(ymax, v[i].p[1]);
    meta->src_height = ymax;
    meta->scale = ymax > 0 ? sp->height / ymax : 1.0f;
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
    float rmax = 0.5f;
    for (size_t i = 0; i < nv; i++) {
        float r = sqrtf(v[i].p[0] * v[i].p[0] + v[i].p[2] * v[i].p[2]);
        rmax = DG_MAX(rmax, r);
        if (!meta->masked[tmat[i / 3]] && v[i].p[1] >= 0.4f * DG_MIN(1.0f, sp->height / 4.0f) && v[i].p[1] <= 1.6f * DG_MIN(1.0f, sp->height / 4.0f)) rs[nr++] = r;
    }
    if (nr) {
        qsort(rs, nr, sizeof(float), cmp_float);
        meta->cap_r = DG_CLAMP(rs[nr / 4] * 1.1f, 0.05f, 1.0f);
    } else meta->cap_r = 0.1f;
    meta->cap_h = DG_MIN(0.5f * sp->height, 5.0f);
    free(rs);
    // deduplicate the corners
    ufbx_vertex_stream st = { v, nv, sizeof(TreeVert) };
    u32 *idx = (u32 *)malloc(sizeof(u32) * nv);
    ufbx_error ge;
    size_t nu = ufbx_generate_indices(&st, 1, idx, nv, NULL, &ge);
    // wind weights: R trunk sway grows with height, G branch bounce with the distance from the trunk
    // (none near the ground), B flutter on the cards (a vertex shared by bark and a card is bark)
    out->vertex_count = (u32)nu;
    out->index_count = (u32)nv;
    out->material_count = meta->material_count;
    out->pos = (float *)malloc(sizeof(float) * 3 * nu);
    out->normal = (float *)malloc(sizeof(float) * 3 * nu);
    out->uv = (float *)malloc(sizeof(float) * 2 * nu);
    out->color = (u32 *)malloc(sizeof(u32) * nu);
    out->indices = idx;
    out->tri_material = tmat;
    u8 *card = (u8 *)calloc(nu, 1);
    for (size_t i = 0; i < nv; i++) card[idx[i]] |= meta->masked[tmat[i / 3]] ? 1 : 2;
    for (size_t i = 0; i < nu; i++) {
        memcpy(out->pos + i * 3, v[i].p, 12);
        memcpy(out->normal + i * 3, v[i].n, 12);
        memcpy(out->uv + i * 2, v[i].uv, 8);
        float y = v[i].p[1], r = sqrtf(v[i].p[0] * v[i].p[0] + v[i].p[2] * v[i].p[2]);
        float wr = powf(DG_CLAMP(y / sp->height, 0.0f, 1.0f), 1.5f);
        float wg = powf(DG_CLAMP(r / rmax, 0.0f, 1.0f), 1.3f) * smooth01(y / (0.25f * sp->height));
        float wb = card[i] == 1 ? 1.0f : 0.0f;
        out->color[i] = (u32)(wr * 255.0f + 0.5f) | (u32)(wg * 255.0f + 0.5f) << 8 | (u32)(wb * 255.0f + 0.5f) << 16 | 0xFFu << 24;
    }
    free(card);
    free(v);
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
    h = vgeo_fnv1a(&sp->height, sizeof(float), h);
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
        m->wpo = WPO_TREE;
        m->wpo_params[0] = sp->sway;
        m->wpo_params[1] = sp->branch;
        m->wpo_params[2] = sp->flutter;
        m->wpo_params[3] = 14.0f;   // full wind (m/s)
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
