#include "anim_library.h"
#include "assets/fbx_loader.h"
#include "assets/anim_cache.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static const char *k_cat_names[AC_CATEGORY_COUNT] = { "skeleton", "clips", "controller", "hold_styles", "anim_sets", "hitboxes", "ragdoll",
                                                      "characters", "hand_poses" };
const char *asset_category_name(int c) { return (c >= 0 && c < AC_CATEGORY_COUNT) ? k_cat_names[c] : "?"; }

u32 anim_manifest_combined(const AnimManifest *m) { return anim_hash(m->hash, sizeof(m->hash), ANIM_HASH_SEED); }

static char *read_text(const char *rel) {
    char buf[512];
    return platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
}

static int cmp_names(const void *a, const void *b) { return SDL_strcasecmp(*(const char *const *)a, *(const char *const *)b); }

// Sorted file list (relative names) of dir matching pattern. Caller SDL_free()s the array.
static char **list_files(const char *rel_dir, const char *pattern, int *count) {
    char buf[512];
    const char *dir = platform_data_path(rel_dir, buf, sizeof(buf));
    char **files = SDL_GlobDirectory(dir, pattern, SDL_GLOB_CASEINSENSITIVE, count);
    if (files && *count > 1) qsort(files, (size_t)*count, sizeof(char *), cmp_names);
    return files;
}

int anim_lib_find_set(const AnimLibrary *lib, const char *name) {
    for (int i = 0; i < lib->set_count; i++) if (!strcmp(lib->sets[i].name, name)) return i;
    return -1;
}

int anim_lib_find_clip(const AnimLibrary *lib, const char *name) {
    for (int i = 0; i < lib->clip_count; i++) if (!strcmp(lib->clips[i].name, name)) return i;
    return -1;
}

const CharacterDef *anim_lib_char_by_net(const AnimLibrary *lib, u8 net_id) {
    for (int i = 0; i < lib->char_count; i++) if (lib->chars[i].net_id == net_id && lib->chars[i].valid) return &lib->chars[i];
    for (int i = 0; i < lib->char_count; i++) if (lib->chars[i].valid) return &lib->chars[i];
    return NULL;
}

const CharacterDef *anim_lib_char_by_id(const AnimLibrary *lib, const char *id) {
    for (int i = 0; i < lib->char_count; i++) if (!strcmp(lib->chars[i].id, id)) return &lib->chars[i];
    return NULL;
}

// ------------------------------------------------------------------ pieces

// Turns a clip's lower body about +Y by deg (positive = to the left): the root bones turn about the
// vertical through the hips, the spine is counter-turned so everything above it (chest, arms, head:
// the first-person eye) keeps its authored model-space pose. A bladed stance: the legs / pelvis turn,
// the upper body still faces the aim. Travel (root_t) is unchanged.
static void clip_turn(const Skeleton *s, Clip *c, float deg) {
    quat q = quat_axis_angle(v3(0, 1, 0), deg * DG_DEG2RAD);
    int hips = s->role[BR_HIPS], spine = s->role[BR_SPINE];
    if (hips < 0 || c->bones != s->count) return;
    for (int f = 0; f < c->frames; f++) {
        Xform *loc = &c->data[(size_t)f * (size_t)c->bones];
        Pose p;
        Xform m[ANIM_MAX_BONES], m2[ANIM_MAX_BONES];
        p.count = c->bones;
        memcpy(p.local, loc, sizeof(Xform) * (size_t)c->bones);
        pose_to_model(s, &p, m);
        vec3 pivot = v3(m[hips].t.x, 0, m[hips].t.z);
        for (int b = 0; b < c->bones; b++) {
            if (s->parent[b] >= 0) continue;
            loc[b].r = quat_norm(quat_mul(q, loc[b].r));
            loc[b].t = v3_add(pivot, quat_rotate(q, v3_sub(loc[b].t, pivot)));
        }
        if (spine >= 0 && s->parent[spine] >= 0) {
            memcpy(p.local, loc, sizeof(Xform) * (size_t)c->bones);
            pose_to_model(s, &p, m2);
            loc[spine] = xf_mul(xf_inverse(m2[s->parent[spine]]), m[spine]);
            loc[spine].r = quat_norm(loc[spine].r);
        }
    }
}

