#include "foliage_import.h"
#include "ufbx.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ raw corners

// Every triangle corner of the file (world space, metres), with its material, connected piece and colour.
typedef struct Raw {
    u32 n, cap;
    float *pos, *nrm, *uv, *col;     // 3, 3, 2, 4 per corner
    u32 *piece;                      // connected piece (per material: pieces never span materials)
    u16 *mat;
    u32 pieces;
    u32 nmat;
    char (*names)[48];
    char (*tex)[64];
    u8 *kind;
    u32 *mat_pieces, *mat_verts;     // per material: pieces, distinct positions (shape classification)
    bool has_color, is_obj;
} Raw;

static void raw_free(Raw *r) {
    free(r->pos), free(r->nrm), free(r->uv), free(r->col), free(r->piece), free(r->mat);
    free(r->names), free(r->tex), free(r->kind), free(r->mat_pieces), free(r->mat_verts);
    memset(r, 0, sizeof(*r));
}

static void raw_grow(Raw *r, u32 need) {
    if (need <= r->cap) return;
    while (r->cap < need) r->cap = r->cap ? r->cap * 2 : 16384;
    r->pos = realloc(r->pos, sizeof(float) * 3 * r->cap);
    r->nrm = realloc(r->nrm, sizeof(float) * 3 * r->cap);
    r->uv = realloc(r->uv, sizeof(float) * 2 * r->cap);
    r->col = realloc(r->col, sizeof(float) * 4 * r->cap);
    r->piece = realloc(r->piece, sizeof(u32) * r->cap);
    r->mat = realloc(r->mat, sizeof(u16) * r->cap);
}

static u32 uf_find(u32 *p, u32 x) {
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}

static bool ends_with_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (b > a) return false;
    for (size_t i = 0; i < b; i++) if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}
static bool contains_ci(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)sub[i])) i++;
        if (i == n) return true;
    }
    return false;
}

// "beech leaf_1" -> "beech leaf" (Tree It suffixes mesh names with the material index).
static void strip_index(char *s) {
    size_t n = strlen(s);
    size_t i = n;
    while (i > 0 && isdigit((unsigned char)s[i - 1])) i--;
    if (i < n && i > 0 && s[i - 1] == '_') s[i - 1] = 0;
}

static void dir_of(const char *path, char *out, size_t n) {
    dg_strcpy(out, n, path);
    char *a = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > a) a = b;
    if (a) a[1] = 0;
    else out[0] = 0;
}
static const char *base_name(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}
static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static u32 raw_material(Raw *r, const char *name, const char *tex, const char *dir) {
    for (u32 i = 0; i < r->nmat; i++)
        if (!strcmp(r->names[i], name)) return i;
    u32 i = r->nmat++;
    r->names = realloc(r->names, 48 * (size_t)r->nmat);
    r->tex = realloc(r->tex, 64 * (size_t)r->nmat);
    dg_strcpy(r->names[i], 48, name);
    r->tex[i][0] = 0;
    if (tex && tex[0]) dg_strcpy(r->tex[i], 64, base_name(tex));
    else {
        char p[600];
        snprintf(p, sizeof(p), "%s%s.png", dir, name);   // Tree It FBX: no materials, textures named like the mesh
        if (file_exists(p)) snprintf(r->tex[i], 64, "%s.png", name);
    }
    return i;
}

