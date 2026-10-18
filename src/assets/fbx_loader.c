#include "fbx_loader.h"
#include "core/log.h"
#include "core/platform.h"
#include "ufbx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// ------------------------------------------------------------------ helpers

void fbx_file_stem(const char *path, char *out, size_t n) {
    const char *s = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') s = p + 1;
    dg_strcpy(out, n, s);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
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

bool fbx_guess_loop(const char *name) {
    // " to ": Mixamo transitions ("Braced Hang To Crouch", "Crouch To Stand") play once.
    static const char *once[] = { "turn", "jump", "death", "die", "hit", "land", "mantle", "vault", "pull", "reload", "fire",
                                  "melee", "attack", "equip", "holster", "climb", "start", "stop", " to " };
    for (int i = 0; i < ARRAY_COUNT(once); i++)
        if (contains_ci(name, once[i])) return false;
    return true;
}

u32 fbx_file_hash(const char *path) {
    size_t size = 0;
    char *data = platform_read_file(path, &size);
    if (!data) return 0;
    u32 h = anim_hash(data, size, ANIM_HASH_SEED);
    free(data);
    return h;
}

static ufbx_scene *load_scene(const char *path, char *err, int err_size) {
    ufbx_load_opts o;
    memset(&o, 0, sizeof(o));
    o.evaluate_skinning = false;
    o.generate_missing_normals = true;
    ufbx_error e;
    ufbx_scene *s = ufbx_load_file(path, &o, &e);
    if (!s && err) {
        char b[512];
        ufbx_format_error(b, sizeof(b), &e);
        snprintf(err, (size_t)err_size, "ufbx: %s: %s", path, b);
    }
    return s;
}

// File world -> model space: meters, 180 degree yaw (file faces +Z, game faces -Z).
typedef struct Conv { float unit; quat rot; } Conv;

static Conv make_conv(const ufbx_scene *s) {
    Conv c;
    c.unit = s->settings.unit_meters > 0 ? (float)s->settings.unit_meters : 0.01f;
    c.rot = quat_yaw(DG_PI);
    // Z-up files: rotate -90 deg about X first.
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

static vec3 conv_point(const Conv *c, ufbx_vec3 p) {
    return quat_rotate(c->rot, v3_scale(v3((float)p.x, (float)p.y, (float)p.z), c->unit));
}

static vec3 conv_dir(const Conv *c, ufbx_vec3 d) { return v3_norm(quat_rotate(c->rot, v3((float)d.x, (float)d.y, (float)d.z))); }

static bool node_is_bone(const ufbx_node *n) { return n->bone != NULL || n->attrib_type == UFBX_ELEMENT_BONE; }

// Nearest bone ancestor of a node (NULL if none).
static const ufbx_node *bone_parent(const ufbx_node *n) {
    for (const ufbx_node *p = n->parent; p; p = p->parent)
        if (node_is_bone(p)) return p;
    return NULL;
}

// ------------------------------------------------------------------ skeleton

static int collect_bones(const ufbx_scene *sc, const ufbx_node **out, int max) {
    // Depth-first from the root so parents come before children.
    int n = 0;
    const ufbx_node *stack[1024];
    int sp = 0;
    stack[sp++] = sc->root_node;
    while (sp > 0 && n < max) {
        const ufbx_node *node = stack[--sp];
        if (node_is_bone(node)) out[n++] = node;
        for (size_t i = node->children.count; i-- > 0;)
            if (sp < 1024) stack[sp++] = node->children.data[i];
    }
    return n;
}

bool fbx_load_skeleton(const char *path, Skeleton *out, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
    ufbx_scene *sc = load_scene(path, err, err_size);
    if (!sc) return false;
    Conv cv = make_conv(sc);
    const ufbx_node *bones[ANIM_MAX_BONES];
    int n = collect_bones(sc, bones, ANIM_MAX_BONES);
    if (n == 0) {
        if (err) snprintf(err, (size_t)err_size, "%s: no bones", path);
        ufbx_free_scene(sc);
        return false;
    }
    int roots = 0;
    Xform model[ANIM_MAX_BONES];
    out->count = n;
    for (int i = 0; i < n; i++) {
        dg_strcpy(out->names[i], ANIM_NAME_MAX, bones[i]->name.data);
        const ufbx_node *bp = bone_parent(bones[i]);
        out->parent[i] = -1;
        for (int j = 0; j < i; j++)
            if (bones[j] == bp) out->parent[i] = (i16)j;
        if (out->parent[i] < 0) roots++;
        model[i] = conv_matrix(&cv, &bones[i]->node_to_world);
        int p = out->parent[i];
        out->bind[i] = p >= 0 ? xf_mul(xf_inverse(model[p]), model[i]) : model[i];
        out->bind[i].s = v3(1, 1, 1);
    }
    if (roots > 1) log_warn("fbx: %s has %d root bones (using all; the first is the root)", path, roots);
    dg_strcpy(out->source, sizeof(out->source), path);
    skel_finalize(out);
    ufbx_free_scene(sc);
    return true;
}

// ------------------------------------------------------------------ skinned meshes

typedef struct VtxKey { SkinVertex v; u8 mat; } VtxKey;

static u32 vtx_hash(const VtxKey *k) { return anim_hash(k, sizeof(*k), ANIM_HASH_SEED); }

typedef struct LodBuild {
    SkinVertex *verts;
    u8 *vmat;
    u32 vcount, vcap;
    u32 *indices;
    u32 icount, icap;
    u32 *table;      // open-addressing hash -> vertex index + 1
    u32 table_size;
} LodBuild;

static u32 lod_add_vertex(LodBuild *b, const SkinVertex *v, u8 mat) {
    VtxKey k;
    memset(&k, 0, sizeof(k));
    k.v = *v;
    k.mat = mat;
    if (b->vcount * 2 >= b->table_size) {
        u32 ns = b->table_size ? b->table_size * 2 : 4096;
        u32 *nt = (u32 *)calloc(ns, sizeof(u32));
        for (u32 i = 0; i < b->vcount; i++) {
            VtxKey kk;
            memset(&kk, 0, sizeof(kk));
            kk.v = b->verts[i];
            kk.mat = b->vmat[i];
            u32 h = vtx_hash(&kk) & (ns - 1);
            while (nt[h]) h = (h + 1) & (ns - 1);
            nt[h] = i + 1;
        }
        free(b->table);
        b->table = nt;
        b->table_size = ns;
    }
    u32 h = vtx_hash(&k) & (b->table_size - 1);
    while (b->table[h]) {
        u32 i = b->table[h] - 1;
        if (b->vmat[i] == mat && !memcmp(&b->verts[i], v, sizeof(SkinVertex))) return i;
        h = (h + 1) & (b->table_size - 1);
    }
    if (b->vcount == b->vcap) {
        b->vcap = b->vcap ? b->vcap * 2 : 1024;
        b->verts = (SkinVertex *)realloc(b->verts, sizeof(SkinVertex) * b->vcap);
        b->vmat = (u8 *)realloc(b->vmat, b->vcap);
    }
    b->verts[b->vcount] = *v;
    b->vmat[b->vcount] = mat;
    b->table[h] = b->vcount + 1;
    return b->vcount++;
}

static void lod_add_index(LodBuild *b, u32 i) {
    if (b->icount == b->icap) {
        b->icap = b->icap ? b->icap * 2 : 4096;
        b->indices = (u32 *)realloc(b->indices, sizeof(u32) * b->icap);
    }
    b->indices[b->icount++] = i;
}

static int lod_from_name(const char *name) {
    const char *p = strstr(name, "_LOD");
    if (!p) p = strstr(name, "_lod");
    if (p && isdigit((unsigned char)p[4])) return DG_CLAMP(p[4] - '0', 0, SKIN_MAX_LODS - 1);
    return 0;
}

// Binding of a file's bones to the Skeleton asset: skeleton bones by (aliased) name, everything else
// becomes a mesh-only leaf bone parented to its nearest bone ancestor (never enters the Skeleton).
typedef struct BindCtx {
    const ufbx_scene *sc;
    Conv cv;
    const SkeletonAsset *sk;
    const BoneAliasTable *aliases;
    SkeletalMesh *m;
    const ufbx_node *node_of[SKM_MAX_BONES];   // file node of each mesh bone (NULL = not from a node)
} BindCtx;

// Bind pose of a bone node in model space: its skin cluster's bind matrix when it has one.
static Xform node_bind_world(const BindCtx *c, const ufbx_node *n) {
    for (size_t i = 0; i < c->sc->skin_clusters.count; i++) {
        const ufbx_skin_cluster *cl = c->sc->skin_clusters.data[i];
        if (cl->bone_node == n) return conv_matrix(&c->cv, &cl->bind_to_world);
    }
    return conv_matrix(&c->cv, &n->node_to_world);
}

static int bind_bone(BindCtx *c, const ufbx_node *bn, int depth) {
    SkeletalMesh *m = c->m;
    const Skeleton *s = &c->sk->skel;
    if (!bn || depth > 64) return skm_add_skel_bone(m, s, 0);
    for (int i = 0; i < m->bone_count; i++) if (c->node_of[i] == bn) return i;
    const char *nm = skm_alias_resolve(c->aliases, bn->name.data);
    int j = skel_find(s, nm);
    if (j >= 0) {
        int mi = skm_bone_of_skel(m, j);
        if (mi < 0) {
            mi = skm_add_skel_bone(m, s, j);
            if (mi < 0) return 0;
            if (nm != bn->name.data) m->aliased++;
            // File bind vs the reference pose (report; the vertices are retargeted onto the reference).
            Xform fb = node_bind_world(c, bn);
            m->bones[mi].bind_delta = xf_mul(fb, xf_inverse(s->bind_model[j]));
            m->bones[mi].bind_delta.s = v3(1, 1, 1);
        }
        c->node_of[mi] = bn;
        return mi;
    }
    // Mesh-only leaf: rides on its parent with the file's relative bind transform.
    const ufbx_node *pn = bone_parent(bn);
    int pi = pn ? bind_bone(c, pn, depth + 1) : skm_add_skel_bone(m, s, 0);
    if (m->bone_count >= SKM_MAX_BONES || pi < 0) return pi < 0 ? 0 : pi;
    Xform fb = node_bind_world(c, bn);
    Xform fp = pn ? node_bind_world(c, pn) : m->bones[pi].bind_model;
    Xform local = xf_mul(xf_inverse(fp), fb);
    local.s = v3(1, 1, 1);
    int k = m->bone_count++;
    SkmBone *b = &m->bones[k];
    memset(b, 0, sizeof(*b));
    dg_strcpy(b->name, sizeof(b->name), bn->name.data);
    b->skel = -1;
    b->parent = (i16)pi;
    b->local = local;
    b->bind_model = xf_mul(m->bones[pi].bind_model, local);
    b->bind_model.s = v3(1, 1, 1);
    b->bind_delta = xf_mul(fb, xf_inverse(b->bind_model));   // file bind = bind_delta * bind_model
    b->bind_delta.s = v3(1, 1, 1);
    b->bind_fix = mat4_identity();
    c->node_of[k] = bn;
    m->extra++;
    return k;
}

bool fbx_load_skeletal_mesh(const char *path, const SkeletonAsset *sk, const BoneAliasTable *aliases, SkeletalMesh *mesh, char *err, int err_size) {
    memset(mesh, 0, sizeof(*mesh));
    SkinMeshData *out = &mesh->data;
    ufbx_scene *sc = load_scene(path, err, err_size);
    if (!sc) return false;
    Conv cv = make_conv(sc);
    static BindCtx bc;
    memset(&bc, 0, sizeof(bc));
    bc.sc = sc;
    bc.cv = cv;
    bc.sk = sk;
    bc.aliases = aliases;
    bc.m = mesh;
    if (aliases) mesh->aliases = *aliases;
    LodBuild lods[SKIN_MAX_LODS];
    memset(lods, 0, sizeof(lods));
    // Retarget mode (docs/CHARACTERS.md "Proportions"). Each influence retargets a vertex rigidly from
    // the file's bind of its bone to that bone on the Skeleton reference pose. Baking that into the
    // vertex blends the influences' retargets, which is exact only while they agree: once the bones
    // bend apart, a vertex skinned with bone A's motion carries bone B's retarget (Mixamo skins on the
    // Dummy rig: up to 8 cm off at knees / shoulders). Bound of that error over every pose: the
    // weighted distance between a vertex's blend and each influence's own retarget. Above 0.1 mm the
    // vertices stay in the file's bind space and the palette retargets per bone (SkmBone.bind_fix).
    static Xform cluster_retarget[512];
    float blend_err = 0;
    for (size_t ni = 0; ni < sc->nodes.count; ni++) {
        const ufbx_node *node = sc->nodes.data[ni];
        const ufbx_mesh *m = node->mesh;
        if (!m || node_is_bone(node) || !m->skin_deformers.count) continue;
        const ufbx_skin_deformer *skin = m->skin_deformers.data[0];
        int ncl = (int)DG_MIN(skin->clusters.count, 512);
        for (int c = 0; c < ncl; c++) {
            const ufbx_skin_cluster *cl = skin->clusters.data[c];
            int j = bind_bone(&bc, cl->bone_node, 0);
            cluster_retarget[c] = xf_mul(mesh->bones[j].bind_model, xf_inverse(conv_matrix(&cv, &cl->bind_to_world)));
            cluster_retarget[c].s = v3(1, 1, 1);
        }
        for (size_t vid = 0; vid < skin->vertices.count && vid < m->vertices.count; vid++) {
            ufbx_skin_vertex sv = skin->vertices.data[vid];
            if (sv.num_weights < 2) continue;
            vec3 p = conv_point(&cv, ufbx_transform_position(&node->geometry_to_world, m->vertices.data[vid]));
            vec3 rp = v3(0, 0, 0);
            float rw = 0;
            for (u32 k = 0; k < sv.num_weights; k++) {
                ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                if ((int)sw.cluster_index >= ncl || sw.weight <= 0) continue;
                rp = v3_madd(rp, xf_point(cluster_retarget[sw.cluster_index], p), (float)sw.weight);
                rw += (float)sw.weight;
            }
            if (rw <= 1e-6f) continue;
            rp = v3_scale(rp, 1.0f / rw);
            float e = 0;
            for (u32 k = 0; k < sv.num_weights; k++) {
                ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                if ((int)sw.cluster_index >= ncl || sw.weight <= 0) continue;
                e += (float)sw.weight / rw * v3_dist(rp, xf_point(cluster_retarget[sw.cluster_index], p));
            }
            blend_err = fmaxf(blend_err, e);
        }
    }
    bool per_bone = blend_err > 1e-4f;
    mesh->retarget_blend_err_m = blend_err;
    for (size_t ni = 0; ni < sc->nodes.count; ni++) {
        const ufbx_node *node = sc->nodes.data[ni];
        const ufbx_mesh *m = node->mesh;
        if (!m || node_is_bone(node)) continue;
        int lod = lod_from_name(node->name.data);
        const ufbx_skin_deformer *skin = m->skin_deformers.count ? m->skin_deformers.data[0] : NULL;
        // Per-cluster mesh bone (skeleton bone or mesh-only leaf).
        int cluster_joint[512];
        // Retarget of each cluster's bind pose in this file. Baked (per_bone false): onto the bone's pose
        // on the Skeleton reference pose, so a character with other proportions keeps its shape on the
        // shared skeleton (identity for the canonical model itself). Per bone: onto the bone's file bind
        // (identity unless this mesh was bound in another pose than the first mesh using the bone); the
        // palette then does the reference-pose retarget per bone.
        int ncl = skin ? (int)DG_MIN(skin->clusters.count, 512) : 0;
        for (int c = 0; c < ncl; c++) {
            const ufbx_skin_cluster *cl = skin->clusters.data[c];
            const ufbx_node *bn = cl->bone_node;
            cluster_joint[c] = bind_bone(&bc, bn, 0);
            Xform mb = conv_matrix(&cv, &cl->bind_to_world);
            const SkmBone *jb = &mesh->bones[cluster_joint[c]];
            Xform target = per_bone ? xf_mul(jb->bind_delta, jb->bind_model) : jb->bind_model;
            cluster_retarget[c] = xf_mul(target, xf_inverse(mb));
            cluster_retarget[c].s = v3(1, 1, 1);
        }
        // Rigid (unskinned) meshes parented under a bone follow that bone.
        int rigid_joint = 0;
        if (!skin) {
            rigid_joint = skm_add_skel_bone(mesh, &sk->skel, 0);
            for (const ufbx_node *p = node->parent; p; p = p->parent)
                if (node_is_bone(p)) { rigid_joint = bind_bone(&bc, p, 0); break; }
        }
        // Materials -> global slots by name.
        int mat_slot[64];
        for (size_t k = 0; k < m->materials.count && k < 64; k++) {
            const ufbx_material *mt = m->materials.data[k];
            int slot = -1;
            for (int q = 0; q < out->material_count; q++)
                if (!strcmp(out->material_names[q], mt->name.data)) slot = q;
            if (slot < 0 && out->material_count < SKIN_MAX_MATERIALS) {
                slot = out->material_count++;
                dg_strcpy(out->material_names[slot], 48, mt->name.data);
                ufbx_vec4 c = mt->pbr.base_color.has_value ? mt->pbr.base_color.value_vec4 : mt->fbx.diffuse_color.value_vec4;
                if (!mt->pbr.base_color.has_value && !mt->fbx.diffuse_color.has_value) { c.x = c.y = c.z = 0.7; c.w = 1; }
                out->material_colors[slot] = v4((float)c.x, (float)c.y, (float)c.z, 1.0f);
            }
            mat_slot[k] = slot < 0 ? 0 : slot;
        }
        u32 tri[64 * 3];
        for (size_t f = 0; f < m->faces.count; f++) {
            ufbx_face face = m->faces.data[f];
            u32 nt = ufbx_triangulate_face(tri, ARRAY_COUNT(tri), m, face);
            int mslot = 0;
            if (m->face_material.count > f && m->face_material.data[f] < m->materials.count && m->face_material.data[f] < 64)
                mslot = mat_slot[m->face_material.data[f]];
            for (u32 t = 0; t < nt * 3; t++) {
                u32 ix = tri[t];
                SkinVertex v;
                memset(&v, 0, sizeof(v));
                ufbx_vec3 p = ufbx_get_vertex_vec3(&m->vertex_position, ix);
                ufbx_vec3 wp = ufbx_transform_position(&node->geometry_to_world, p);
                v.pos = conv_point(&cv, wp);
                if (m->vertex_normal.exists) {
                    ufbx_vec3 nn = ufbx_get_vertex_vec3(&m->vertex_normal, ix);
                    ufbx_vec3 wn = ufbx_transform_direction(&node->geometry_to_world, nn);
                    v.normal = conv_dir(&cv, wn);
                } else {
                    v.normal = v3(0, 1, 0);
                }
                if (m->vertex_uv.exists) {
                    ufbx_vec2 uv = ufbx_get_vertex_vec2(&m->vertex_uv, ix);
                    v.uv = v2((float)uv.x, 1.0f - (float)uv.y);
                }
                v.color = 0xFFFFFFFFu;
                u32 vid = m->vertex_indices.data[ix];
                if (skin && vid < skin->vertices.count) {
                    ufbx_skin_vertex sv = skin->vertices.data[vid];
                    // Blend the per-cluster retarget (linear, like the skinning itself).
                    vec3 rp = v3(0, 0, 0), rn = v3(0, 0, 0);
                    float rw = 0;
                    for (u32 k = 0; k < sv.num_weights; k++) {
                        ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                        if ((int)sw.cluster_index >= ncl) continue;
                        const Xform *rt = &cluster_retarget[sw.cluster_index];
                        float wgt = (float)sw.weight;
                        rp = v3_madd(rp, xf_point(*rt, v.pos), wgt);
                        rn = v3_madd(rn, quat_rotate(rt->r, v.normal), wgt);
                        rw += wgt;
                    }
                    if (rw > 1e-6f) {
                        v.pos = v3_scale(rp, 1.0f / rw);
                        if (v3_len2(rn) > 1e-12f) v.normal = v3_norm(rn);
                    }
                    // Top 4 weights, accumulated per canonical joint (remapped bones merge).
                    int js[16];
                    float ws[16];
                    int cnt = 0;
                    for (u32 k = 0; k < sv.num_weights; k++) {
                        ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                        if ((int)sw.cluster_index >= ncl) continue;
                        int j = cluster_joint[sw.cluster_index];
                        int at = -1;
                        for (int q = 0; q < cnt; q++) if (js[q] == j) at = q;
                        if (at < 0 && cnt < 16) { at = cnt++; js[at] = j; ws[at] = 0; }
                        if (at >= 0) ws[at] += (float)sw.weight;
                    }
                    for (int a = 0; a < cnt; a++)
                        for (int b = a + 1; b < cnt; b++)
                            if (ws[b] > ws[a]) { float tw = ws[a]; ws[a] = ws[b]; ws[b] = tw; int tj = js[a]; js[a] = js[b]; js[b] = tj; }
                    if (cnt > 4) cnt = 4;
                    float sum = 0;
                    for (int q = 0; q < cnt; q++) sum += ws[q];
                    if (cnt == 0 || sum <= 0) { v.joints[0] = (u8)rigid_joint; v.weights[0] = 255; }
                    else {
                        // Exactly 255 in all (the old round-each-then-remainder could reach 256).
                        skin_quantize_weights(ws, cnt, v.weights);
                        for (int q = 0; q < cnt; q++) v.joints[q] = (u8)js[q];
                    }
                } else {
                    v.joints[0] = (u8)rigid_joint;
                    v.weights[0] = 255;
                }
                u32 at = lod_add_vertex(&lods[lod], &v, (u8)mslot);
                lod_add_index(&lods[lod], at);
            }
        }
    }
    // Compact LOD list (keep order, skip empty).
    AABB bb = { { 1e9f, 1e9f, 1e9f }, { -1e9f, -1e9f, -1e9f } };
    for (int l = 0; l < SKIN_MAX_LODS; l++) {
        LodBuild *b = &lods[l];
        free(b->table);
        if (!b->icount) { free(b->verts); free(b->vmat); free(b->indices); continue; }
        SkinLod *dst = &out->lods[out->lod_count++];
        dst->verts = b->verts;
        dst->vcount = b->vcount;
        dst->indices = b->indices;
        dst->icount = b->icount;
        dst->vmat = b->vmat;
        for (u32 i = 0; i < b->vcount; i++) { bb.min = v3_min(bb.min, b->verts[i].pos); bb.max = v3_max(bb.max, b->verts[i].pos); }
    }
    out->bounds = bb;
    out->skel_hash = sk->skel.hash;
    out->remapped_bones = mesh->extra;
    dg_strcpy(out->source, sizeof(out->source), path);
    mesh->skeleton_hash = sk->skel.hash;
    dg_strcpy(mesh->skeleton_id, sizeof(mesh->skeleton_id), sk->id);
    // Per-bone retarget: palette entry = skeleton palette * inverse(bind_delta) (= world * inverse(file bind)).
    for (int i = 0; i < mesh->bone_count; i++) {
        SkmBone *b = &mesh->bones[i];
        Xform fix = xf_inverse(b->bind_delta);
        b->has_fix = per_bone && (v3_len(fix.t) > 1e-6f || quat_angle_between(fix.r, quat_identity()) > 1e-5f);
        b->bind_fix = b->has_fix ? xf_to_mat4(fix) : mat4_identity();
    }
    if (per_bone) log_info("fbx: %s: per-bone retarget (baked retarget would be off by up to %.1f mm)", path, blend_err * 1000.0f);
    // Report: skeleton bones the file doesn't have (simply not skinned), largest bind deltas.
    for (int j = 0; j < sk->skel.count; j++) {
        bool found = false;
        for (size_t ni = 0; ni < sc->nodes.count && !found; ni++)
            if (node_is_bone(sc->nodes.data[ni]) && skel_find(&sk->skel, skm_alias_resolve(aliases, sc->nodes.data[ni]->name.data)) == j) found = true;
        mesh->missing += !found;
    }
    for (int i = 0; i < mesh->bone_count; i++) {
        if (mesh->bones[i].skel < 0) continue;
        Xform d = mesh->bones[i].bind_delta;
        float ang = quat_angle_between(d.r, quat_identity()) * DG_RAD2DEG;
        // Translation delta of the bone origin (rotation about the model origin excluded).
        float dist = v3_dist(xf_point(d, sk->skel.bind_model[mesh->bones[i].skel].t), sk->skel.bind_model[mesh->bones[i].skel].t);
        if (dist > mesh->max_bind_delta_m) mesh->max_bind_delta_m = dist;
        if (ang > mesh->max_bind_delta_deg) mesh->max_bind_delta_deg = ang;
    }
    if (!out->material_count) {
        out->material_count = 1;
        dg_strcpy(out->material_names[0], 48, "default");
        out->material_colors[0] = v4(0.7f, 0.7f, 0.7f, 1);
    }
    ufbx_free_scene(sc);
    if (!out->lod_count) {
        if (err) snprintf(err, (size_t)err_size, "%s: no meshes", path);
        return false;
    }
    return true;
}

void skin_mesh_free(SkinMeshData *m) {
    for (int l = 0; l < m->lod_count; l++) {
        free(m->lods[l].verts);
        free(m->lods[l].indices);
        free(m->lods[l].vmat);
    }
    memset(m, 0, sizeof(*m));
}

// ------------------------------------------------------------------ clips

static ufbx_matrix compose_world(const ufbx_anim *anim, const ufbx_node *n, double time) {
    // World = parent world * local (ufbx resolves pivots / pre-post rotation per node).
    const ufbx_node *chain[64];
    int c = 0;
    for (const ufbx_node *p = n; p && c < 64; p = p->parent) chain[c++] = p;
    ufbx_matrix w = ufbx_identity_matrix;
    for (int i = c - 1; i >= 0; i--) {
        ufbx_transform t = ufbx_evaluate_transform(anim, chain[i], time);
        ufbx_matrix lm = ufbx_transform_to_matrix(&t);
        w = ufbx_matrix_mul(&w, &lm);
    }
    return w;
}

static float yaw_of(quat r, vec3 fwd_local) {
    vec3 f = quat_rotate(r, fwd_local);
    return atan2f(-f.x, -f.z);
}

int fbx_bake_clips(const char *path, const Skeleton *canon, const FbxBakeOptions *opt, Clip *out, int max, char *err, int err_size) {
    ufbx_scene *sc = load_scene(path, err, err_size);
    if (!sc) return 0;
    Conv cv = make_conv(sc);
    float hz = opt && opt->sample_hz > 0 ? opt->sample_hz : ANIM_DEFAULT_RATE;
    char stem[64];
    fbx_file_stem(path, stem, sizeof(stem));
    // Map canonical bones -> file nodes by name.
    const ufbx_node *map[ANIM_MAX_BONES];
    int missing = 0;
    for (int i = 0; i < canon->count; i++) {
        map[i] = ufbx_find_node(sc, canon->names[i]);
        if (!map[i]) missing++;
    }
    if (missing == canon->count) {
        if (err) snprintf(err, (size_t)err_size, "%s: no bone of the canonical skeleton found", path);
        ufbx_free_scene(sc);
        return 0;
    }
    if (missing) log_warn("anim: %s lacks %d canonical bones (bind pose used)", path, missing);
    int written = 0;
    // Files that also contain a mesh often carry a 1-frame "bind" take; skip takes shorter than 2 frames.
    for (size_t si = 0; si < sc->anim_stacks.count && written < max; si++) {
        const ufbx_anim_stack *st = sc->anim_stacks.data[si];
        double t0 = st->time_begin, t1 = st->time_end;
        int frames = (int)lrint((t1 - t0) * hz) + 1;
        if (frames < 3) continue;
        Clip *c = &out[written];
        memset(c, 0, sizeof(*c));
        int valid_takes = 0;
        for (size_t k = 0; k < sc->anim_stacks.count; k++) {
            const ufbx_anim_stack *s2 = sc->anim_stacks.data[k];
            if ((int)lrint((s2->time_end - s2->time_begin) * hz) + 1 >= 3) valid_takes++;
        }
        if (valid_takes > 1) snprintf(c->name, sizeof(c->name), "%s:%s", stem, st->name.data);
        else dg_strcpy(c->name, sizeof(c->name), stem);
        c->rate = hz;
        c->frames = frames;
        c->duration = (float)(frames - 1) / hz;
        c->bones = canon->count;
        c->loop = opt && opt->loop_override >= 0 ? opt->loop_override != 0 : fbx_guess_loop(stem);
        c->speed = 1.0f;
        c->additive_ref = -1;
        c->blend_in = 0.2f;
        c->blend_out = 0.2f;
        c->data = (Xform *)malloc(sizeof(Xform) * (size_t)frames * (size_t)canon->count);
        c->root_t = (vec3 *)calloc((size_t)frames, sizeof(vec3));
        c->root_yaw = (float *)calloc((size_t)frames, sizeof(float));
        Xform model[ANIM_MAX_BONES];
        for (int f = 0; f < frames; f++) {
            double t = t0 + (double)f / hz;
            if (t > t1) t = t1;
            Xform *dst = c->data + (size_t)f * canon->count;
            for (int i = 0; i < canon->count; i++) {
                int p = canon->parent[i];
                if (map[i]) {
                    ufbx_matrix wm = compose_world(st->anim, map[i], t);
                    model[i] = conv_matrix(&cv, &wm);
                } else {
                    model[i] = p >= 0 ? xf_mul(model[p], canon->bind[i]) : canon->bind[i];
                }
                dst[i] = p >= 0 ? xf_mul(xf_inverse(model[p]), model[i]) : model[i];
                dst[i].s = v3(1, 1, 1);
                // Bone lengths: keep the canonical offsets for non-root bones (models share the rig).
                if (p >= 0) dst[i].t = canon->bind[i].t;
            }
        }
        // ---- root motion extraction on the root bone (index 0)
        // Clips keep the facing they were authored with: the file's forward is the canonical forward
        // (Mixamo clips and the canonical rig share it). The hips' own yaw is part of the motion: a
        // bladed rifle stance has them 40 deg off the aim, a diagonal jog's start frame 10-20 deg off
        // its travel, so turning each clip until its first-frame hips face forward would point guns
        // and diagonals the wrong way.
        vec3 fwd_local = quat_rotate(quat_conj(canon->bind_model[0].r), v3(0, 0, -1));
        Xform *first = c->data, *last = c->data + (size_t)(frames - 1) * canon->count;
        vec3 p0 = first[0].t, pend = last[0].t;
        float y0 = yaw_of(first[0].r, fwd_local);
        float prev = y0, unwrap = 0;
        float *yaw = (float *)malloc(sizeof(float) * (size_t)frames);
        for (int f = 0; f < frames; f++) {
            float y = yaw_of(c->data[(size_t)f * canon->count].r, fwd_local);
            float d = y - prev;
            while (d > DG_PI) d -= 2 * DG_PI;
            while (d < -DG_PI) d += 2 * DG_PI;
            unwrap += d;
            prev = y;
            yaw[f] = unwrap;
        }
        float yend = yaw[frames - 1];
        // A one-shot whose hips end 45 deg or more from where they started is a turn: its yaw is
        // root motion. Less is the body's own sway (a jump squaring up, a climb), which stays.
        bool turn = fabsf(yend) >= 0.25f * DG_PI;
        for (int f = 0; f < frames; f++) {
            float u = (float)f / (float)(frames - 1);
            vec3 p = c->data[(size_t)f * canon->count].t;
            vec3 travel = c->loop ? v3_scale(v3_flat(v3_sub(pend, p0)), u) : v3_flat(v3_sub(p, p0));
            float ytrack = c->loop ? yend * u : (turn ? yaw[f] : 0.0f);
            c->root_t[f] = travel;
            c->root_yaw[f] = ytrack;
        }
        // In place: remove travel + yaw about the start position, then recenter over the bind.
        vec3 mean = v3(0, 0, 0);
        for (int f = 0; f < frames; f++) {
            Xform *r = &c->data[(size_t)f * canon->count];
            quat unyaw = quat_yaw(-c->root_yaw[f]);
            vec3 rel = v3_sub(v3_sub(r->t, p0), c->root_t[f]);
            rel = quat_rotate(unyaw, rel);
            r->t = v3_add(p0, rel);
            r->r = quat_norm(quat_mul(unyaw, r->r));
            mean = v3_add(mean, r->t);
        }
        mean = v3_scale(mean, 1.0f / (float)frames);
        vec3 center = c->loop ? mean : p0;
        vec3 off = v3_flat(v3_sub(center, canon->bind[0].t));
        for (int f = 0; f < frames; f++) {
            Xform *r = &c->data[(size_t)f * canon->count];
            r->t = v3_sub(r->t, off);
        }
        free(yaw);
        // Seamless loops: spread the last-frame -> first-frame difference over the whole clip
        // (per bone, local space), so the cycle wraps without a pop.
        if (c->loop && frames > 2) {
            const Xform *f0 = c->data;
            const Xform *fl = c->data + (size_t)(frames - 1) * canon->count;
            for (int i = 0; i < canon->count; i++) {
                vec3 dt_ = v3_sub(f0[i].t, fl[i].t);
                quat dr = quat_norm(quat_mul(f0[i].r, quat_conj(fl[i].r)));
                if (dr.w < 0) { dr.x = -dr.x; dr.y = -dr.y; dr.z = -dr.z; dr.w = -dr.w; }
                for (int f = 1; f < frames; f++) {
                    float u = (float)f / (float)(frames - 1);
                    Xform *x = &c->data[(size_t)f * canon->count + i];
                    x->t = v3_madd(x->t, dt_, u);
                    x->r = quat_norm(quat_mul(quat_nlerp(quat_identity(), dr, u), x->r));
                }
            }
        }
        float dist = v3_len(c->root_t[frames - 1]);
        c->avg_speed = c->duration > 0 ? dist / c->duration : 0;
        clip_detect_markers(c, canon);
        u32 h = anim_hash_str(c->name, ANIM_HASH_SEED);
        h = anim_hash(c->data, sizeof(Xform) * (size_t)frames * (size_t)canon->count, h);
        c->hash = h;
        written++;
    }
    ufbx_free_scene(sc);
    if (!written && err) snprintf(err, (size_t)err_size, "%s: no animation takes", path);
    return written;
}
