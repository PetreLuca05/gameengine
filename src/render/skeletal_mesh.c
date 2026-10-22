#include "skeletal_mesh.h"
#include "core/platform.h"
#include "anim/anim.h"
#include "cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define SKM_MAGIC 0x4D534744u   // "DGSM"

static const char *base_name(const char *n) {
    const char *c = strrchr(n, ':');
    return c ? c + 1 : n;
}

void skm_free(SkeletalMesh *m) {
    skin_mesh_free(&m->data);
    memset(m, 0, sizeof(*m));
}

void skm_copy(SkeletalMesh *dst, const SkeletalMesh *src) {
    memcpy(dst, src, sizeof(*dst));
    for (int l = 0; l < src->data.lod_count; l++) {
        const SkinLod *a = &src->data.lods[l];
        SkinLod *b = &dst->data.lods[l];
        b->verts = (SkinVertex *)malloc(sizeof(SkinVertex) * DG_MAX(a->vcount, 1u));
        b->vmat = (u8 *)malloc(DG_MAX(a->vcount, 1u));
        b->indices = (u32 *)malloc(sizeof(u32) * DG_MAX(a->icount, 1u));
        memcpy(b->verts, a->verts, sizeof(SkinVertex) * a->vcount);
        memcpy(b->vmat, a->vmat, a->vcount);
        memcpy(b->indices, a->indices, sizeof(u32) * a->icount);
    }
}

// ------------------------------------------------------------------ aliases

int skm_alias_parse(BoneAliasTable *t, const cJSON *obj) {
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, obj) {
        if (!cJSON_IsString(e) || t->count >= SKM_MAX_ALIASES) continue;
        BoneAlias *a = &t->a[t->count++];
        dg_strcpy(a->from, sizeof(a->from), e->string);
        dg_strcpy(a->to, sizeof(a->to), e->valuestring);
        n++;
    }
    return n;
}

const char *skm_alias_resolve(const BoneAliasTable *t, const char *name) {
    if (!t) return name;
    for (int i = 0; i < t->count; i++)
        if (!strcmp(t->a[i].from, name)) return t->a[i].to;
    const char *b = base_name(name);
    for (int i = 0; i < t->count; i++)
        if (!strcmp(base_name(t->a[i].from), b)) return t->a[i].to;
    return name;
}

void skm_alias_apply_skeleton(const BoneAliasTable *t, Skeleton *s) {
    if (!t || !t->count) return;
    for (int i = 0; i < s->count; i++) {
        const char *r = skm_alias_resolve(t, s->names[i]);
        if (r != s->names[i]) {
            char tmp[ANIM_NAME_MAX];
            dg_strcpy(tmp, sizeof(tmp), r);
            dg_strcpy(s->names[i], ANIM_NAME_MAX, tmp);
        }
    }
    skel_finalize(s);
}

u32 skm_alias_hash(const BoneAliasTable *t) {
    u32 h = ANIM_HASH_SEED;
    if (!t) return h;
    for (int i = 0; i < t->count; i++) { h = anim_hash_str(t->a[i].from, h); h = anim_hash_str(t->a[i].to, h); }
    return h;
}

// ------------------------------------------------------------------ bones

int skm_find_bone(const SkeletalMesh *m, const char *name) {
    for (int i = 0; i < m->bone_count; i++) if (!strcmp(m->bones[i].name, name)) return i;
    const char *b = base_name(name);
    for (int i = 0; i < m->bone_count; i++) if (!strcmp(base_name(m->bones[i].name), b)) return i;
    return -1;
}

int skm_bone_of_skel(const SkeletalMesh *m, int skel_bone) {
    for (int i = 0; i < m->bone_count; i++) if (m->bones[i].skel == skel_bone) return i;
    return -1;
}

int skm_add_skel_bone(SkeletalMesh *m, const Skeleton *s, int skel_bone) {
    int i = skm_bone_of_skel(m, skel_bone);
    if (i >= 0 || skel_bone < 0) return i;
    if (m->bone_count >= SKM_MAX_BONES) return -1;
    SkmBone *b = &m->bones[m->bone_count];
    memset(b, 0, sizeof(*b));
    dg_strcpy(b->name, sizeof(b->name), s->names[skel_bone]);
    b->skel = (i16)skel_bone;
    b->parent = -1;
    b->local = xf_identity();
    b->bind_model = s->bind_model[skel_bone];
    b->bind_delta = xf_identity();
    b->bind_fix = mat4_identity();
    b->has_fix = false;
    return m->bone_count++;
}