static bool raw_load(const char *path, float unit, Raw *r, char *err, int err_size) {
    memset(r, 0, sizeof(*r));
    r->is_obj = ends_with_ci(path, ".obj");
    ufbx_load_opts o;
    memset(&o, 0, sizeof(o));
    o.target_axes = ufbx_axes_right_handed_y_up;
    o.target_unit_meters = 1.0f;
    o.generate_missing_normals = true;
    ufbx_error e;
    ufbx_scene *s = ufbx_load_file(path, &o, &e);
    if (!s) { snprintf(err, (size_t)err_size, "%s: %s", path, e.description.data); return false; }
    float extra = r->is_obj ? (unit > 0 ? unit : 1.0f) : (unit > 0 && s->settings.unit_meters > 0 ? unit / (float)s->settings.unit_meters : 1.0f);
    char dir[512];
    dir_of(path, dir, sizeof(dir));
    u32 tri[64 * 3];
    u32 vbase = 0, total_pos = 0;
    for (size_t ni = 0; ni < s->nodes.count; ni++) if (s->nodes.data[ni]->mesh) total_pos += (u32)s->nodes.data[ni]->mesh->num_vertices;
    u32 *uf = (u32 *)malloc(sizeof(u32) * (total_pos ? total_pos : 1));
    for (u32 i = 0; i < total_pos; i++) uf[i] = i;
    u32 *posid = NULL;
    u32 posid_cap = 0;
    for (size_t ni = 0; ni < s->nodes.count; ni++) {
        ufbx_node *node = s->nodes.data[ni];
        ufbx_mesh *m = node->mesh;
        if (!m) continue;
        ufbx_matrix nm = ufbx_matrix_for_normals(&node->geometry_to_world);
        if (m->vertex_color.exists) r->has_color = true;
        for (size_t pi = 0; pi < m->material_parts.count; pi++) {
            const ufbx_mesh_part *part = &m->material_parts.data[pi];
            char name[48] = "";
            const char *tex = NULL;
            ufbx_material *mt = pi < m->materials.count ? m->materials.data[pi] : NULL;
            if (mt) {
                dg_strcpy(name, sizeof(name), mt->name.data);
                if (mt->fbx.diffuse_color.texture) tex = mt->fbx.diffuse_color.texture->relative_filename.length ? mt->fbx.diffuse_color.texture->relative_filename.data : mt->fbx.diffuse_color.texture->filename.data;
                else if (mt->pbr.base_color.texture) tex = mt->pbr.base_color.texture->filename.data;
            } else {
                dg_strcpy(name, sizeof(name), m->name.length ? m->name.data : node->name.data);
                strip_index(name);
            }
            u32 mat = raw_material(r, name, tex, dir);
            for (size_t fi = 0; fi < part->face_indices.count; fi++) {
                ufbx_face f = m->faces.data[part->face_indices.data[fi]];
                if (f.num_indices < 3 || f.num_indices > 64) continue;
                u32 nt = ufbx_triangulate_face(tri, 64 * 3, m, f);
                raw_grow(r, r->n + nt * 3);
                if (posid_cap < r->n + nt * 3) { posid_cap = (r->n + nt * 3) * 2; posid = realloc(posid, sizeof(u32) * posid_cap); }
                for (u32 t = 0; t < nt; t++) {
                    for (int k = 0; k < 3; k++) {
                        u32 ix = tri[t * 3 + (u32)k], c = r->n + (u32)k;
                        ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, ufbx_get_vertex_vec3(&m->vertex_position, ix));
                        ufbx_vec3 n = m->vertex_normal.exists ? ufbx_transform_direction(&nm, ufbx_get_vertex_vec3(&m->vertex_normal, ix)) : (ufbx_vec3){ 0, 1, 0 };
                        float nl = (float)sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                        if (nl < 1e-12f) nl = 1;
                        r->pos[c * 3] = (float)p.x * extra, r->pos[c * 3 + 1] = (float)p.y * extra, r->pos[c * 3 + 2] = (float)p.z * extra;
                        r->nrm[c * 3] = (float)n.x / nl, r->nrm[c * 3 + 1] = (float)n.y / nl, r->nrm[c * 3 + 2] = (float)n.z / nl;
                        ufbx_vec2 uv = m->vertex_uv.exists ? ufbx_get_vertex_vec2(&m->vertex_uv, ix) : (ufbx_vec2){ 0, 0 };
                        r->uv[c * 2] = (float)uv.x, r->uv[c * 2 + 1] = 1.0f - (float)uv.y;   // glTF / Vulkan convention (v down)
                        ufbx_vec4 col = m->vertex_color.exists ? ufbx_get_vertex_vec4(&m->vertex_color, ix) : (ufbx_vec4){ 0, 0, 0, 1 };
                        r->col[c * 4] = (float)col.x, r->col[c * 4 + 1] = (float)col.y, r->col[c * 4 + 2] = (float)col.z, r->col[c * 4 + 3] = (float)col.w;
                        r->mat[c] = (u16)mat;
                        posid[c] = vbase + m->vertex_indices.data[ix];
                    }
                    for (int k = 1; k < 3; k++) {
                        u32 a = uf_find(uf, posid[r->n]), b = uf_find(uf, posid[r->n + (u32)k]);
                        if (a != b) uf[a] = b;
                    }
                    r->n += 3;
                }
            }
        }
        vbase += (u32)m->num_vertices;
    }
    ufbx_free_scene(s);
    if (!r->n) { snprintf(err, (size_t)err_size, "%s: no triangles", path); free(uf); free(posid); raw_free(r); return false; }
    // pieces: (material, connected component), numbered densely
    u32 *remap = (u32 *)malloc(sizeof(u32) * total_pos);
    for (u32 i = 0; i < total_pos; i++) remap[i] = ~0u;
    r->mat_pieces = calloc(r->nmat, sizeof(u32));
    r->mat_verts = calloc(r->nmat, sizeof(u32));
    u8 *seen = calloc(total_pos, 1);
    for (u32 c = 0; c < r->n; c++) {
        u32 root = uf_find(uf, posid[c]);
        if (remap[root] == ~0u) { remap[root] = r->pieces++; r->mat_pieces[r->mat[c]]++; }
        r->piece[c] = remap[root];
        if (!seen[posid[c]]) { seen[posid[c]] = 1; r->mat_verts[r->mat[c]]++; }
    }
    free(seen);
    free(remap);
    free(uf);
    free(posid);
    return true;
}