// A jump played with the movement's own rise: every frame's hips are capped at their highest
// height with a toe down (within 3 cm of its lowest; a running jump never has both down), so the
// clip keeps the take-off crouch, the tuck and the landing squat but the body rises only with the
// character.
static void clip_air_in_place(const Skeleton *s, Clip *c) {
    int hips = s->role[BR_HIPS], toe[2] = { s->role[BR_L_TOE], s->role[BR_R_TOE] };
    if (hips < 0 || s->parent[hips] >= 0 || toe[0] < 0 || toe[1] < 0 || c->bones != s->count || c->frames < 2) return;
    float *ty = (float *)malloc(sizeof(float) * 2 * (size_t)c->frames);
    float lo[2] = { 1e9f, 1e9f };
    Pose p;
    Xform m[ANIM_MAX_BONES];
    p.count = c->bones;
    for (int f = 0; f < c->frames; f++) {
        memcpy(p.local, &c->data[(size_t)f * (size_t)c->bones], sizeof(Xform) * (size_t)c->bones);
        pose_to_model(s, &p, m);
        for (int k = 0; k < 2; k++) {
            ty[f * 2 + k] = m[toe[k]].t.y;
            lo[k] = fminf(lo[k], m[toe[k]].t.y);
        }
    }
    float stand = -1e9f;
    for (int f = 0; f < c->frames; f++)
        if (ty[f * 2] <= lo[0] + 0.03f || ty[f * 2 + 1] <= lo[1] + 0.03f) stand = fmaxf(stand, c->data[(size_t)f * (size_t)c->bones + (size_t)hips].t.y);
    free(ty);
    if (stand <= -1e8f) return;
    for (int f = 0; f < c->frames; f++) {
        Xform *x = &c->data[(size_t)f * (size_t)c->bones + (size_t)hips];
        x->t.y = fminf(x->t.y, stand);
    }
}

static void apply_clip_meta(AnimLibrary *lib) {
    char rel[300];
    float yaw[LIB_MAX_CLIPS] = { 0 };
    bool air[LIB_MAX_CLIPS] = { 0 };
    snprintf(rel, sizeof(rel), "%s/clips.json", lib->human_dir);
    char *text = read_text(rel);
    u32 h = ANIM_HASH_SEED;
    for (int i = 0; i < lib->clip_count; i++) h = anim_hash(&lib->clips[i].hash, sizeof(u32), h);
    if (text) {
        h = anim_hash_str(text, h);
        cJSON *root = cJSON_Parse(text);
        const cJSON *clips = root ? cJSON_GetObjectItemCaseSensitive(root, "clips") : NULL;
        const cJSON *e;
        cJSON_ArrayForEach(e, clips) {
            int ci = anim_lib_find_clip(lib, e->string);
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "name");
            if (ci < 0 && cJSON_IsString(v)) ci = anim_lib_find_clip(lib, v->valuestring);   // already renamed (reload)
            if (ci < 0) { log_warn("clips.json: unknown clip '%s'", e->string); continue; }
            Clip *c = &lib->clips[ci];
            // "name": short alias used by anim sets ("Animations:Armature|Jog Forward" -> "jog_forward").
            if (cJSON_IsString(v)) dg_strcpy(c->name, sizeof(c->name), v->valuestring);
            // "root_speed": travel speed of an in-place clip (m/s), for speed matching / foot sliding.
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "root_speed"))) c->avg_speed = (float)v->valuedouble;
            if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(e, "loop"))) c->loop = cJSON_IsTrue(v);
            if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(e, "root_motion"))) c->root_motion = cJSON_IsTrue(v);
            if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(e, "additive"))) c->additive = cJSON_IsTrue(v);
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "speed"))) c->speed = (float)v->valuedouble;
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "blend_in"))) c->blend_in = (float)v->valuedouble;
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "blend_out"))) c->blend_out = (float)v->valuedouble;
            // "yaw": the clip's body turned about the vertical (degrees, negative = to the right), for
            // clips authored facing off the forward axis.
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "yaw"))) yaw[ci] = (float)v->valuedouble;
            // "air_in_place": a jump whose rise comes from the movement (clip_air_in_place).
            air[ci] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "air_in_place"));
            if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "reference"))) c->additive_ref = anim_lib_find_clip(lib, v->valuestring);
            v = cJSON_GetObjectItemCaseSensitive(e, "markers");
            if (cJSON_IsArray(v) && cJSON_GetArraySize(v) >= 2) {
                c->plant[0] = (float)cJSON_GetArrayItem(v, 0)->valuedouble;
                c->plant[1] = (float)cJSON_GetArrayItem(v, 1)->valuedouble;
                c->has_markers = true;
            }
            const cJSON *ev;
            c->event_count = 0;
            cJSON_ArrayForEach(ev, cJSON_GetObjectItemCaseSensitive(e, "events")) {
                if (c->event_count >= ANIM_MAX_CLIP_EVENTS) break;
                ClipEvent *ce = &c->events[c->event_count++];
                const cJSON *t = cJSON_GetObjectItemCaseSensitive(ev, "time");
                const cJSON *ty = cJSON_GetObjectItemCaseSensitive(ev, "type");
                ce->time = cJSON_IsNumber(t) ? (float)t->valuedouble : 0;
                const char *tn = cJSON_IsString(ty) ? ty->valuestring : "custom";
                ce->type = !strcmp(tn, "footstep_l") ? CEV_FOOTSTEP_L : !strcmp(tn, "footstep_r") ? CEV_FOOTSTEP_R
                         : !strcmp(tn, "weapon_fire") ? CEV_WEAPON_FIRE : CEV_CUSTOM;
                dg_strcpy(ce->name, sizeof(ce->name), tn);
            }
        }
        cJSON_Delete(root);
        free(text);
    }
    // Only the change since the last apply (meta reloads keep the baked data).
    for (int i = 0; i < lib->clip_count; i++) {
        if (fabsf(yaw[i] - lib->clip_yaw[i]) > 1e-4f) clip_turn(&lib->skel, &lib->clips[i], yaw[i] - lib->clip_yaw[i]);
        lib->clip_yaw[i] = yaw[i];
        // Capping is not undone by a meta reload (anim_rebake / a restart restores the clip).
        if (air[i] && !lib->clip_air[i]) clip_air_in_place(&lib->skel, &lib->clips[i]);
        lib->clip_air[i] |= air[i];
    }
    lib->manifest.hash[AC_CLIPS] = h;
}