int skm_skeleton_bone(const SkeletalMesh *m, int j) {
    for (int guard = 0; j >= 0 && j < m->bone_count && guard < SKM_MAX_BONES; guard++) {
        if (m->bones[j].skel >= 0) return m->bones[j].skel;
        j = m->bones[j].parent;
    }
    return 0;
}

void skm_joint_map(const SkeletalMesh *m, i16 *out) {
    for (int j = 0; j < SKM_MAX_BONES; j++) out[j] = (i16)(j < m->bone_count ? skm_skeleton_bone(m, j) : 0);
}

int skm_vertex_bone(const SkeletalMesh *m, const SkinVertex *v) {
    int best = 0;
    for (int k = 1; k < 4; k++) if (v->weights[k] > v->weights[best]) best = k;
    return skm_skeleton_bone(m, v->joints[best]);
}

int skm_triangle_bone(const SkeletalMesh *m, const SkinLod *L, u32 first_index) {
    // The skeleton bone of the most rigidly bound vertex (largest single weight; first vertex on ties).
    int best_w = -1, bone = 0;
    for (int k = 0; k < 3; k++) {
        const SkinVertex *v = &L->verts[L->indices[first_index + k]];
        int j = 0;
        for (int q = 1; q < 4; q++) if (v->weights[q] > v->weights[j]) j = q;
        if (v->weights[j] > best_w) { best_w = v->weights[j]; bone = skm_skeleton_bone(m, v->joints[j]); }
    }
    return bone;
}

int skm_mesh_only_count(const SkeletalMesh *m) {
    int n = 0;
    for (int i = 0; i < m->bone_count; i++) n += m->bones[i].skel < 0;
    return n;
}

void skm_finalize(SkeletalMesh *m, const SkeletonAsset *a) {
    const Skeleton *s = &a->skel;
    // LOD2+: skeleton joints outside the reduced bone set fold onto their kept ancestor (fingers onto
    // the hand, end bones onto their parent). Mesh-only leaves keep riding on their own parents.
    for (int l = 2; l < m->data.lod_count; l++) {
        const BoneSet *keep = &a->lod_bones[DG_MIN(l, SKEL_LOD_COUNT - 1)];
        SkinLod *L = &m->data.lods[l];
        for (u32 i = 0; i < L->vcount; i++)
            for (int k = 0; k < 4; k++) {
                int j = L->verts[i].joints[k];
                if (j >= m->bone_count || m->bones[j].skel < 0 || boneset_has(keep, m->bones[j].skel)) continue;
                int b = m->bones[j].skel;
                while (b >= 0 && !boneset_has(keep, b)) b = s->parent[b];
                int nj = skm_add_skel_bone(m, s, b < 0 ? 0 : b);
                if (nj >= 0) L->verts[i].joints[k] = (u8)nj;
            }
    }
    memset(&m->skinned, 0, sizeof(m->skinned));
    if (m->data.lod_count) {
        const SkinLod *L = &m->data.lods[0];
        for (u32 i = 0; i < L->vcount; i++)
            for (int k = 0; k < 4; k++)
                if (L->verts[i].weights[k] && L->verts[i].joints[k] < m->bone_count && m->bones[L->verts[i].joints[k]].skel >= 0)
                    boneset_add(&m->skinned, m->bones[L->verts[i].joints[k]].skel);
    }
    // Mesh-local sockets: bone names resolve against the mesh's own list (bind space = its bind pose).
    for (int i = 0; i < m->socket_count; i++) {
        SkelSocket *k = &m->sockets[i];
        k->bone = skm_find_bone(m, k->bone_name);
        if (k->bone < 0) { k->rel = xf_identity(); continue; }
        quat rot = quat_euler(k->rotation_deg.y * DG_DEG2RAD, k->rotation_deg.x * DG_DEG2RAD, k->rotation_deg.z * DG_DEG2RAD);
        if (k->bind_space) {
            Xform bind = m->bones[k->bone].bind_model;
            k->rel = xf_mul(xf_inverse(bind), xf_make(v3_add(bind.t, k->offset), rot));
        } else {
            k->rel = xf_make(k->offset, rot);
        }
        k->rel.s = v3(1, 1, 1);
    }
}