// Bark (rigid tubes) or leaf cards: by name, else by shape (tubes have many vertices per piece).
static void raw_classify(Raw *r, const VgFoliageImportOptions *o) {
    r->kind = malloc(r->nmat ? r->nmat : 1);
    static const char *bark[] = { "bark", "stump", "trunk", "stem", "log", "wood" };
    static const char *leaf[] = { "leaf", "leaves", "branch", "needle", "flower", "frond", "petal", "grass", "blade", "card", "twig" };
    for (u32 m = 0; m < r->nmat; m++) {
        int k = -1;
        for (int i = 0; o && i < o->kind_count && k < 0; i++)
            if (contains_ci(r->names[m], o->kinds[i].pattern)) k = o->kinds[i].kind;
        for (int i = 0; i < 6 && k < 0; i++) if (contains_ci(r->names[m], bark[i])) k = VGFM_BARK;
        for (int i = 0; i < 11 && k < 0; i++) if (contains_ci(r->names[m], leaf[i])) k = VGFM_LEAF;
        if (k < 0) k = r->mat_verts[m] >= 24u * (r->mat_pieces[m] ? r->mat_pieces[m] : 1) ? VGFM_BARK : VGFM_LEAF;
        r->kind[m] = (u8)k;
    }
}

// ------------------------------------------------------------------ detection

typedef struct Acc { double n, sx, sy, sxx, syy, sxy; } Acc;
static void acc_add(Acc *a, double x, double y) { a->n += 1, a->sx += x, a->sy += y, a->sxx += x * x, a->syy += y * y, a->sxy += x * y; }
static double acc_corr(const Acc *a) {
    if (a->n < 2) return 0;
    double cx = a->sxx - a->sx * a->sx / a->n, cy = a->syy - a->sy * a->sy / a->n, cxy = a->sxy - a->sx * a->sy / a->n;
    return cx > 1e-12 && cy > 1e-12 ? cxy / sqrt(cx * cy) : 0;
}

typedef struct ChStats {
    double within, total;            // pooled within-piece / total sums of squares
    int distinct;
    float bark_max, max;
    float card_zero;                 // fraction of leaf pieces whose min is ~0
    double uv_corr, radial_corr;
    bool varies;
} ChStats;