static bool load_controller(AnimLibrary *lib) {
    char rel[300];
    snprintf(rel, sizeof(rel), "%s/controller.json", lib->human_dir);
    char *text = read_text(rel);
    if (!text) { log_error("anim: cannot read %s", rel); return false; }
    char err[256] = { 0 };
    bool ok = ctrl_def_load(&lib->ctrl, text, &lib->skel, err, sizeof(err));
    if (!ok) log_error("anim: %s", err);
    dg_strcpy(lib->ctrl.source, sizeof(lib->ctrl.source), rel);
    cJSON *root = cJSON_Parse(text);
    proc_params_load(&lib->proc, root);
    cJSON_Delete(root);
    // Hold points (camera-space item placement per style / state).
    snprintf(rel, sizeof(rel), "%s/holdpoint.json", lib->human_dir);
    char *hp_text = read_text(rel);
    cJSON *hp_root = hp_text ? cJSON_Parse(hp_text) : NULL;
    if (hp_text && !hp_root) log_error("anim: %s: JSON parse error", rel);
    proc_holdpoint_load(&lib->proc, hp_root);
    cJSON_Delete(hp_root);
    free(hp_text);
    lib->manifest.hash[AC_CONTROLLER] = lib->ctrl.hash;
    lib->manifest.hash[AC_HOLD_STYLES] = lib->proc.hash;
    free(text);
    return ok;
}

static void load_sets(AnimLibrary *lib) {
    char rel[300];
    snprintf(rel, sizeof(rel), "%s/anim_sets", lib->human_dir);
    int n = 0;
    char **files = list_files(rel, "*.json", &n);
    lib->set_count = 0;
    u32 h = ANIM_HASH_SEED;
    for (int i = 0; i < n && lib->set_count < LIB_MAX_SETS; i++) {
        if (strstr(files[i], "_template")) continue;
        char path[400];
        snprintf(path, sizeof(path), "%s/%s", rel, files[i]);
        char *text = read_text(path);
        if (!text) continue;
        char err[256] = { 0 };
        AnimBinding *b = &lib->sets[lib->set_count];
        if (ctrl_bind(b, &lib->ctrl, text, &lib->lookup, err, sizeof(err))) {
            if (!b->name[0] || !strcmp(b->name, "set")) fbx_file_stem(files[i], b->name, sizeof(b->name));
            h = anim_hash(&b->hash, sizeof(u32), h);
            lib->set_count++;
        } else {
            log_error("anim set %s: %s", path, err);
        }
        free(text);
    }
    SDL_free(files);
    if (!lib->set_count) {
        // Always have one binding (everything falls back to the first clip).
        ctrl_bind(&lib->sets[0], &lib->ctrl, "{\"name\":\"default\",\"slots\":{}}", &lib->lookup, NULL, 0);
        lib->set_count = 1;
    }
    lib->manifest.hash[AC_ANIM_SETS] = h;
}

static void load_physics_defs(AnimLibrary *lib) {
    char rel[300], err[256] = { 0 };
    snprintf(rel, sizeof(rel), "%s/ragdoll.json", lib->human_dir);
    char *text = read_text(rel);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    if (!ragdoll_def_build(&lib->ragdoll, &lib->skel, lib->bone_radius, root, err, sizeof(err))) log_error("anim: %s", err);
    cJSON_Delete(root);
    free(text);
    lib->manifest.hash[AC_RAGDOLL] = lib->ragdoll.hash;
    anim_lib_reload_hitboxes(lib);
}