void skm_compact_bones(SkeletalMesh *m) {
    bool used[SKM_MAX_BONES] = { 0 };
    for (int l = 0; l < m->data.lod_count; l++) {
        const SkinLod *L = &m->data.lods[l];
        for (u32 i = 0; i < L->vcount; i++)
            for (int k = 0; k < 4; k++)
                if (L->verts[i].weights[k] && L->verts[i].joints[k] < m->bone_count) used[L->verts[i].joints[k]] = true;
    }
    for (int i = 0; i < m->socket_count; i++) if (m->sockets[i].bone >= 0) used[m->sockets[i].bone] = true;
    // Parents of used mesh-only bones (parents always come first: walk backwards).
    for (int j = m->bone_count - 1; j >= 0; j--)
        if (used[j] && m->bones[j].skel < 0 && m->bones[j].parent >= 0) used[m->bones[j].parent] = true;
    int remap[SKM_MAX_BONES];
    int n = 0;
    for (int j = 0; j < m->bone_count; j++) {
        remap[j] = used[j] ? n : -1;
        if (used[j]) {
            m->bones[n] = m->bones[j];
            if (m->bones[n].parent >= 0) m->bones[n].parent = (i16)remap[m->bones[n].parent];
            n++;
        }
    }
    if (n == 0 && m->bone_count) { remap[0] = 0; n = 1; }   // keep one bone (degenerate meshes)
    for (int l = 0; l < m->data.lod_count; l++) {
        SkinLod *L = &m->data.lods[l];
        for (u32 i = 0; i < L->vcount; i++) {
            SkinVertex *v = &L->verts[i];
            int first = -1;
            for (int k = 0; k < 4; k++)
                if (v->weights[k] && v->joints[k] < SKM_MAX_BONES && remap[v->joints[k]] >= 0) { first = remap[v->joints[k]]; break; }
            for (int k = 0; k < 4; k++) {
                int r = v->joints[k] < SKM_MAX_BONES ? remap[v->joints[k]] : -1;
                v->joints[k] = (u8)(v->weights[k] && r >= 0 ? r : (first >= 0 ? first : 0));
            }
        }
    }
    for (int i = 0; i < m->socket_count; i++) if (m->sockets[i].bone >= 0) m->sockets[i].bone = remap[m->sockets[i].bone];
    m->bone_count = n;
}

// ------------------------------------------------------------------ palette

static Xform mesh_only_world(const SkeletalMesh *m, const Xform *world, const Xform *leaf_local, const Xform *mo, int j) {
    const SkmBone *b = &m->bones[j];
    int p = b->parent;
    Xform pw = p < 0 ? world[0] : (m->bones[p].skel >= 0 ? world[m->bones[p].skel] : mo[p]);
    return xf_mul(pw, leaf_local ? leaf_local[j] : b->local);
}

static bool per_bone(const SkeletalMesh *m) {
    for (int j = 0; j < m->bone_count; j++) if (m->bones[j].has_fix) return true;
    return false;
}

static Xform file_bind(const SkmBone *b) { return b->has_fix ? xf_mul(b->bind_delta, b->bind_model) : b->bind_model; }

