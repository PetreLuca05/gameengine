#include "skeleton.h"
#include "assets/anim_cache.h"
#include "assets/fbx_loader.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define SKEL_MAGIC 0x4B534744u   // "DGSK"

int boneset_count(const BoneSet *s) {
    int n = 0;
    for (int w = 0; w < 2; w++)
        for (u64 v = s->bits[w]; v; v &= v - 1) n++;
    return n;
}

static void errf(char *err, int err_size, const char *fmt, ...) {
    if (!err || err_size <= 0) return;
    size_t len = strlen(err);
    if ((int)len >= err_size - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + len, (size_t)err_size - len, fmt, ap);
    va_end(ap);
}

static int find_bone(const Skeleton *s, const char *name) {
    int b = skel_find(s, name);
    return b >= 0 ? b : skel_find_suffix(s, name);
}

// ------------------------------------------------------------------ groups

static void add_chain(const Skeleton *s, BoneSet *set, int root) {
    if (root < 0) return;
    for (int i = 0; i < s->count; i++)
        if (skel_is_descendant(s, i, root)) boneset_add(set, i);
}

static void remove_chain(const Skeleton *s, BoneSet *set, int root) {
    if (root < 0) return;
    for (int i = 0; i < s->count; i++)
        if (skel_is_descendant(s, i, root)) set->bits[i >> 6] &= ~((u64)1 << (i & 63));
}

static BoneGroup *group_slot(SkeletonAsset *a, const char *name) {
    for (int g = 0; g < a->group_count; g++)
        if (!strcmp(a->groups[g].name, name)) return &a->groups[g];
    if (a->group_count >= SKEL_MAX_GROUPS) return NULL;
    BoneGroup *g = &a->groups[a->group_count++];
    memset(g, 0, sizeof(*g));
    dg_strcpy(g->name, sizeof(g->name), name);
    return g;
}

// Built-in groups from the bone roles (every humanoid skeleton gets them).
static void default_groups(SkeletonAsset *a) {
    const Skeleton *s = &a->skel;
    BoneGroup *g;
    if ((g = group_slot(a, "all")))
        for (int i = 0; i < s->count; i++) boneset_add(&g->bones, i);
    if ((g = group_slot(a, "upper_body"))) add_chain(s, &g->bones, s->role[BR_SPINE]);
    if ((g = group_slot(a, "lower_body"))) {
        for (int i = 0; i < s->count; i++) boneset_add(&g->bones, i);
        remove_chain(s, &g->bones, s->role[BR_SPINE]);
    }
    if ((g = group_slot(a, "spine"))) {
        add_chain(s, &g->bones, s->role[BR_SPINE]);
        remove_chain(s, &g->bones, s->role[BR_NECK]);
        remove_chain(s, &g->bones, s->role[BR_L_SHOULDER]);
        remove_chain(s, &g->bones, s->role[BR_R_SHOULDER]);
    }
    if ((g = group_slot(a, "head"))) add_chain(s, &g->bones, s->role[BR_NECK]);
    if ((g = group_slot(a, "arms"))) { add_chain(s, &g->bones, s->role[BR_L_SHOULDER]); add_chain(s, &g->bones, s->role[BR_R_SHOULDER]); }
    if ((g = group_slot(a, "arm_l"))) add_chain(s, &g->bones, s->role[BR_L_SHOULDER]);
    if ((g = group_slot(a, "arm_r"))) add_chain(s, &g->bones, s->role[BR_R_SHOULDER]);
    if ((g = group_slot(a, "hands"))) { add_chain(s, &g->bones, s->role[BR_L_HAND]); add_chain(s, &g->bones, s->role[BR_R_HAND]); }
    if ((g = group_slot(a, "fingers"))) {
        add_chain(s, &g->bones, s->role[BR_L_HAND]);
        add_chain(s, &g->bones, s->role[BR_R_HAND]);
        g->bones.bits[s->role[BR_L_HAND] >> 6] &= ~((u64)1 << (s->role[BR_L_HAND] & 63));
        g->bones.bits[s->role[BR_R_HAND] >> 6] &= ~((u64)1 << (s->role[BR_R_HAND] & 63));
    }
    if ((g = group_slot(a, "legs"))) { add_chain(s, &g->bones, s->role[BR_L_UPLEG]); add_chain(s, &g->bones, s->role[BR_R_UPLEG]); }
    if ((g = group_slot(a, "feet"))) { add_chain(s, &g->bones, s->role[BR_L_FOOT]); add_chain(s, &g->bones, s->role[BR_R_FOOT]); }
    // Name-based groups (rigs with hair / face bones); empty on the Dummy rig.
    if ((g = group_slot(a, "hair")))
        for (int i = 0; i < s->count; i++) if (strstr(s->names[i], "Hair") || strstr(s->names[i], "hair")) boneset_add(&g->bones, i);
    if ((g = group_slot(a, "face"))) {
        int head = s->role[BR_HEAD];
        for (int i = 0; i < s->count; i++)
            if (head >= 0 && i != head && skel_is_descendant(s, i, head) && i != s->role[BR_HEAD_END] && !boneset_has(&a->groups[skel_asset_find_group(a, "hair")].bones, i))
                boneset_add(&g->bones, i);
    }
}