bool anim_lib_reload_hitboxes(AnimLibrary *lib) {
    char rel[300], err[256] = { 0 };
    snprintf(rel, sizeof(rel), "%s/hitboxes.json", lib->human_dir);
    char *text = read_text(rel);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    bool ok = hitbox_table_load(&lib->hitboxes, &lib->ragdoll, root, err, sizeof(err));
    if (!ok) log_error("anim: %s", err);
    cJSON_Delete(root);
    free(text);
    lib->manifest.hash[AC_HITBOXES] = lib->hitboxes.hash;
    return ok;
}

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && SDL_strcasecmp(s + a - b, suffix) == 0;
}

// Same-name .json next to a mesh (drag-and-drop metadata: aliases, sockets, skin name / atlas).
static cJSON *read_sidecar(const char *rel_path) {
    char side[300];
    dg_strcpy(side, sizeof(side), rel_path);
    char *dot = strrchr(side, '.');
    if (!dot) return NULL;
    dg_strcpy(dot, sizeof(side) - (size_t)(dot - side), ".json");
    char *text = read_text(side);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    if (text && !root) log_error("%s: JSON parse error", side);
    free(text);
    return root;
}

static void parse_mesh_sockets(SkeletalMesh *m, const cJSON *obj) {
    const cJSON *e;
    cJSON_ArrayForEach(e, obj) {
        if (!cJSON_IsObject(e) || m->socket_count >= SKM_MAX_SOCKETS) continue;
        SkelSocket *k = &m->sockets[m->socket_count++];
        memset(k, 0, sizeof(*k));
        dg_strcpy(k->name, sizeof(k->name), e->string);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "bone");
        dg_strcpy(k->bone_name, sizeof(k->bone_name), cJSON_IsString(v) ? v->valuestring : "Spine2");
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(e, "offset"), *r = cJSON_GetObjectItemCaseSensitive(e, "rotation");
        if (cJSON_GetArraySize(o) >= 3) k->offset = v3((float)cJSON_GetArrayItem(o, 0)->valuedouble, (float)cJSON_GetArrayItem(o, 1)->valuedouble, (float)cJSON_GetArrayItem(o, 2)->valuedouble);
        if (cJSON_GetArraySize(r) >= 3) k->rotation_deg = v3((float)cJSON_GetArrayItem(r, 0)->valuedouble, (float)cJSON_GetArrayItem(r, 1)->valuedouble, (float)cJSON_GetArrayItem(r, 2)->valuedouble);
        v = cJSON_GetObjectItemCaseSensitive(e, "space");
        k->bind_space = !(cJSON_IsString(v) && !strcmp(v->valuestring, "bone"));
        k->bone = -1;
    }
}

// A plain mesh file bound to the skeleton: .skm as baked, .fbx through rig validation + the cache.
static bool load_mesh_file(AnimLibrary *lib, const char *rel_path, SkeletalMesh *out) {
    char buf[512], err[512] = { 0 };
    const char *full = platform_data_path(rel_path, buf, sizeof(buf));
    cJSON *side = read_sidecar(rel_path);
    BoneAliasTable *aliases = (BoneAliasTable *)calloc(1, sizeof(BoneAliasTable));
    if (side) skm_alias_parse(aliases, cJSON_GetObjectItemCaseSensitive(side, "aliases"));
    bool ok = false;
    if (ends_with_ci(rel_path, ".skm")) {
        ok = skm_read(out, full, err, sizeof(err));
        if (ok && out->skeleton_hash != lib->skel.hash) {
            snprintf(err, sizeof(err), "baked against skeleton %08x, this skeleton is %08x: re-run tools/bake_mesh", out->skeleton_hash, lib->skel.hash);
            skm_free(out);
            ok = false;
        }
    } else {
        // Rig validation first: the model's own skeleton (aliases applied) against the Skeleton asset.
        Skeleton *ms = (Skeleton *)malloc(sizeof(Skeleton));
        RigReport *rep = (RigReport *)malloc(sizeof(RigReport));
        if (anim_cache_skeleton(full, ms, err, sizeof(err))) {
            skm_alias_apply_skeleton(aliases, ms);
            ok = rig_validate(&lib->skel, ms, 0.05f, rep);
            if (!ok || !rep->identical) log_info("rig %s:\n%s", rel_path, rep->text);
            if (!ok) snprintf(err, sizeof(err), "rig rejected (see report above; a same-name .json sidecar with \"aliases\" fixes renamed bones)");
        }
        free(rep);
        free(ms);
        if (ok) ok = anim_cache_skm(full, &lib->skeleton, aliases, out, err, sizeof(err));
    }
    if (ok && side) {
        parse_mesh_sockets(out, cJSON_GetObjectItemCaseSensitive(side, "sockets"));
        skm_finalize(out, &lib->skeleton);
    }
    if (!ok) log_error("character mesh %s: %s", rel_path, err);
    cJSON_Delete(side);
    free(aliases);
    return ok;
}