// A per-bone retarget mesh (another rig: a Mixamo skin) in its own proportions: every bone keeps the
// skeleton's rotation, but its position follows the mesh's own bone lengths - each joint is placed
// from its parent along the file's bind offset (in the parent's frame, which the retarget maps onto
// the skeleton bone's frame), the root moved up / down by the leg-length difference so the feet stay on
// the ground. With the skeleton's joint positions instead, a longer or shorter segment pinched or
// overlapped at every joint (elbows, knees, shoulders, hips). Bones the mesh lacks use the skeleton's
// own offsets.
void skm_proportioned_world(const SkeletalMesh *m, const SkeletonInstance *si, Xform *out) {
    const Skeleton *s = si->asset ? &si->asset->skel : NULL;
    int n = si->count;
    if (!s) { memcpy(out, si->world, sizeof(Xform) * (size_t)n); return; }
    int mesh_of[ANIM_MAX_BONES];
    for (int k = 0; k < n; k++) mesh_of[k] = -1;
    for (int j = 0; j < m->bone_count; j++) if (m->bones[j].skel >= 0 && m->bones[j].skel < n) mesh_of[m->bones[j].skel] = j;
    for (int k = 0; k < n; k++) {
        Xform w = si->world[k];
        int p = s->parent[k], mk = mesh_of[k];
        Xform fk = mk >= 0 ? file_bind(&m->bones[mk]) : s->bind_model[k];
        if (p < 0 || p >= n) {
            w.t.y += (fk.t.y - s->bind_model[k].t.y) * w.s.y;   // the root: the mesh's own hip height
        } else {
            int mp = mesh_of[p];
            vec3 off = mk >= 0 && mp >= 0 ? xf_point(xf_inverse(file_bind(&m->bones[mp])), fk.t) : s->bind[k].t;
            Xform pw = si->world[p];
            pw.t = out[p].t;
            w.t = xf_point(pw, off);
        }
        out[k] = w;
    }
    // The arms in their own lengths reach where the skeleton's hands are (the held item, the grips, the
    // hand poses stay right): two-bone IK from the mesh's shoulder, bending towards the skeleton's elbow;
    // the hand keeps the skeleton's rotation, its fingers follow it.
    static const int arms[2][3] = { { BR_L_ARM, BR_L_FOREARM, BR_L_HAND }, { BR_R_ARM, BR_R_FOREARM, BR_R_HAND } };
    for (int side = 0; side < 2; side++) {
        int ia = s->role[arms[side][0]], ib = s->role[arms[side][1]], ic = s->role[arms[side][2]];
        if (ia < 0 || ib < 0 || ic < 0 || ia >= n || ib >= n || ic >= n) continue;
        vec3 sh = out[ia].t, el = out[ib].t, wr = out[ic].t, tgt = si->world[ic].t;
        float a = v3_dist(sh, el), b = v3_dist(el, wr);
        if (a < 1e-4f || b < 1e-4f) continue;
        vec3 pole = v3_sub(si->world[ib].t, v3_lerp(si->world[ia].t, tgt, 0.5f));
        vec3 mid, end;
        anim_ik_two_bone(sh, tgt, pole, a, b, &mid, &end);
        quat r1 = quat_from_to(v3_norm(v3_sub(el, sh)), v3_norm(v3_sub(mid, sh)));
        quat r2 = quat_from_to(v3_norm(quat_rotate(r1, v3_sub(wr, el))), v3_norm(v3_sub(end, mid)));
        out[ia].r = quat_norm(quat_mul(r1, out[ia].r));
        out[ib].r = quat_norm(quat_mul(quat_mul(r2, r1), out[ib].r));
        out[ib].t = mid;
        vec3 d = v3_sub(end, wr);
        for (int k = 0; k < n; k++) {
            int q = k;
            while (q >= 0 && q != ic) q = s->parent[q];
            if (q == ic) out[k].t = v3_add(out[k].t, d);   // the hand and its fingers
        }
    }
}

int skm_gather_palette(const SkeletalMesh *m, const SkeletonInstance *si, const Xform *leaf_local, mat4 *out) {
    Xform mo[SKM_MAX_BONES];
    if (per_bone(m)) {
        static DG_THREAD_LOCAL Xform pw[ANIM_MAX_BONES];
        skm_proportioned_world(m, si, pw);
        for (int j = 0; j < m->bone_count; j++) {
            const SkmBone *b = &m->bones[j];
            mo[j] = b->skel >= 0 && b->skel < si->count ? pw[b->skel] : mesh_only_world(m, pw, leaf_local, mo, j);
            out[j] = mat4_mul(xf_to_mat4(mo[j]), xf_to_mat4(xf_inverse(file_bind(b))));
        }
        return m->bone_count;
    }
    for (int j = 0; j < m->bone_count; j++) {
        const SkmBone *b = &m->bones[j];
        if (b->skel >= 0 && b->skel < si->count) { out[j] = si->skin[b->skel]; continue; }
        mo[j] = mesh_only_world(m, si->world, leaf_local, mo, j);
        out[j] = mat4_mul(xf_to_mat4(mo[j]), xf_to_mat4(xf_inverse(b->bind_model)));
    }
    return m->bone_count;
}

