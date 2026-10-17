// Skinning against the source file (docs/CHARACTERS.md "Proportions", docs/VGEO.md "Skin per position"):
// every character mesh is re-read straight from its FBX with ufbx and skinned on the CPU with the file's
// own weights (all influences, float) and its own inverse bind matrices, on the shared Skeleton's
// animated pose (Unreal's USkeletalMesh semantics: a mesh follows the skeleton's bones relative to its
// own bind). That reference is compared per vertex against what the two GPU paths are fed:
//   legacy: the baked SkinVertex (pos, u8 joints / weights) x skm_gather_palette (skinned.vert);
//   vgeo:   the same vertices built by vgeo_build (weld, skin per position, u8 renormalisation), decoded
//           from the pages and skinned like vg_decode.glsl.
// Poses: every frame-ish of a set of strong-bend clips (crouch, attacks, death, jog). Run from the build
// output dir (or the repository root) so assets/ resolves; meshes whose FBX is missing are skipped.
#include "anim/anim_library.h"
#include "anim/skeleton.h"
#include "assets/fbx_loader.h"
#include "render/skeletal_mesh.h"
#include "render/vgeo/vgeo_build.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "ufbx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;
static AnimLibrary *L;
static bool g_verbose;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// ------------------------------------------------------------------ file space (as fbx_loader.c)

typedef struct Conv { float unit; quat rot; } Conv;
static Conv make_conv(const ufbx_scene *s) {
    Conv c;
    c.unit = s->settings.unit_meters > 0 ? (float)s->settings.unit_meters : 0.01f;
    c.rot = quat_yaw(DG_PI);
    if (s->settings.axes.up == UFBX_COORDINATE_AXIS_POSITIVE_Z) c.rot = quat_mul(c.rot, quat_axis_angle(v3(1, 0, 0), -DG_PI * 0.5f));
    return c;
}
static Xform conv_matrix(const Conv *c, const ufbx_matrix *m) {
    ufbx_transform t = ufbx_matrix_to_transform(m);
    Xform x;
    x.t = quat_rotate(c->rot, v3_scale(v3((float)t.translation.x, (float)t.translation.y, (float)t.translation.z), c->unit));
    quat q = { (float)t.rotation.x, (float)t.rotation.y, (float)t.rotation.z, (float)t.rotation.w };
    x.r = quat_norm(quat_mul(c->rot, q));
    x.s = v3(1, 1, 1);
    return x;
}
static vec3 conv_point(const Conv *c, ufbx_vec3 p) { return quat_rotate(c->rot, v3_scale(v3((float)p.x, (float)p.y, (float)p.z), c->unit)); }
static int lod_from_name(const char *name) {
    const char *p = strstr(name, "_LOD");
    if (!p) p = strstr(name, "_lod");
    if (p && p[4] >= '0' && p[4] <= '9') return DG_CLAMP(p[4] - '0', 0, SKIN_MAX_LODS - 1);
    return 0;
}
static bool node_is_bone(const ufbx_node *n) { return n->bone != NULL || n->attrib_type == UFBX_ELEMENT_BONE; }

// One LOD0 face corner of the file: its position in file bind space, every influence (mesh bone,
// weight) and the file node it came from.
#define REF_MAX_INF 8
typedef struct RefCorner {
    vec3 pos;
    int n;
    int joint[REF_MAX_INF];
    float w[REF_MAX_INF];
    int node;
} RefCorner;

typedef struct RefMesh {
    RefCorner *c;
    u32 count;
    Xform file_bind[SKM_MAX_BONES];   // per mesh bone: the file's model-space bind (cluster bind_to_world)
    bool has_bind[SKM_MAX_BONES];
    int nodes;
} RefMesh;