// ------------------------------------------------------------------ build

void skel_asset_from_skeleton(SkeletonAsset *a, const Skeleton *s, const char *id, const char *source, u32 source_hash) {
    memset(a, 0, sizeof(*a));   // padding included: the .skel bytes are deterministic
    a->skel = *s;
    dg_strcpy(a->id, sizeof(a->id), id ? id : "human");
    dg_strcpy(a->source, sizeof(a->source), source ? source : s->source);
    a->source_hash = source_hash;
    for (int i = 0; i < s->count; i++) {
        a->inv_bind[i] = xf_to_mat4(xf_inverse(s->bind_model[i]));
        a->base_pose[i] = s->bind[i];
    }
    for (int l = 0; l < SKEL_LOD_COUNT; l++)
        for (int i = 0; i < s->count; i++)
            if (l < 2 || s->lod_keep[i]) boneset_add(&a->lod_bones[l], i);
    default_groups(a);
}

static vec3 jvec3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}

static const char *jstr(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static void meta_hash(SkeletonAsset *a) {
    u32 h = ANIM_HASH_SEED;
    h = anim_hash(&a->socket_count, sizeof(int), h);
    for (int i = 0; i < a->socket_count; i++) {
        h = anim_hash_str(a->sockets[i].name, h);
        h = anim_hash(&a->sockets[i].bone, sizeof(int), h);
        h = anim_hash(&a->sockets[i].rel, sizeof(Xform), h);
    }
    for (int i = 0; i < a->vbone_count; i++) {
        h = anim_hash_str(a->vbones[i].name, h);
        h = anim_hash(&a->vbones[i].source, sizeof(i16) * 2, h);
    }
    for (int i = 0; i < a->group_count; i++) {
        h = anim_hash_str(a->groups[i].name, h);
        h = anim_hash(&a->groups[i].bones, sizeof(BoneSet), h);
    }
    h = anim_hash(a->base_pose, sizeof(Xform) * (size_t)a->skel.count, h);
    a->meta_hash = h;
}

bool skel_asset_apply_meta(SkeletonAsset *a, const char *text, char *err, int err_size) {
    const Skeleton *s = &a->skel;
    cJSON *root = cJSON_Parse(text);
    if (!root) { errf(err, err_size, "skeleton %s: JSON parse error\n", a->id); return false; }
    const cJSON *e;
    // Sockets: { "name": { "bone": "Head", "offset": [x, y, z], "rotation": [x, y, z], "space": "bind" | "bone" } }
    a->socket_count = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "sockets")) {
        if (!cJSON_IsObject(e) || a->socket_count >= SKEL_MAX_SOCKETS) continue;
        SkelSocket *k = &a->sockets[a->socket_count++];
        memset(k, 0, sizeof(*k));
        dg_strcpy(k->name, sizeof(k->name), e->string);
        dg_strcpy(k->bone_name, sizeof(k->bone_name), jstr(e, "bone", "Head"));
        k->offset = jvec3(e, "offset", v3(0, 0, 0));
        k->rotation_deg = jvec3(e, "rotation", v3(0, 0, 0));
        k->bind_space = strcmp(jstr(e, "space", "bind"), "bone") != 0;
        skel_socket_resolve(k, s);
        if (k->bone < 0) errf(err, err_size, "skeleton %s: socket '%s': unknown bone '%s'\n", a->id, k->name, k->bone_name);
    }
    // Virtual bones: { "VB hand_l_in_r": { "source": "RightHand", "target": "LeftHand" } }
    a->vbone_count = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "virtual_bones")) {
        if (!cJSON_IsObject(e) || a->vbone_count >= SKEL_MAX_VBONES) continue;
        VirtualBone *v = &a->vbones[a->vbone_count++];
        memset(v, 0, sizeof(*v));
        dg_strcpy(v->name, sizeof(v->name), e->string);
        dg_strcpy(v->source_name, sizeof(v->source_name), jstr(e, "source", ""));
        dg_strcpy(v->target_name, sizeof(v->target_name), jstr(e, "target", ""));
        v->source = (i16)find_bone(s, v->source_name);
        v->target = (i16)find_bone(s, v->target_name);
        if (v->source < 0 || v->target < 0) errf(err, err_size, "skeleton %s: virtual bone '%s': unknown source / target\n", a->id, v->name);
    }
    // Groups: { "name": { "chains": [bones with descendants], "bones": [single bones], "groups": [other groups],
    //                     "exclude_chains": [...], "exclude": [...] } } (added to / replacing the built-in ones).
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "groups")) {
        if (!cJSON_IsObject(e)) continue;
        BoneGroup *g = group_slot(a, e->string);
        if (!g) { errf(err, err_size, "skeleton %s: too many groups\n", a->id); break; }
        memset(&g->bones, 0, sizeof(g->bones));
        const cJSON *n;
        cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(e, "groups")) {
            int gi = cJSON_IsString(n) ? skel_asset_find_group(a, n->valuestring) : -1;
            if (gi >= 0) boneset_or(&g->bones, &a->groups[gi].bones);
        }
        cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(e, "chains")) if (cJSON_IsString(n)) add_chain(s, &g->bones, find_bone(s, n->valuestring));
        cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(e, "bones")) if (cJSON_IsString(n)) boneset_add(&g->bones, find_bone(s, n->valuestring));
        cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(e, "exclude_chains")) if (cJSON_IsString(n)) remove_chain(s, &g->bones, find_bone(s, n->valuestring));
        cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(e, "exclude")) {
            int b = cJSON_IsString(n) ? find_bone(s, n->valuestring) : -1;
            if (b >= 0) g->bones.bits[b >> 6] &= ~((u64)1 << (b & 63));
        }
    }
    // Retarget base pose: per-bone local rotation offsets from the bind ({ "LeftArm": { "rotation": [x, y, z] } }),
    // e.g. to describe the canonical rig in an A-pose when the bind is a T-pose.
    for (int i = 0; i < s->count; i++) a->base_pose[i] = s->bind[i];
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "base_pose")) {
        int b = cJSON_IsObject(e) ? find_bone(s, e->string) : -1;
        if (b < 0) { errf(err, err_size, "skeleton %s: base_pose: unknown bone '%s'\n", a->id, e->string ? e->string : "?"); continue; }
        vec3 d = jvec3(e, "rotation", v3(0, 0, 0));
        quat q = quat_euler(d.y * DG_DEG2RAD, d.x * DG_DEG2RAD, d.z * DG_DEG2RAD);
        a->base_pose[b].r = quat_norm(quat_mul(s->bind[b].r, q));
    }
    cJSON_Delete(root);
    meta_hash(a);
    return true;
}