bool skm_palette_equal(const SkeletalMesh *a, const SkeletalMesh *b) {
    if (a == b) return true;
    if (a->bone_count != b->bone_count) return false;
    for (int j = 0; j < a->bone_count; j++) {
        const SkmBone *x = &a->bones[j], *y = &b->bones[j];
        if (x->skel != y->skel || x->has_fix != y->has_fix) return false;
        if (x->has_fix && memcmp(&x->bind_fix, &y->bind_fix, sizeof(mat4))) return false;
        if (x->skel >= 0) continue;   // a skeleton bone: si->skin[skel] (bound meshes index the instance's skeleton)
        if (x->parent != y->parent || memcmp(&x->local, &y->local, sizeof(Xform)) || memcmp(&x->bind_model, &y->bind_model, sizeof(Xform))) return false;
    }
    return true;
}

Xform skm_bone_world(const SkeletalMesh *m, const SkeletonInstance *si, int mesh_bone) {
    Xform mo[SKM_MAX_BONES];
    static DG_THREAD_LOCAL Xform pw[ANIM_MAX_BONES];
    const Xform *world = si->world;
    if (per_bone(m)) { skm_proportioned_world(m, si, pw); world = pw; }
    for (int j = 0; j <= mesh_bone && j < m->bone_count; j++) {
        if (m->bones[j].skel >= 0) mo[j] = world[m->bones[j].skel];
        else mo[j] = mesh_only_world(m, world, NULL, mo, j);
    }
    return mo[mesh_bone];
}