static bool ref_load(const char *path, const SkeletalMesh *m, RefMesh *out) {
    memset(out, 0, sizeof(*out));
    ufbx_load_opts o;
    memset(&o, 0, sizeof(o));
    o.generate_missing_normals = true;
    ufbx_error e;
    ufbx_scene *sc = ufbx_load_file(path, &o, &e);
    if (!sc) return false;
    Conv cv = make_conv(sc);
    u32 cap = 0;
    for (size_t ni = 0; ni < sc->nodes.count; ni++) {
        const ufbx_node *node = sc->nodes.data[ni];
        if (node->mesh && !node_is_bone(node) && lod_from_name(node->name.data) == 0) cap += (u32)node->mesh->num_triangles * 3;
    }
    out->c = (RefCorner *)calloc(cap ? cap : 1, sizeof(RefCorner));
    u32 tri[64 * 3];
    for (size_t ni = 0; ni < sc->nodes.count; ni++) {
        const ufbx_node *node = sc->nodes.data[ni];
        const ufbx_mesh *mesh = node->mesh;
        if (!mesh || node_is_bone(node) || lod_from_name(node->name.data) != 0) continue;
        const ufbx_skin_deformer *skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : NULL;
        int cj[512];
        int ncl = skin ? (int)DG_MIN(skin->clusters.count, 512) : 0;
        for (int c = 0; c < ncl; c++) {
            const ufbx_skin_cluster *cl = skin->clusters.data[c];
            cj[c] = cl->bone_node ? skm_find_bone(m, skm_alias_resolve(&m->aliases, cl->bone_node->name.data)) : -1;
            if (cj[c] >= 0 && !out->has_bind[cj[c]]) {
                out->file_bind[cj[c]] = conv_matrix(&cv, &cl->bind_to_world);
                out->has_bind[cj[c]] = true;
            }
        }
        int node_idx = out->nodes++;
        for (size_t f = 0; f < mesh->faces.count; f++) {
            u32 nt = ufbx_triangulate_face(tri, ARRAY_COUNT(tri), mesh, mesh->faces.data[f]);
            for (u32 t = 0; t < nt * 3; t++) {
                u32 ix = tri[t];
                RefCorner *rc = &out->c[out->count++];
                rc->node = node_idx;
                rc->pos = conv_point(&cv, ufbx_transform_position(&node->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, ix)));
                u32 vid = mesh->vertex_indices.data[ix];
                if (!skin || vid >= skin->vertices.count) continue;
                ufbx_skin_vertex sv = skin->vertices.data[vid];
                for (u32 k = 0; k < sv.num_weights; k++) {
                    ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                    if ((int)sw.cluster_index >= ncl || cj[sw.cluster_index] < 0 || sw.weight <= 0) continue;
                    int j = cj[sw.cluster_index], at = -1;
                    for (int q = 0; q < rc->n; q++) if (rc->joint[q] == j) at = q;
                    if (at < 0 && rc->n < REF_MAX_INF) { at = rc->n++; rc->joint[at] = j; rc->w[at] = 0; }
                    if (at >= 0) rc->w[at] += (float)sw.weight;
                }
                float s = 0;
                for (int q = 0; q < rc->n; q++) s += rc->w[q];
                for (int q = 0; q < rc->n; q++) rc->w[q] /= s > 0 ? s : 1;
            }
        }
    }
    ufbx_free_scene(sc);
    return true;
}

// Reference: sum_c w_c * world_c * inverse(file bind_c) * p (the file's skin, the skeleton's pose).
static vec3 ref_skin(const RefCorner *rc, const RefMesh *r, const Xform *bone_world) {
    if (!rc->n) return xf_point(bone_world[0], rc->pos);
    vec3 p = v3(0, 0, 0);
    for (int q = 0; q < rc->n; q++) {
        int j = rc->joint[q];
        Xform pal = xf_mul(bone_world[j], xf_inverse(r->file_bind[j]));
        p = v3_madd(p, xf_point(pal, rc->pos), rc->w[q]);
    }
    return p;
}

static vec3 skin_u8(const mat4 *pal, const u8 *joints, const u8 *weights, vec3 pos) {
    vec3 p = v3(0, 0, 0);
    for (int k = 0; k < 4; k++)
        if (weights[k]) p = v3_madd(p, mat4_mul_point(pal[joints[k]], pos), (float)weights[k] / 255.0f);
    return p;
}

// ------------------------------------------------------------------ vgeo skin lookup (grid -> skin)

