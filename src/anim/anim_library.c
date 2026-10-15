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
                                                      "characters" };
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

static void apply_clip_meta(AnimLibrary *lib) {
    char rel[300];
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
            if (ci < 0) { log_warn("clips.json: unknown clip '%s'", e->string); continue; }
            Clip *c = &lib->clips[ci];
            const cJSON *v;
            if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(e, "loop"))) c->loop = cJSON_IsTrue(v);
            if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(e, "root_motion"))) c->root_motion = cJSON_IsTrue(v);
            if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(e, "additive"))) c->additive = cJSON_IsTrue(v);
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "speed"))) c->speed = (float)v->valuedouble;
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "blend_in"))) c->blend_in = (float)v->valuedouble;
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "blend_out"))) c->blend_out = (float)v->valuedouble;
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

static int load_mesh(AnimLibrary *lib, const char *rel_path) {
    for (int i = 0; i < lib->mesh_count; i++) if (!strcmp(lib->mesh_paths[i], rel_path)) return i;
    if (lib->mesh_count >= LIB_MAX_MESHES) return -1;
    char buf[512], err[512] = { 0 };
    const char *full = platform_data_path(rel_path, buf, sizeof(buf));
    // Rig validation first: the model's own skeleton against the canonical one.
    Skeleton *ms = (Skeleton *)malloc(sizeof(Skeleton));
    if (!anim_cache_skeleton(full, ms, err, sizeof(err))) { log_error("character mesh %s: %s", rel_path, err); free(ms); return -1; }
    RigReport *rep = (RigReport *)malloc(sizeof(RigReport));
    bool ok = rig_validate(&lib->skel, ms, 0.05f, rep);
    if (!ok || !rep->identical) log_info("rig %s:\n%s", rel_path, rep->text);
    free(rep);
    free(ms);
    if (!ok) { log_error("character mesh %s: rig rejected (see report above)", rel_path); return -1; }
    int i = lib->mesh_count;
    if (!anim_cache_mesh(full, &lib->skel, &lib->meshes[i], err, sizeof(err))) { log_error("character mesh %s: %s", rel_path, err); return -1; }
    dg_strcpy(lib->mesh_paths[i], 128, rel_path);
    lib->mesh_count++;
    return i;
}

bool anim_lib_reload_characters(AnimLibrary *lib) {
    int n = 0;
    char **files = list_files("assets/characters", "*.json", &n);
    lib->char_count = 0;
    u32 h = ANIM_HASH_SEED;
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
        d->mesh = load_mesh(lib, d->mesh_path);
        d->anim_set_index = anim_lib_find_set(lib, d->anim_set);
        if (d->anim_set_index < 0) { log_warn("character %s: anim set '%s' not found, using '%s'", d->id, d->anim_set, lib->sets[0].name); d->anim_set_index = 0; }
        d->valid = d->mesh >= 0;
        character_resolve_sockets(d, &lib->skel);
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
    if (!anim_cache_dir()[0]) anim_cache_set_dir(platform_data_path("assets/cache/", buf, sizeof(buf)));
    double t0 = platform_time();
    char rig[300], err[512] = { 0 };
    if (!find_canonical(lib, rig, sizeof(rig))) { log_error("anim: canonical rig assets/human/dummy.fbx not found"); return false; }
    if (!anim_cache_skeleton(platform_data_path(rig, buf, sizeof(buf)), &lib->skel, err, sizeof(err))) { log_error("anim: %s", err); return false; }
    lib->manifest.hash[AC_SKELETON] = lib->skel.hash;
    load_clips(lib);
    apply_clip_meta(lib);
    load_controller(lib);
    proc_hand_frames(&lib->skel, &lib->hands);
    load_sets(lib);
    // Canonical mesh first: its fitted radii shape the ragdoll and the hitboxes.
    int canon_mesh = load_mesh(lib, rig);
    if (canon_mesh >= 0) skin_fit_bone_radii(&lib->meshes[canon_mesh], 0, &lib->skel, lib->bone_radius);
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

void anim_lib_free(AnimLibrary *lib) {
    for (int i = 0; i < lib->clip_count; i++) clip_free(&lib->clips[i]);
    for (int i = 0; i < lib->mesh_count; i++) skin_mesh_free(&lib->meshes[i]);
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