bool skm_socket_world(const SkeletalMesh *m, const SkeletonInstance *si, const char *name, Xform *out) {
    for (int i = 0; i < m->socket_count; i++) {
        const SkelSocket *k = &m->sockets[i];
        if (strcmp(k->name, name) || k->bone < 0) continue;
        Xform bw[SKM_MAX_BONES];
        bw[k->bone] = skm_bone_world(m, si, k->bone);
        SkelSocket tmp = *k;
        *out = skel_socket_world(&tmp, bw);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ derived meshes

static void recompute_bounds(SkeletalMesh *m) {
    AABB bb = { { 1e9f, 1e9f, 1e9f }, { -1e9f, -1e9f, -1e9f } };
    for (int l = 0; l < m->data.lod_count; l++)
        for (u32 i = 0; i < m->data.lods[l].vcount; i++) {
            bb.min = v3_min(bb.min, m->data.lods[l].verts[i].pos);
            bb.max = v3_max(bb.max, m->data.lods[l].verts[i].pos);
        }
    m->data.bounds = bb;
}

bool skm_extract_region(const SkeletalMesh *src, const BoneSet *region, SkeletalMesh *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out, src, sizeof(*out));
    memset(out->data.lods, 0, sizeof(out->data.lods));
    out->data.lod_count = 0;
    out->source_hash = 0;
    for (int l = 0; l < src->data.lod_count; l++) {
        const SkinLod *a = &src->data.lods[l];
        u32 *remap = (u32 *)malloc(sizeof(u32) * DG_MAX(a->vcount, 1u));
        for (u32 i = 0; i < a->vcount; i++) remap[i] = 0xFFFFFFFFu;
        SkinLod *b = &out->data.lods[out->data.lod_count];
        b->verts = (SkinVertex *)malloc(sizeof(SkinVertex) * DG_MAX(a->vcount, 1u));
        b->vmat = (u8 *)malloc(DG_MAX(a->vcount, 1u));
        b->indices = (u32 *)malloc(sizeof(u32) * DG_MAX(a->icount, 1u));
        b->generated = a->generated;
        // Each triangle has one owner bone (whatever region is asked for), so disjoint regions tile
        // the body exactly: no holes, no doubled triangles at the seams.
        for (u32 t = 0; t + 2 < a->icount; t += 3) {
            if (!boneset_has(region, skm_triangle_bone(src, a, t))) continue;
            for (int k = 0; k < 3; k++) {
                u32 v = a->indices[t + k];
                if (remap[v] == 0xFFFFFFFFu) {
                    remap[v] = b->vcount;
                    b->verts[b->vcount] = a->verts[v];
                    b->vmat[b->vcount] = a->vmat[v];
                    b->vcount++;
                }
                b->indices[b->icount++] = remap[v];
            }
        }
        free(remap);
        if (!b->icount) { free(b->verts); free(b->vmat); free(b->indices); memset(b, 0, sizeof(*b)); continue; }
        out->data.lod_count++;
    }
    if (!out->data.lod_count) return false;
    skm_compact_bones(out);
    recompute_bounds(out);
    return true;
}

void skm_deform(SkeletalMesh *m, float inflate, vec3 scale) {
    for (int l = 0; l < m->data.lod_count; l++) {
        SkinLod *L = &m->data.lods[l];
        for (u32 i = 0; i < L->vcount; i++) {
            SkinVertex *v = &L->verts[i];
            int best = 0;
            for (int k = 1; k < 4; k++) if (v->weights[k] > v->weights[best]) best = k;
            const SkmBone *pb = &m->bones[DG_MIN((int)v->joints[best], m->bone_count - 1)];
            vec3 pivot = pb->has_fix ? xf_mul(pb->bind_delta, pb->bind_model).t : pb->bind_model.t;   // the vertices' bind space
            vec3 d = v3_sub(v->pos, pivot);
            v->pos = v3_add(v3_add(pivot, v3(d.x * scale.x, d.y * scale.y, d.z * scale.z)), v3_scale(v->normal, inflate));
        }
    }
    recompute_bounds(m);
}

u32 skm_indices_without(const SkeletalMesh *m, int lod, const BoneSet *hidden, u32 *out) {
    if (lod < 0 || lod >= m->data.lod_count) return 0;
    const SkinLod *L = &m->data.lods[lod];
    u32 n = 0;
    for (u32 t = 0; t + 2 < L->icount; t += 3) {
        bool all = true;
        for (int k = 0; k < 3 && all; k++) all = boneset_has(hidden, skm_vertex_bone(m, &L->verts[L->indices[t + k]]));
        if (all) continue;
        out[n++] = L->indices[t];
        out[n++] = L->indices[t + 1];
        out[n++] = L->indices[t + 2];
    }
    return n;
}

// ------------------------------------------------------------------ files

typedef struct SkmFileHeader { u32 magic, version, size, vsize; } SkmFileHeader;

bool skm_serialize(const SkeletalMesh *m, u8 **out_data, size_t *out_size) {
    size_t n = sizeof(SkmFileHeader) + sizeof(SkeletalMesh);
    for (int l = 0; l < m->data.lod_count; l++) n += (sizeof(SkinVertex) + 1) * m->data.lods[l].vcount + sizeof(u32) * m->data.lods[l].icount;
    u8 *p = (u8 *)calloc(1, n);
    if (!p) return false;
    SkmFileHeader h = { SKM_MAGIC, SKM_FILE_VERSION, (u32)sizeof(SkeletalMesh), (u32)sizeof(SkinVertex) };
    size_t o = 0;
    memcpy(p, &h, sizeof(h));
    o += sizeof(h);
    SkeletalMesh *hdr = (SkeletalMesh *)(p + o);
    memcpy(hdr, m, sizeof(*m));
    for (int l = 0; l < SKIN_MAX_LODS; l++) { hdr->data.lods[l].verts = NULL; hdr->data.lods[l].vmat = NULL; hdr->data.lods[l].indices = NULL; }
    o += sizeof(*m);
    for (int l = 0; l < m->data.lod_count; l++) {
        const SkinLod *L = &m->data.lods[l];
        memcpy(p + o, L->verts, sizeof(SkinVertex) * L->vcount);
        o += sizeof(SkinVertex) * L->vcount;
        memcpy(p + o, L->vmat, L->vcount);
        o += L->vcount;
        memcpy(p + o, L->indices, sizeof(u32) * L->icount);
        o += sizeof(u32) * L->icount;
    }
    *out_data = p;
    *out_size = n;
    return true;
}

static void errf(char *err, int err_size, const char *fmt, ...) {
    if (!err || err_size <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)err_size, fmt, ap);
    va_end(ap);
}