typedef struct GridSkin { i32 g[3]; u8 j[4], w[4]; bool used; } GridSkin;
typedef struct GridMap { GridSkin *e; u32 cap; } GridMap;
static u32 grid_hash(const i32 g[3]) { return vgeo_fnv1a(g, 12, 2166136261u); }
static GridSkin *grid_find(GridMap *m, const i32 g[3], bool insert) {
    u32 h = grid_hash(g) & (m->cap - 1);
    while (m->e[h].used) {
        if (!memcmp(m->e[h].g, g, 12)) return &m->e[h];
        h = (h + 1) & (m->cap - 1);
    }
    if (!insert) return NULL;
    m->e[h].used = true;
    memcpy(m->e[h].g, g, 12);
    return &m->e[h];
}

// ------------------------------------------------------------------ poses

typedef struct Stats { double sum, max; u64 n; u64 over; } Stats;
static void st_add(Stats *s, double e, double thr) {
    s->sum += e;
    s->n++;
    if (e > s->max) s->max = e;
    if (e > thr) s->over++;
}

static const char *k_clips[] = { "crouch", "Zombie Attack", "Zombie Punching", "death", "jog", "running", "Hanging Idle", "jump" };

static int collect_poses(Xform (**out)[ANIM_MAX_BONES]) {
    int cap = 256, n = 0;
    *out = (Xform(*)[ANIM_MAX_BONES])calloc((size_t)cap, sizeof(Xform) * ANIM_MAX_BONES);
    for (int c = 0; c < L->clip_count && n < cap; c++) {
        bool want = false;
        for (int k = 0; k < (int)ARRAY_COUNT(k_clips); k++) {
            const char *a = L->clips[c].name, *b = k_clips[k];
            for (; *a && !want; a++) {
                size_t i = 0;
                while (b[i] && a[i] && (a[i] | 32) == (b[i] | 32)) i++;
                if (!b[i]) want = true;
            }
        }
        if (!want || L->clips[c].additive) continue;
        for (int s = 0; s < 6 && n < cap; s++) {
            Pose p;
            clip_sample_phase(&L->clips[c], (s + 0.5f) / 6.0f, &p);
            pose_to_model(&L->skel, &p, (*out)[n]);
            n++;
        }
    }
    return n;
}

// ------------------------------------------------------------------ the comparison

typedef struct MeshReport {
    Stats legacy, legacy_q, vgeo_vs_legacy, vgeo;
    u32 corners, vgeo_missing, cross_node_cells, skin_mismatch_cells, bad_sum;
    double far_max;   // legacy skin of the same pose 300 m from the origin vs the reference
    u32 welded, clusters;
} MeshReport;

#define ERR_THR 0.005   // 5 mm: visible at character scale