int anim_lib_load_mesh_ref(AnimLibrary *lib, const MeshRef *ref) {
    u32 h = mesh_ref_hash(ref);
    for (int i = 0; i < lib->mesh_count; i++)
        if (mesh_ref_hash(&lib->mesh_refs[i]) == h && !strcmp(lib->mesh_refs[i].path, ref->path) && !strcmp(lib->mesh_refs[i].region, ref->region)) return i;
    if (!mesh_ref_is_plain(ref)) {
        // Derived part: cut out of / deformed from the plain mesh (placeholder modular sets).
        MeshRef base;
        memset(&base, 0, sizeof(base));
        dg_strcpy(base.path, sizeof(base.path), ref->path);
        base.scale = v3(1, 1, 1);
        int b = anim_lib_load_mesh_ref(lib, &base);
        if (b < 0 || lib->mesh_count >= LIB_MAX_MESHES) return -1;
        SkeletalMesh *dst = &lib->meshes[lib->mesh_count];
        if (ref->region[0]) {
            int g = skel_asset_find_group(&lib->skeleton, ref->region);
            if (g < 0) { log_error("character mesh %s: unknown region (bone group) '%s'", ref->path, ref->region); return -1; }
            if (!skm_extract_region(&lib->meshes[b], &lib->skeleton.groups[g].bones, dst)) {
                log_error("character mesh %s: region '%s' has no triangles", ref->path, ref->region);
                return -1;
            }
        } else {
            skm_copy(dst, &lib->meshes[b]);
        }
        if (ref->inflate != 0.0f || ref->scale.x != 1.0f || ref->scale.y != 1.0f || ref->scale.z != 1.0f) skm_deform(dst, ref->inflate, ref->scale);
        skm_finalize(dst, &lib->skeleton);
        lib->mesh_refs[lib->mesh_count] = *ref;
        return lib->mesh_count++;
    }
    if (lib->mesh_count >= LIB_MAX_MESHES) { log_error("character mesh %s: more than %d meshes", ref->path, LIB_MAX_MESHES); return -1; }
    int i = lib->mesh_count;
    if (!load_mesh_file(lib, ref->path, &lib->meshes[i])) return -1;
    lib->mesh_refs[i] = *ref;
    lib->mesh_count++;
    return i;
}

int anim_lib_load_mesh(AnimLibrary *lib, const char *rel_path) {
    MeshRef r;
    memset(&r, 0, sizeof(r));
    dg_strcpy(r.path, sizeof(r.path), rel_path);
    r.scale = v3(1, 1, 1);
    return anim_lib_load_mesh_ref(lib, &r);
}

int anim_lib_find_skin(const AnimLibrary *lib, const char *id) {
    for (int k = 0; k < lib->skin_count; k++) if (!strcmp(lib->skins[k].id, id)) return k;
    return -1;
}

static void skin_meta(CharSkinDef *sk, const cJSON *e) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "name");
    if (cJSON_IsString(v)) dg_strcpy(sk->name, sizeof(sk->name), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(e, "atlas");
    if (cJSON_IsString(v)) dg_strcpy(sk->atlas, sizeof(sk->atlas), v->valuestring);
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(e, "tiles")) {
        const cJSON *mat = cJSON_GetObjectItemCaseSensitive(t, "material");
        if (sk->tile_count < SKIN_MAX_MATERIALS) dg_strcpy(sk->tile_material[sk->tile_count++], 48, cJSON_IsString(mat) ? mat->valuestring : "");
    }
}