static void channel_stats(const Raw *r, ChStats st[4], bool *have_bark, bool *have_leaf) {
    memset(st, 0, sizeof(ChStats) * 4);
    double *pm = calloc(r->pieces, sizeof(double) * 4), *pn = calloc(r->pieces, sizeof(double));
    float *pmin = malloc(sizeof(float) * 4 * r->pieces);
    u8 *pleaf = calloc(r->pieces, 1);
    for (u32 i = 0; i < r->pieces * 4; i++) pmin[i] = 1e9f;
    double mean[4] = { 0 };
    *have_bark = *have_leaf = false;
    for (u32 c = 0; c < r->n; c++) {
        u32 p = r->piece[c];
        pn[p] += 1;
        pleaf[p] = r->kind[r->mat[c]] == VGFM_LEAF;
        if (pleaf[p]) *have_leaf = true;
        else *have_bark = true;
        for (int k = 0; k < 4; k++) {
            float v = r->col[c * 4 + k];
            pm[p * 4 + (u32)k] += v;
            mean[k] += v;
            if (v < pmin[p * 4 + (u32)k]) pmin[p * 4 + (u32)k] = v;
        }
    }
    for (int k = 0; k < 4; k++) mean[k] /= r->n;
    Acc uv[4], rad[4];
    memset(uv, 0, sizeof(uv));
    memset(rad, 0, sizeof(rad));
    u8 seen[4][256];
    memset(seen, 0, sizeof(seen));
    for (u32 c = 0; c < r->n; c++) {
        u32 p = r->piece[c];
        bool leaf = r->kind[r->mat[c]] == VGFM_LEAF;
        double rr = sqrt((double)r->pos[c * 3] * r->pos[c * 3] + (double)r->pos[c * 3 + 2] * r->pos[c * 3 + 2]);
        for (int k = 0; k < 4; k++) {
            double v = r->col[c * 4 + k], pmean = pm[p * 4 + (u32)k] / pn[p];
            st[k].within += (v - pmean) * (v - pmean);
            st[k].total += (v - mean[k]) * (v - mean[k]);
            if (v > st[k].max) st[k].max = (float)v;
            if (!leaf && v > st[k].bark_max) st[k].bark_max = (float)v;
            int q = (int)lround(v * 255.0);
            seen[k][q < 0 ? 0 : q > 255 ? 255 : q] = 1;
            if (leaf) acc_add(&uv[k], v, r->uv[c * 2 + 1]);
            else acc_add(&rad[k], v, rr);
        }
    }
    u32 nleaf = 0;
    u32 zero[4] = { 0 };
    for (u32 p = 0; p < r->pieces; p++) {
        if (!pleaf[p] || !pn[p]) continue;
        nleaf++;
        for (int k = 0; k < 4; k++) zero[k] += pmin[p * 4 + (u32)k] <= 2.5f / 255.0f;
    }
    for (int k = 0; k < 4; k++) {
        for (int q = 0; q < 256; q++) st[k].distinct += seen[k][q];
        st[k].card_zero = nleaf ? (float)zero[k] / (float)nleaf : 0;
        st[k].uv_corr = acc_corr(&uv[k]);
        st[k].radial_corr = acc_corr(&rad[k]);
        st[k].varies = st[k].total > 1e-6 * r->n && st[k].distinct >= 2;
    }
    free(pm), free(pn), free(pmin), free(pleaf);
}

// The mapping of a loaded file (raw_classify done). Returns false without colours / when undecided.
static bool detect(const Raw *r, VgFoliageChannelReport *rep) {
    memset(rep, 0, sizeof(*rep));
    rep->flutter = rep->phase = rep->branch = -1;
    rep->has_color = r->has_color;
    if (!r->has_color) { dg_strcpy(rep->note, sizeof(rep->note), "no vertex colours"); return false; }
    ChStats st[4];
    bool have_bark, have_leaf;
    channel_stats(r, st, &have_bark, &have_leaf);
    // phase: the most per-piece-constant varying channel
    double best = 1e9;
    for (int k = 0; k < 4; k++) {
        if (!st[k].varies || st[k].distinct < 3) continue;
        double f = st[k].within / st[k].total;
        if (f < best) { best = f; rep->phase = k; }
    }
    // flutter: ~0 on the bark, ~0 at the stem end of most cards. When two channels pass (a plant whose
    // only bark is the trunk, where the branch weight is 0 too: bamboo) the data cannot tell them
    // apart: the Tree It order (R) wins and the report says so.
    double bf = -1, score[4] = { -1, -1, -1, -1 };
    for (int k = 0; k < 4; k++) {
        if (k == rep->phase || !st[k].varies) continue;
        if (have_bark && st[k].bark_max > 3.0f / 255.0f) continue;
        score[k] = st[k].card_zero + 0.25 * fabs(st[k].uv_corr);
        if (score[k] > bf) { bf = score[k]; rep->flutter = k; }
    }
    int passing = 0;
    for (int k = 0; k < 4; k++) passing += score[k] >= 0.8 * bf && bf > 0;
    bool ambiguous = passing > 1;
    if (ambiguous && score[0] >= 0.8 * bf && rep->phase != 0) rep->flutter = 0;
    // branch: the remaining colour channel (prefer RGB, the one that varies most)
    double bv = -1;
    for (int k = 0; k < 4; k++) {
        if (k == rep->phase || k == rep->flutter) continue;
        double v = st[k].total + (k < 3 ? 1e-3 : 0);
        if (v > bv) { bv = v; rep->branch = k; }
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

bool vgeo_foliage_detect_channels(const char *path, VgFoliageChannelReport *rep, char *err, int err_size) {
    Raw r;
    if (!raw_load(path, 0, &r, err, err_size)) return false;
    raw_classify(&r, NULL);
    detect(&r, rep);
    raw_free(&r);
    return true;
}

int vgeo_foliage_channel_parse(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "r") || !strcmp(s, "R") || !strcmp(s, "x")) return 0;
    if (!strcmp(s, "g") || !strcmp(s, "G") || !strcmp(s, "y")) return 1;
    if (!strcmp(s, "b") || !strcmp(s, "B") || !strcmp(s, "z")) return 2;
    if (!strcmp(s, "a") || !strcmp(s, "A") || !strcmp(s, "w")) return 3;
    if (!strcmp(s, "none")) return VGFC_NONE;
    return -1;
}