static void compare_mesh(const char *path, const SkeletalMesh *m, Xform (*poses)[ANIM_MAX_BONES], int npose, MeshReport *rep) {
    memset(rep, 0, sizeof(*rep));
    char buf[512];
    RefMesh *r = (RefMesh *)calloc(1, sizeof(RefMesh));
    if (!ref_load(platform_data_path(path, buf, sizeof(buf)), m, r)) { printf("  %s: FBX unavailable (skipped)\n", path); free(r); return; }
    const SkinLod *lod = &m->data.lods[0];
    CHECK(r->count == lod->icount, "%s: %u file corners vs %u baked LOD0 indices", path, r->count, lod->icount);
    if (r->count != lod->icount) { free(r->c); free(r); return; }
    rep->corners = r->count;
    for (u32 i = 0; i < lod->vcount; i++) {
        int sum = 0;
        for (int k = 0; k < 4; k++) sum += lod->verts[i].weights[k];
        rep->bad_sum += sum != 255;
    }
    // Every mesh bone without a cluster (mesh-only parents, rigid joints): the baked bind is the file's.
    for (int j = 0; j < m->bone_count; j++)
        if (!r->has_bind[j]) r->file_bind[j] = m->bones[j].bind_model;

    // vgeo build of the same vertices the renderer builds (vk_vgeo.c skin_worker).
    u32 vn = lod->vcount;
    float *pos = (float *)malloc(sizeof(float) * 3 * vn), *nrm = (float *)malloc(sizeof(float) * 3 * vn), *uv = (float *)malloc(sizeof(float) * 2 * vn);
    u8 *jo = (u8 *)malloc(4u * vn), *we = (u8 *)malloc(4u * vn);
    u32 *col = (u32 *)malloc(sizeof(u32) * vn);
    for (u32 i = 0; i < vn; i++) {
        const SkinVertex *v = &lod->verts[i];
        memcpy(&pos[i * 3], &v->pos, 12);
        memcpy(&nrm[i * 3], &v->normal, 12);
        memcpy(&uv[i * 2], &v->uv, 8);
        memcpy(&jo[i * 4], v->joints, 4);
        memcpy(&we[i * 4], v->weights, 4);
        col[i] = v->color;
    }
    VgSource src;
    memset(&src, 0, sizeof(src));
    src.vertex_count = vn;
    src.pos = pos;
    src.normal = nrm;
    src.uv = uv;
    src.color = col;
    src.index_count = lod->icount;
    src.indices = lod->indices;
    src.joints = jo;
    src.weights_u8 = we;
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    VgBuilt *vb = (VgBuilt *)calloc(1, sizeof(VgBuilt));
    char err[128];
    bool vok = vgeo_build(&src, &opt, vb, err, sizeof(err));
    CHECK(vok, "%s: vgeo build failed: %s", path, err);
    GridMap gm = { 0, 0 };
    if (vok) {
        rep->welded = vb->stats.welded_verts;
        rep->clusters = vb->hdr.cluster_count;
        gm.cap = 1;
        while (gm.cap < vn * 4) gm.cap <<= 1;
        gm.e = (GridSkin *)calloc(gm.cap, sizeof(GridSkin));
        VgDecoded *d = (VgDecoded *)malloc(sizeof(VgDecoded));
        for (u32 p = 0; p < vb->hdr.page_count; p++) {
            const VgPageHeader *ph = vgeo_page(vb, p);
            for (u32 c = 0; c < ph->cluster_count; c++) {
                vgeo_decode_cluster(vb, p, c, d);
                for (u32 v = 0; v < d->vert_count; v++) {
                    GridSkin *g = grid_find(&gm, d->grid[v], true);
                    memcpy(g->j, d->joints[v], 4);
                    memcpy(g->w, d->weights[v], 4);
                }
            }
        }
        free(d);
        // Cells shared by corners of different file meshes / with different source skins.
        int *cell_node = (int *)malloc(sizeof(int) * gm.cap);
        u8 *cell_flags = (u8 *)calloc(gm.cap, 1);
        for (u32 i = 0; i < gm.cap; i++) cell_node[i] = -1;
        for (u32 k = 0; k < r->count; k++) {
            const SkinVertex *v = &lod->verts[lod->indices[k]];
            i32 g[3];
            for (int a = 0; a < 3; a++) g[a] = (i32)llrint((double)(&v->pos.x)[a] / (double)vb->hdr.q_step);
            GridSkin *gs = grid_find(&gm, g, false);
            if (!gs) continue;
            u32 ci = (u32)(gs - gm.e);
            if (cell_node[ci] < 0) cell_node[ci] = r->c[k].node;
            else if (cell_node[ci] != r->c[k].node && !(cell_flags[ci] & 1)) { cell_flags[ci] |= 1; rep->cross_node_cells++; }
            bool same = true;
            for (int q = 0; q < 4; q++) {
                // the cell's skin vs this vertex's own (normalised the same way: sorted, merged)
                int wj = 0;
                for (int s = 0; s < 4; s++) if (v->joints[s] == gs->j[q]) wj += v->weights[s];
                if (gs->w[q] && abs(wj - gs->w[q]) > 2) same = false;
            }
            if (!same && !(cell_flags[ci] & 2)) { cell_flags[ci] |= 2; rep->skin_mismatch_cells++; }
        }
        free(cell_node);
        free(cell_flags);
    }

    SkeletonInstance *si = (SkeletonInstance *)calloc(1, sizeof(SkeletonInstance));
    skel_inst_init(si, &L->skeleton);
    static mat4 pal[SKM_MAX_BONES];
    static Xform bw[SKM_MAX_BONES];
    for (int pi = 0; pi < npose; pi++) {
        skel_inst_set_pose(si, poses[pi]);
        skm_gather_palette(m, si, NULL, pal);
        for (int j = 0; j < m->bone_count; j++) bw[j] = skm_bone_world(m, si, j);
        for (u32 k = 0; k < r->count; k++) {
            const RefCorner *rc = &r->c[k];
            const SkinVertex *v = &lod->verts[lod->indices[k]];
            vec3 ref = ref_skin(rc, r, bw);
            vec3 leg = skin_u8(pal, v->joints, v->weights, v->pos);
            st_add(&rep->legacy, v3_dist(leg, ref), ERR_THR);
            // Same u8 influences, the file's per-bone inverse bind: only the baked retarget differs.
            vec3 q = v3(0, 0, 0);
            for (int s = 0; s < 4; s++)
                if (v->weights[s]) q = v3_madd(q, xf_point(xf_mul(bw[v->joints[s]], xf_inverse(r->file_bind[v->joints[s]])), rc->pos), v->weights[s] / 255.0f);
            if (!rc->n) q = ref;
            st_add(&rep->legacy_q, v3_dist(leg, q), ERR_THR);
            if (gm.e) {
                i32 g[3];
                for (int a = 0; a < 3; a++) g[a] = (i32)llrint((double)(&v->pos.x)[a] / (double)vb->hdr.q_step);
                GridSkin *gs = grid_find(&gm, g, false);
                if (!gs) { if (pi == 0) rep->vgeo_missing++; continue; }
                vec3 gp = v3((float)(g[0] * (double)vb->hdr.q_step), (float)(g[1] * (double)vb->hdr.q_step), (float)(g[2] * (double)vb->hdr.q_step));
                vec3 vg = skin_u8(pal, gs->j, gs->w, gp);
                st_add(&rep->vgeo_vs_legacy, v3_dist(vg, leg), ERR_THR);
                st_add(&rep->vgeo, v3_dist(vg, ref), ERR_THR);
            }
        }
    }
    // One pose far from the origin: weights not summing to 255 scale the vertex about the world origin.
    {
        Xform far[ANIM_MAX_BONES];
        Xform off = xf_make(v3(300, 0, 300), quat_identity());
        for (int b = 0; b < L->skel.count; b++) far[b] = xf_mul(off, poses[npose / 2][b]);
        skel_inst_set_pose(si, far);
        skm_gather_palette(m, si, NULL, pal);
        for (int j = 0; j < m->bone_count; j++) bw[j] = skm_bone_world(m, si, j);
        for (u32 k = 0; k < r->count; k++) {
            const SkinVertex *v = &lod->verts[lod->indices[k]];
            double e = v3_dist(skin_u8(pal, v->joints, v->weights, v->pos), ref_skin(&r->c[k], r, bw));
            if (e > rep->far_max) rep->far_max = e;
        }
    }
    free(si);
    free(gm.e);
    if (vok) vgeo_free(vb);
    free(vb);
    free(pos); free(nrm); free(uv); free(jo); free(we); free(col);
    free(r->c);
    free(r);
}