// Skins: skins.json entries, then every mesh dropped into assets/skins/ (sorted, so every machine
// numbers them alike). Returns the content hash (part of the characters manifest).
static u32 load_skins(AnimLibrary *lib, char npc_def[][32], int *npc_skin, int *npc_count) {
    lib->skin_count = 0;
    lib->player_skin = -1;
    *npc_count = 0;
    u32 h = ANIM_HASH_SEED;
    char rel[300];
    snprintf(rel, sizeof(rel), "%s/skins.json", lib->human_dir);
    char *text = read_text(rel);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    if (text) h = anim_hash_str(text, h);
    free(text);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "skins")) {
        if (lib->skin_count >= LIB_MAX_SKINS) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
        CharSkinDef *sk = &lib->skins[lib->skin_count];
        memset(sk, 0, sizeof(*sk));
        if (!cJSON_IsString(id) || !mesh_ref_parse(&sk->ref, cJSON_GetObjectItemCaseSensitive(e, "mesh"))) continue;
        dg_strcpy(sk->id, sizeof(sk->id), id->valuestring);
        dg_strcpy(sk->name, sizeof(sk->name), id->valuestring);
        skin_meta(sk, e);
        sk->mesh = anim_lib_load_mesh_ref(lib, &sk->ref);
        if (sk->mesh < 0) { log_warn("skin %s: mesh %s unavailable", sk->id, sk->ref.path); continue; }
        lib->skin_count++;
    }
    // Drag and drop: assets/skins/<Name>.fbx (+ optional <Name>.json) is a skin with no other edit.
    static const char *const pats[] = { "*.fbx", "*.skm" };
    for (int p = 0; p < 2; p++) {
        int n = 0;
        char **files = list_files("assets/skins", pats[p], &n);
        for (int i = 0; i < n && lib->skin_count < LIB_MAX_SKINS; i++) {
            CharSkinDef *sk = &lib->skins[lib->skin_count];
            memset(sk, 0, sizeof(*sk));
            snprintf(sk->ref.path, sizeof(sk->ref.path), "assets/skins/%s", files[i]);
            sk->ref.scale = v3(1, 1, 1);
            fbx_file_stem(files[i], sk->name, sizeof(sk->name));
            for (int c = 0; sk->name[c] && c < (int)sizeof(sk->id) - 1; c++) {
                char ch = sk->name[c];
                sk->id[c] = (char)((ch == ' ' || ch == '-') ? '_' : (ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
            }
            if (anim_lib_find_skin(lib, sk->id) >= 0) continue;   // listed in skins.json already
            cJSON *side = read_sidecar(sk->ref.path);
            if (side) skin_meta(sk, side);
            cJSON_Delete(side);
            h = anim_hash_str(sk->ref.path, h);
            sk->mesh = anim_lib_load_mesh_ref(lib, &sk->ref);
            if (sk->mesh < 0) { log_warn("skin %s: %s unusable (see above)", sk->id, sk->ref.path); continue; }
            log_info("skin %s: %s (dropped in assets/skins)", sk->id, sk->ref.path);
            lib->skin_count++;
        }
        SDL_free(files);
    }
    const cJSON *pl = cJSON_GetObjectItemCaseSensitive(root, "player");
    if (cJSON_IsString(pl)) lib->player_skin = anim_lib_find_skin(lib, pl->valuestring);
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "npc")) {
        if (*npc_count >= LIB_MAX_CHARS || !cJSON_IsString(e)) continue;
        dg_strcpy(npc_def[*npc_count], 32, e->string);
        npc_skin[*npc_count] = anim_lib_find_skin(lib, e->valuestring);
        (*npc_count)++;
    }
    cJSON_Delete(root);
    for (int k = 0; k < lib->skin_count; k++) h = anim_hash_str(lib->skins[k].id, h);
    return h;
}

static CharPartOption skin_option(const AnimLibrary *lib, int k) {
    CharPartOption o;
    memset(&o, 0, sizeof(o));
    const CharSkinDef *sk = &lib->skins[k];
    dg_strcpy(o.id, sizeof(o.id), sk->id);
    o.ref = sk->ref;
    o.mesh = sk->mesh;
    o.skin = k;
    o.weight = 0;   // only by choice (cl_skin / skins.json defaults)
    return o;
}

bool anim_lib_reload_characters(AnimLibrary *lib) {
    char npc_def[LIB_MAX_CHARS][32];
    int npc_skin[LIB_MAX_CHARS], npc_count = 0;
    u32 h = load_skins(lib, npc_def, npc_skin, &npc_count);
    int n = 0;
    char **files = list_files("assets/characters", "*.json", &n);
    lib->char_count = 0;
    for (int i = 0; i < n && lib->char_count < LIB_MAX_CHARS; i++) {
        if (strstr(files[i], "_template")) continue;
        char path[400];
        snprintf(path, sizeof(path), "assets/characters/%s", files[i]);
        char *text = read_text(path);
        if (!text) continue;
        CharacterDef *d = &lib->chars[lib->char_count];
        char err[256] = { 0 };
        if (!character_def_parse(d, text, path, err, sizeof(err))) { log_error("%s", err); free(text); continue; }
        free(text);
        if (!d->net_id) d->net_id = (u8)(lib->char_count + 1);
        // Parts: every option's SkeletalMesh (deduplicated across characters).
        d->mesh = -1;
        for (int sl = 0; sl < CSLOT_COUNT; sl++)
            for (int o = 0; o < d->slots[sl].option_count; o++) {
                CharPartOption *po = &d->slots[sl].options[o];
                po->mesh = anim_lib_load_mesh_ref(lib, &po->ref);
                if (d->mesh < 0) d->mesh = po->mesh;
            }
        // Skins: players can wear any of them (body slot); skins.json "npc" sets a character's default.
        for (int k = 0; k < lib->skin_count; k++) {
            bool npc_default = false;
            for (int q = 0; q < npc_count; q++) if (npc_skin[q] == k && !strcmp(npc_def[q], d->id)) npc_default = true;
            if (d->kind != CK_PLAYER && !npc_default) continue;
            CharPartOption so = skin_option(lib, k);
            so.is_default = npc_default;
            if (!character_add_option(d, CSLOT_BODY, &so)) log_warn("character %s: too many body options, skin %s skipped", d->id, so.id);
        }
        d->anim_set_index = anim_lib_find_set(lib, d->anim_set);
        if (d->anim_set_index < 0) { log_warn("character %s: anim set '%s' not found, using '%s'", d->id, d->anim_set, lib->sets[0].name); d->anim_set_index = 0; }
        d->valid = d->mesh >= 0;
        character_resolve_sockets(d, &lib->skeleton);
        character_resolve_parts(d, &lib->skeleton);
        h = anim_hash(&d->hash, sizeof(u32), h);
        lib->char_count++;
    }
    SDL_free(files);
    for (int a = 0; a < lib->char_count; a++)
        for (int b = a + 1; b < lib->char_count; b++)
            if (lib->chars[a].net_id == lib->chars[b].net_id)
                log_error("characters '%s' and '%s' share character_id %d", lib->chars[a].id, lib->chars[b].id, lib->chars[a].net_id);
    lib->manifest.hash[AC_CHARACTERS] = h;
    log_info("anim: %d character definitions, %d meshes", lib->char_count, lib->mesh_count);
    return lib->char_count > 0;
}