// ------------------------------------------------------------------ serialization

typedef struct SkelFileHeader { u32 magic, version, size, source_hash; } SkelFileHeader;

bool skel_asset_serialize(const SkeletonAsset *a, u8 **out_data, size_t *out_size) {
    size_t n = sizeof(SkelFileHeader) + sizeof(SkeletonAsset);
    u8 *p = (u8 *)calloc(1, n);
    if (!p) return false;
    SkelFileHeader h = { SKEL_MAGIC, SKEL_FILE_VERSION, (u32)sizeof(SkeletonAsset), a->source_hash };
    memcpy(p, &h, sizeof(h));
    memcpy(p + sizeof(h), a, sizeof(*a));
    *out_data = p;
    *out_size = n;
    return true;
}

bool skel_asset_deserialize(SkeletonAsset *a, const u8 *data, size_t size, char *err, int err_size) {
    SkelFileHeader h;
    if (size < sizeof(h)) { errf(err, err_size, "skeleton file truncated"); return false; }
    memcpy(&h, data, sizeof(h));
    if (h.magic != SKEL_MAGIC || h.version != SKEL_FILE_VERSION || h.size != sizeof(SkeletonAsset) || size != sizeof(h) + sizeof(SkeletonAsset)) {
        errf(err, err_size, "skeleton file: wrong magic / version / layout (re-bake with tools/bake_skeleton)");
        return false;
    }
    memcpy(a, data + sizeof(h), sizeof(*a));
    return true;
}