static void print_stats(const char *what, const Stats *s) {
    printf("    %-16s max %7.2f mm  mean %6.3f mm  >5mm %6.3f%%\n", what, s->max * 1000.0, s->n ? s->sum / (double)s->n * 1000.0 : 0.0,
           s->n ? 100.0 * (double)s->over / (double)s->n : 0.0);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) g_verbose = true;
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    L = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    if (!anim_lib_load(L, 30.0f)) { printf("FAIL: library load\n"); return 1; }
    printf("skinning vs the source FBX\n");
    Xform (*poses)[ANIM_MAX_BONES] = NULL;
    int npose = collect_poses(&poses);
    printf("  %d poses\n", npose);
    CHECK(npose >= 12, "too few strong-bend poses (%d)", npose);
    int tested = 0;
    for (int i = 0; i < L->mesh_count; i++) {
        const MeshRef *ref = &L->mesh_refs[i];
        if (ref->region[0] || ref->inflate != 0 || ref->scale.x != 1 || !strstr(ref->path, ".fbx")) continue;
        MeshReport rep;
        compare_mesh(ref->path, &L->meshes[i], poses, npose, &rep);
        if (!rep.corners) continue;
        tested++;
        printf("  %s: %u corners, %d bones (%d mesh-only), max bind delta %.1f mm / %.1f deg; vgeo %u welded, %u clusters\n", ref->path, rep.corners,
               L->meshes[i].bone_count, skm_mesh_only_count(&L->meshes[i]), L->meshes[i].max_bind_delta_m * 1000.0f, L->meshes[i].max_bind_delta_deg, rep.welded,
               rep.clusters);
        int fixes = 0;
        for (int j = 0; j < L->meshes[i].bone_count; j++) fixes += L->meshes[i].bones[j].has_fix;
        printf("    baked-retarget error bound %.2f mm -> %s (%d bones with a palette fix)\n", L->meshes[i].retarget_blend_err_m * 1000.0f,
               fixes ? "per-bone file binds" : "retarget baked into the vertices", fixes);
        printf("    vgeo cells: %u shared by different file meshes, %u whose skin differs from a vertex in it, %u corners not found\n", rep.cross_node_cells,
               rep.skin_mismatch_cells, rep.vgeo_missing);
        printf("    %u baked vertices whose weights do not sum to 255; legacy max error 300 m from the origin %.2f mm\n", rep.bad_sum, rep.far_max * 1000.0);
        if (g_verbose)
            for (int j = 0; j < L->meshes[i].bone_count; j++) {
                const SkmBone *bn = &L->meshes[i].bones[j];
                float ang = quat_angle_between(bn->bind_delta.r, quat_identity()) * DG_RAD2DEG;
                if (ang > 5.0f) printf("      bind delta %-32s %6.1f deg\n", bn->name, ang);
            }
        print_stats("legacy vs ref", &rep.legacy);
        print_stats("legacy vs ref(u8)", &rep.legacy_q);
        print_stats("vgeo vs legacy", &rep.vgeo_vs_legacy);
        print_stats("vgeo vs ref", &rep.vgeo);
        // Tolerances. The retarget (same u8 influences, the file's own inverse binds) is exact: before the
        // per-bone fix it was off by 43-84 mm (mean 1.5-4.3 mm) on the Mixamo skins. What remains against
        // the full-weight reference is 4-influence truncation + u8 weights (a few vertices with a 5th
        // influence, up to ~17 mm; mean < 0.15 mm). vgeo: the weld gives coincident vertices the first
        // one's skin (crack-free cuts), a few u8 steps on seam duplicates (< 2 mm).
        CHECK(rep.legacy_q.max < 0.0005, "%s: baked retarget deviates from the per-bone file binds (max %.1f mm)", ref->path, rep.legacy_q.max * 1000.0);
        CHECK(rep.legacy.max < 0.025 && rep.legacy.sum / (double)rep.legacy.n < 0.0003, "%s: legacy skin deviates from the file (max %.1f mm, mean %.3f mm)",
              ref->path, rep.legacy.max * 1000.0, rep.legacy.sum / (double)rep.legacy.n * 1000.0);
        CHECK(rep.vgeo_vs_legacy.max < 0.0025, "%s: vgeo skin deviates from the legacy skin (max %.2f mm)", ref->path, rep.vgeo_vs_legacy.max * 1000.0);
        CHECK(rep.bad_sum == 0, "%s: %u vertices with weights not summing to 255", ref->path, rep.bad_sum);
        CHECK(rep.far_max < 0.025, "%s: legacy skin 300 m from the origin deviates by %.1f mm", ref->path, rep.far_max * 1000.0);
        CHECK(rep.vgeo_missing == 0, "%s: %u corners missing from the vgeo pages", ref->path, rep.vgeo_missing);
    }
    CHECK(tested >= 1, "no character FBX available");
    free(poses);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