static bool load_clips(AnimLibrary *lib) {
    for (int i = 0; i < lib->clip_count; i++) clip_free(&lib->clips[i]);
    lib->clip_count = 0;
    memset(lib->clip_yaw, 0, sizeof(lib->clip_yaw));
    memset(lib->clip_air, 0, sizeof(lib->clip_air));
    int n = 0;
    char **files = list_files(lib->human_dir, "*.fbx", &n);
    for (int i = 0; i < n && lib->clip_count < LIB_MAX_CLIPS; i++) {
        char rel[400], buf[512], err[512] = { 0 };
        snprintf(rel, sizeof(rel), "%s/%s", lib->human_dir, files[i]);
        const char *full = platform_data_path(rel, buf, sizeof(buf));
        int got = anim_cache_clips(full, &lib->skel, lib->sample_rate, &lib->clips[lib->clip_count], LIB_MAX_CLIPS - lib->clip_count, err, sizeof(err));
        if (got > 0) lib->clip_count += got;
    }
    SDL_free(files);
    lib->lookup.clips = lib->clips;
    lib->lookup.count = lib->clip_count;
    return lib->clip_count > 0;
}

static bool find_canonical(AnimLibrary *lib, char *out, size_t n) {
    int count = 0;
    char **files = list_files(lib->human_dir, "dummy.fbx", &count);
    bool ok = files && count > 0;
    if (ok) snprintf(out, n, "%s/%s", lib->human_dir, files[0]);
    SDL_free(files);
    return ok;
}

bool anim_lib_load(AnimLibrary *lib, float rate) {
    memset(lib, 0, sizeof(*lib));
    lib->sample_rate = rate > 0 ? rate : ANIM_DEFAULT_RATE;
    dg_strcpy(lib->human_dir, sizeof(lib->human_dir), "assets/human");
    char buf[512];
    if (!anim_cache_dir()[0]) anim_cache_set_dir(platform_writable_dir("assets/cache/", buf, sizeof(buf)));
    double t0 = platform_time();
    char rig[300], err[512] = { 0 };
    if (!find_canonical(lib, rig, sizeof(rig))) dg_strcpy(rig, sizeof(rig), "assets/human/Dummy.fbx");   // a baked .skel may still exist
    if (!skel_asset_load(&lib->skeleton, "human", rig, err, sizeof(err))) { log_error("anim: %s", err); return false; }
    dg_strcpy(rig, sizeof(rig), lib->skeleton.source);
    lib->manifest.hash[AC_SKELETON] = skel_asset_hash(&lib->skeleton);
    load_clips(lib);
    apply_clip_meta(lib);
    load_controller(lib);
    proc_hand_frames(&lib->skel, &lib->hands);
    anim_lib_reload_handposes(lib);
    load_sets(lib);
    // Canonical mesh first: its fitted radii shape the ragdoll and the hitboxes.
    int canon_mesh = anim_lib_load_mesh(lib, rig);
    if (canon_mesh >= 0) {
        i16 jmap[SKM_MAX_BONES];
        skm_joint_map(&lib->meshes[canon_mesh], jmap);
        skin_fit_bone_radii(&lib->meshes[canon_mesh].data, 0, &lib->skel, jmap, lib->bone_radius);
    }
    load_physics_defs(lib);
    anim_lib_reload_characters(lib);
    if (!anim_cache_dir()[0] || 1) {
        char tp[300];
        snprintf(tp, sizeof(tp), "%s/anim_sets/player_template.json", lib->human_dir);
        if (!platform_file_exists(platform_data_path(tp, buf, sizeof(buf)))) anim_lib_write_template(lib, "player", false);
        snprintf(tp, sizeof(tp), "%s/anim_sets/zombie_template.json", lib->human_dir);
        if (!platform_file_exists(platform_data_path(tp, buf, sizeof(buf)))) anim_lib_write_template(lib, "zombie", true);
    }
    AnimCacheStats st = anim_cache_stats();
    log_info("anim: skeleton %d bones (%08x), %d clips, %d anim sets, %d ragdoll bodies, manifest %08x (%.0f ms, cache %d hits / %d baked)",
             lib->skel.count, lib->skel.hash, lib->clip_count, lib->set_count, lib->ragdoll.body_count, anim_manifest_combined(&lib->manifest),
             (platform_time() - t0) * 1000.0, st.hits, st.misses);
    lib->loaded = true;
    lib->revision++;
    return true;
}