bool skm_deserialize(SkeletalMesh *m, const u8 *data, size_t size, char *err, int err_size) {
    SkmFileHeader h;
    memset(m, 0, sizeof(*m));
    if (size < sizeof(h) + sizeof(*m)) { errf(err, err_size, "skeletal mesh file truncated"); return false; }
    memcpy(&h, data, sizeof(h));
    if (h.magic != SKM_MAGIC || h.version != SKM_FILE_VERSION || h.size != sizeof(SkeletalMesh) || h.vsize != sizeof(SkinVertex)) {
        errf(err, err_size, "skeletal mesh file: wrong magic / version / layout (re-bake with tools/bake_mesh)");
        return false;
    }
    size_t o = sizeof(h);
    memcpy(m, data + o, sizeof(*m));
    o += sizeof(*m);
    if (m->data.lod_count < 0 || m->data.lod_count > SKIN_MAX_LODS || m->bone_count < 0 || m->bone_count > SKM_MAX_BONES) {
        memset(m, 0, sizeof(*m));
        errf(err, err_size, "skeletal mesh file: corrupt header");
        return false;
    }
    for (int l = 0; l < m->data.lod_count; l++) {
        SkinLod *L = &m->data.lods[l];
        size_t need = (sizeof(SkinVertex) + 1) * L->vcount + sizeof(u32) * L->icount;
        if (o + need > size) {
            for (int k = 0; k < l; k++) { free(m->data.lods[k].verts); free(m->data.lods[k].vmat); free(m->data.lods[k].indices); }
            memset(m, 0, sizeof(*m));
            errf(err, err_size, "skeletal mesh file truncated");
            return false;
        }
        L->verts = (SkinVertex *)malloc(sizeof(SkinVertex) * DG_MAX(L->vcount, 1u));
        memcpy(L->verts, data + o, sizeof(SkinVertex) * L->vcount);
        o += sizeof(SkinVertex) * L->vcount;
        L->vmat = (u8 *)malloc(DG_MAX(L->vcount, 1u));
        memcpy(L->vmat, data + o, L->vcount);
        o += L->vcount;
        L->indices = (u32 *)malloc(sizeof(u32) * DG_MAX(L->icount, 1u));
        memcpy(L->indices, data + o, sizeof(u32) * L->icount);
        o += sizeof(u32) * L->icount;
    }
    return true;
}

bool skm_write(const SkeletalMesh *m, const char *path) {
    u8 *p = NULL;
    size_t n = 0;
    if (!skm_serialize(m, &p, &n)) return false;
    bool ok = platform_write_file(path, p, n);
    free(p);
    return ok;
}

bool skm_read(SkeletalMesh *m, const char *path, char *err, int err_size) {
    size_t n = 0;
    char *d = platform_read_file(path, &n);
    if (!d) { errf(err, err_size, "cannot read %s", path); return false; }
    bool ok = skm_deserialize(m, (const u8 *)d, n, err, err_size);
    free(d);
    return ok;
}

// ------------------------------------------------------------------ batching

static const u64 *g_sort_keys;
static int cmp_order(const void *a, const void *b) {
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    u64 kx = g_sort_keys[x], ky = g_sort_keys[y];
    if (kx != ky) return kx < ky ? -1 : 1;
    return x < y ? -1 : (x > y ? 1 : 0);   // stable: submission order within a batch
}

u32 skin_batch_build(const u64 *keys, u32 n, u32 *order, SkinBatchRun *batches) {
    for (u32 i = 0; i < n; i++) order[i] = i;
    g_sort_keys = keys;
    qsort(order, n, sizeof(u32), cmp_order);
    u32 nb = 0;
    for (u32 k = 0; k < n; k++) {
        u64 key = keys[order[k]];
        if (nb && batches[nb - 1].key == key) batches[nb - 1].count++;
        else batches[nb++] = (SkinBatchRun){ key, k, 1 };
    }
    return nb;
}