bool skel_asset_write(const SkeletonAsset *a, const char *path) {
    u8 *p = NULL;
    size_t n = 0;
    if (!skel_asset_serialize(a, &p, &n)) return false;
    bool ok = platform_write_file(path, p, n);
    free(p);
    return ok;
}

bool skel_asset_read(SkeletonAsset *a, const char *path, char *err, int err_size) {
    size_t n = 0;
    char *d = platform_read_file(path, &n);
    if (!d) { errf(err, err_size, "cannot read %s", path); return false; }
    bool ok = skel_asset_deserialize(a, (const u8 *)d, n, err, err_size);
    free(d);
    return ok;
}

u32 skel_asset_hash(const SkeletonAsset *a) {
    u32 h = anim_hash(&a->skel.hash, sizeof(u32), ANIM_HASH_SEED);
    return anim_hash(&a->meta_hash, sizeof(u32), h);
}

// ------------------------------------------------------------------ load

bool skel_asset_load(SkeletonAsset *a, const char *id, const char *fallback_source, char *err, int err_size) {
    char rel[256], buf[512], skel_path[512];
    snprintf(rel, sizeof(rel), "assets/skeletons/%s.json", id);
    char *meta = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
    char source[128];
    dg_strcpy(source, sizeof(source), fallback_source ? fallback_source : "assets/human/Dummy.fbx");
    if (meta) {
        cJSON *root = cJSON_Parse(meta);
        if (root) dg_strcpy(source, sizeof(source), jstr(root, "source", source));
        cJSON_Delete(root);
    }
    char fbx[512];
    dg_strcpy(fbx, sizeof(fbx), platform_data_path(source, buf, sizeof(buf)));
    u32 fbx_hash = fbx_file_hash(fbx);
    snprintf(rel, sizeof(rel), "assets/skeletons/%s.skel", id);
    dg_strcpy(skel_path, sizeof(skel_path), platform_data_path(rel, buf, sizeof(buf)));
    bool have = false;
    // Baked .skel: used when it matches the rig FBX, or when the FBX isn't shipped.
    if (platform_file_exists(skel_path)) {
        char e2[256] = { 0 };
        SkeletonAsset *tmp = (SkeletonAsset *)malloc(sizeof(SkeletonAsset));
        if (skel_asset_read(tmp, skel_path, e2, sizeof(e2)) && (!fbx_hash || tmp->source_hash == fbx_hash)) {
            *a = *tmp;
            have = true;
        } else if (e2[0]) {
            log_warn("skeleton %s: %s", id, e2);
        }
        free(tmp);
    }
    if (!have) {
        if (!fbx_hash) {
            errf(err, err_size, "skeleton %s: rig %s not found (and no baked %s)", id, source, rel);
            free(meta);
            return false;
        }
        Skeleton *s = (Skeleton *)malloc(sizeof(Skeleton));
        if (!anim_cache_skeleton(fbx, s, err, err_size)) { free(s); free(meta); return false; }
        skel_asset_from_skeleton(a, s, id, source, fbx_hash);
        free(s);
    }
    // The authored metadata always wins (sockets / groups edited without a re-bake).
    if (meta) {
        char e3[1024] = { 0 };
        skel_asset_apply_meta(a, meta, e3, sizeof(e3));
        if (e3[0]) log_warn("%s", e3);
        free(meta);
    } else {
        meta_hash(a);
    }
    return true;
}

// ------------------------------------------------------------------ queries