bool anim_lib_reload_handposes(AnimLibrary *lib) {
    handpose_db_clear(&lib->handposes);
    int files = handpose_db_load_dir(&lib->handposes, HANDPOSE_DIR);
    lib->manifest.hash[AC_HAND_POSES] = lib->handposes.hash;
    log_info("anim: %d hand pose files, %d grips, %d cases (%08x)", files, lib->handposes.count, lib->handposes.case_count, lib->handposes.hash);
    return true;
}

void anim_lib_free(AnimLibrary *lib) {
    handpose_db_free(&lib->handposes);
    for (int i = 0; i < lib->clip_count; i++) clip_free(&lib->clips[i]);
    for (int i = 0; i < lib->mesh_count; i++) skm_free(&lib->meshes[i]);
    lib->clip_count = lib->mesh_count = 0;
    lib->loaded = false;
}

bool anim_lib_reload_meta(AnimLibrary *lib) {
    apply_clip_meta(lib);
    bool ok = load_controller(lib);
    load_sets(lib);
    for (int i = 0; i < lib->char_count; i++) {
        int s = anim_lib_find_set(lib, lib->chars[i].anim_set);
        lib->chars[i].anim_set_index = s >= 0 ? s : 0;
    }
    lib->revision++;
    return ok;
}

bool anim_lib_rebake(AnimLibrary *lib) {
    bool ok = load_clips(lib);
    apply_clip_meta(lib);
    load_sets(lib);
    lib->revision++;
    return ok;
}

bool anim_lib_write_template(const AnimLibrary *lib, const char *name, bool zombie) {
    cJSON *root = cJSON_CreateObject();
    char nm[64];
    snprintf(nm, sizeof(nm), "%s_template", name);
    cJSON_AddStringToObject(root, "name", nm);
    cJSON_AddStringToObject(root, "_comment",
                            "Every controller slot. Map each to a clip name (FBX file stem, or 'file:take'). Modifiers: '@mirror', "
                            "'@reverse', '@0.2:0.6' (normalized sub-range). Empty = missing: base-layer states fall back to idle, "
                            "upper-body states fall back to the procedural hold, blend-tree children are dropped.");
    cJSON *slots = cJSON_AddObjectToObject(root, "slots");
    for (int s = 0; s < lib->ctrl.slot_count; s++) cJSON_AddStringToObject(slots, lib->ctrl.slot_names[s], "");
    // Weapon-type pattern slots ("reload_{weapon}" -> reload_rifle, ...).
    for (int n = 0; n < lib->ctrl.node_count; n++) {
        const char *pat = lib->ctrl.nodes[n].slot_pattern;
        const char *br = pat[0] ? strstr(pat, "{weapon}") : NULL;
        if (!br) continue;
        for (int w = 0; w < lib->ctrl.weapon_type_count; w++) {
            char s[64];
            snprintf(s, sizeof(s), "%.*s%s%s", (int)(br - pat), pat, lib->ctrl.weapon_types[w], br + 8);
            if (!cJSON_GetObjectItemCaseSensitive(slots, s)) cJSON_AddStringToObject(slots, s, "");
        }
    }
    if (zombie) {
        static const char *extra[] = { "attack_1", "attack_2", "attack_3", "lunge", "crawl", "scream", "eat" };
        for (int i = 0; i < ARRAY_COUNT(extra); i++)
            if (!cJSON_GetObjectItemCaseSensitive(slots, extra[i])) cJSON_AddStringToObject(slots, extra[i], "");
    }
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    char rel[300], buf[512];
    snprintf(rel, sizeof(rel), "%s/anim_sets/%s_template.json", lib->human_dir, name);
    bool ok = platform_write_file(platform_data_path(rel, buf, sizeof(buf)), text, strlen(text));
    if (ok) log_info("anim: wrote %s", rel);
    free(text);
    return ok;
}