// ------------------------------------------------------------------ import

void vgeo_foliage_import_defaults(VgFoliageImportOptions *o) {
    memset(o, 0, sizeof(*o));
    o->channels[VGFC_FLUTTER] = 0;
    o->channels[VGFC_PHASE] = 1;
    o->channels[VGFC_BRANCH] = 2;
    o->normalize_branch = true;
}

static float frac_hash(u32 h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return (float)(h >> 8) / 16777216.0f;
}

static u8 unorm8(float v) { return (u8)lroundf((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f); }

bool vgeo_foliage_import(const char *path, const VgFoliageImportOptions *o, VgFoliageAsset *out, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
    Raw r;
    if (!raw_load(path, o->unit, &r, err, err_size)) return false;
    raw_classify(&r, o);
    int ch[VGFC_COUNT];
    memcpy(ch, o->channels, sizeof(ch));
    if (o->detect && r.has_color) {
        VgFoliageChannelReport rep;
        if (detect(&r, &rep)) ch[VGFC_FLUTTER] = rep.flutter, ch[VGFC_PHASE] = rep.phase, ch[VGFC_BRANCH] = rep.branch;
    }
    if (!r.has_color || o->ignore_colors)
        for (int k = 0; k < VGFC_COUNT; k++) ch[k] = VGFC_NONE;
    for (int k = 0; k < VGFC_COUNT; k++) {
        if (ch[k] < 0 || ch[k] > VGFC_NONE) ch[k] = VGFC_NONE;
        if (ch[k] == VGFC_NONE) out->inferred = true;
    }
    memcpy(out->channels, ch, sizeof(ch));
    // geometry: height above the pivot, radius from the trunk axis
    float rmax = 1e-3f, hmax = 0;
    for (u32 c = 0; c < r.n; c++) {
        float rr = sqrtf(r.pos[c * 3] * r.pos[c * 3] + r.pos[c * 3 + 2] * r.pos[c * 3 + 2]);
        rmax = fmaxf(rmax, rr);
        hmax = fmaxf(hmax, r.pos[c * 3 + 1]);
    }
    out->height = hmax > 1e-3f ? hmax : 1e-3f;
    out->radius = rmax;
    // inference inputs: per piece the attachment corner (nearest the trunk axis), its extent, a phase
    float *att = NULL, *ext = NULL, *ph = NULL;
    if (out->inferred) {
        att = malloc(sizeof(float) * 4 * r.pieces);   // xyz, r of the attachment corner
        ext = calloc(r.pieces, sizeof(float));
        ph = malloc(sizeof(float) * r.pieces);
        double *cen = calloc(r.pieces, sizeof(double) * 4);
        for (u32 p = 0; p < r.pieces; p++) att[p * 4 + 3] = 1e30f;
        for (u32 c = 0; c < r.n; c++) {
            u32 p = r.piece[c];
            float rr = sqrtf(r.pos[c * 3] * r.pos[c * 3] + r.pos[c * 3 + 2] * r.pos[c * 3 + 2]);
            if (rr < att[p * 4 + 3]) { memcpy(&att[p * 4], &r.pos[c * 3], 12); att[p * 4 + 3] = rr; }
            for (int k = 0; k < 3; k++) cen[p * 4 + (u32)k] += r.pos[c * 3 + (u32)k];
            cen[p * 4 + 3] += 1;
        }
        for (u32 c = 0; c < r.n; c++) {
            u32 p = r.piece[c];
            float dx = r.pos[c * 3] - att[p * 4], dy = r.pos[c * 3 + 1] - att[p * 4 + 1], dz = r.pos[c * 3 + 2] - att[p * 4 + 2];
            ext[p] = fmaxf(ext[p], sqrtf(dx * dx + dy * dy + dz * dz));
        }
        for (u32 p = 0; p < r.pieces; p++) {   // pieces within ~1.5 m share a phase (a branch and its cards)
            i32 q[3];
            for (int k = 0; k < 3; k++) q[k] = (i32)floor(cen[p * 4 + (u32)k] / fmax(cen[p * 4 + 3], 1.0) / 1.5);
            ph[p] = frac_hash((u32)q[0] * 73856093u ^ (u32)q[1] * 19349663u ^ (u32)q[2] * 83492791u);
        }
        free(cen);
    }
    // canonical weights per corner (float), then the branch normalisation
    float *w = malloc(sizeof(float) * 3 * r.n), bmax = 0;
    for (u32 c = 0; c < r.n; c++) {
        bool leaf = r.kind[r.mat[c]] == VGFM_LEAF;
        u32 p = r.piece[c];
        for (int k = 0; k < VGFC_COUNT; k++) {
            float v;
            if (ch[k] != VGFC_NONE) v = r.col[c * 4 + (u32)ch[k]];
            else if (k == VGFC_FLUTTER) {
                float dx = r.pos[c * 3] - att[p * 4], dy = r.pos[c * 3 + 1] - att[p * 4 + 1], dz = r.pos[c * 3 + 2] - att[p * 4 + 2];
                v = leaf && ext[p] > 1e-4f ? 0.8f * sqrtf(dx * dx + dy * dy + dz * dz) / ext[p] : 0.0f;
            } else if (k == VGFC_PHASE) v = ph[p];
            else v = sqrtf(r.pos[c * 3] * r.pos[c * 3] + r.pos[c * 3 + 2] * r.pos[c * 3 + 2]) / rmax;
            w[c * 3 + (u32)k] = v;
        }
        bmax = fmaxf(bmax, w[c * 3 + VGFC_BRANCH]);
    }
    out->branch_scale = o->normalize_branch && bmax > 1e-3f ? 1.0f / bmax : 1.0f;
    // output: one vertex per corner (the builder welds), materials in file order
    VgImported *im = &out->im;
    im->vertex_count = r.n;
    im->index_count = r.n;
    im->pos = r.pos, r.pos = NULL;
    im->normal = r.nrm, r.nrm = NULL;
    im->uv = r.uv, r.uv = NULL;
    im->indices = malloc(sizeof(u32) * r.n);
    im->tri_material = malloc(sizeof(u16) * (r.n / 3 + 1));
    out->color = malloc(sizeof(u32) * r.n);
    for (u32 c = 0; c < r.n; c++) {
        im->indices[c] = c;
        if (c % 3 == 0) im->tri_material[c / 3] = r.mat[c];
        out->color[c] = (u32)unorm8(w[c * 3 + VGFC_FLUTTER]) | (u32)unorm8(w[c * 3 + VGFC_PHASE]) << 8 | (u32)unorm8(w[c * 3 + VGFC_BRANCH] * out->branch_scale) << 16 | 0xFF000000u;
    }
    im->material_count = r.nmat;
    im->material_names = (char (*)[48])r.names, r.names = NULL;
    out->material_kind = r.kind, r.kind = NULL;
    out->textures = r.tex, r.tex = NULL;
    free(w), free(att), free(ext), free(ph);
    raw_free(&r);
    return true;
}

void vgeo_foliage_asset_free(VgFoliageAsset *a) {
    vgeo_imported_free(&a->im);
    free(a->color);
    free(a->material_kind);
    free(a->textures);
    memset(a, 0, sizeof(*a));
}

void vgeo_foliage_asset_source(const VgFoliageAsset *a, VgSource *src) {
    vgeo_imported_source(&a->im, src);
    src->color = a->color;
}