int skel_asset_find_socket(const SkeletonAsset *a, const char *name) {
    for (int i = 0; i < a->socket_count; i++) if (!strcmp(a->sockets[i].name, name)) return i;
    return -1;
}
const SkelSocket *skel_asset_socket(const SkeletonAsset *a, const char *name) {
    int i = skel_asset_find_socket(a, name);
    return i >= 0 ? &a->sockets[i] : NULL;
}
int skel_asset_find_vbone(const SkeletonAsset *a, const char *name) {
    for (int i = 0; i < a->vbone_count; i++) if (!strcmp(a->vbones[i].name, name)) return i;
    return -1;
}
int skel_asset_find_group(const SkeletonAsset *a, const char *name) {
    for (int i = 0; i < a->group_count; i++) if (!strcmp(a->groups[i].name, name)) return i;
    return -1;
}

bool skel_asset_group_mask(const SkeletonAsset *a, const char *name, BoneMask *out) {
    int g = skel_asset_find_group(a, name);
    if (g < 0) return false;
    for (int i = 0; i < ANIM_MAX_BONES; i++) out->w[i] = boneset_has(&a->groups[g].bones, i) ? 1.0f : 0.0f;
    return true;
}

static quat socket_euler(vec3 deg) { return quat_euler(deg.y * DG_DEG2RAD, deg.x * DG_DEG2RAD, deg.z * DG_DEG2RAD); }

void skel_socket_resolve(SkelSocket *k, const Skeleton *s) {
    k->bone = find_bone(s, k->bone_name);
    if (k->bone < 0) { k->rel = xf_identity(); return; }
    quat rot = socket_euler(k->rotation_deg);
    if (k->bind_space) {
        Xform bind = s->bind_model[k->bone];
        Xform want = xf_make(v3_add(bind.t, k->offset), rot);
        k->rel = xf_mul(xf_inverse(bind), want);
    } else {
        k->rel = xf_make(k->offset, rot);
    }
    k->rel.s = v3(1, 1, 1);
}

Xform skel_socket_world(const SkelSocket *k, const Xform *bone_world) {
    Xform b = bone_world[k->bone], out;
    float sc = b.s.x;
    // Bone world transforms carry the character scale: scale the offset, keep the rotation pure.
    out.t = v3_add(b.t, quat_rotate(b.r, v3_scale(k->rel.t, sc)));
    out.r = quat_norm(quat_mul(b.r, k->rel.r));
    out.s = v3(1, 1, 1);
    return out;
}

Xform skel_vbone_xform(const VirtualBone *v, const Xform *bones) { return bones[v->target]; }

Xform skel_vbone_local(const VirtualBone *v, const Xform *bones) {
    Xform l = xf_mul(xf_inverse(bones[v->source]), bones[v->target]);
    l.s = v3(1, 1, 1);
    return l;
}

// ------------------------------------------------------------------ instance

void skel_inst_init(SkeletonInstance *si, const SkeletonAsset *a) {
    memset(si, 0, sizeof(*si));
    si->asset = a;
    si->count = a->skel.count;
    for (int i = 0; i < si->count; i++) {
        si->world[i] = a->skel.bind_model[i];
        si->skin[i] = mat4_identity();
    }
}

void skel_inst_set_pose(SkeletonInstance *si, const Xform *world) {
    const SkeletonAsset *a = si->asset;
    if (world != si->world) memcpy(si->world, world, sizeof(Xform) * (size_t)si->count);
    for (int i = 0; i < si->count; i++) si->skin[i] = mat4_mul(xf_to_mat4(si->world[i]), a->inv_bind[i]);
    si->pose_version++;
    si->palette_builds++;
}

bool skel_inst_socket_world(const SkeletonInstance *si, const char *name, Xform *out) {
    const SkelSocket *k = skel_asset_socket(si->asset, name);
    if (!k || k->bone < 0) return false;
    *out = skel_socket_world(k, si->world);
    return true;
}

bool skel_inst_vbone_world(const SkeletonInstance *si, const char *name, Xform *out) {
    int v = skel_asset_find_vbone(si->asset, name);
    if (v < 0 || si->asset->vbones[v].source < 0 || si->asset->vbones[v].target < 0) return false;
    *out = skel_vbone_xform(&si->asset->vbones[v], si->world);
    return true;
}
