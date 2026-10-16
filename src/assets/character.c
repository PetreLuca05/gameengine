#include "character.h"
#include "gameplay/hitboxes.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include "cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static const char *js(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

int character_part_group(const char *name, bool *out) {
    memset(out, 0, sizeof(bool) * BP_COUNT);
    int p = body_part_from_name(name);
    if (p >= 0) { out[p] = true; return 1; }
    int a = -1, b = -1;
    if (!strcmp(name, "arms")) { a = BP_UPPER_ARM_L; b = BP_LOWER_ARM_R; }
    else if (!strcmp(name, "hands")) { a = BP_HAND_L; b = BP_HAND_R; }
    else if (!strcmp(name, "legs")) { a = BP_UPPER_LEG_L; b = BP_LOWER_LEG_R; }
    else if (!strcmp(name, "feet")) { a = BP_FOOT_L; b = BP_FOOT_R; }
    else if (!strcmp(name, "limbs")) { a = BP_UPPER_ARM_L; b = BP_FOOT_R; }
    else if (!strcmp(name, "torso")) { a = BP_CHEST; b = BP_PELVIS; }
    else if (!strcmp(name, "all")) { a = 0; b = BP_COUNT - 1; }
    if (a < 0) return 0;
    for (int i = a; i <= b; i++) out[i] = true;
    return b - a + 1;
}

const char *char_surface_name(int s) {
    static const char *const n[CSURF_COUNT] = { "flesh", "cloth", "armor", "bone" };
    return s >= 0 && s < CSURF_COUNT ? n[s] : "flesh";
}

static void parse_blood(CharacterDef *d, const cJSON *root) {
    bool zombie = d->kind == CK_ZOMBIE;
    // Zombies: darker, thicker (less aerosol), clotted wounds bleed out slower.
    d->blood.color = zombie ? v3(0.20f, 0.035f, 0.02f) : v3(0.30f, 0.02f, 0.015f);
    d->blood.darkness = zombie ? 0.7f : 0.55f;
    d->blood.amount = zombie ? 1.15f : 1.0f;
    d->blood.mist = zombie ? 0.7f : 1.0f;
    d->blood.decay = zombie ? 0.7f : 1.0f;
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "blood");
    if (cJSON_IsObject(b)) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(b, "color");
        if (cJSON_IsArray(c) && cJSON_GetArraySize(c) >= 3)
            d->blood.color = v3((float)cJSON_GetArrayItem(c, 0)->valuedouble, (float)cJSON_GetArrayItem(c, 1)->valuedouble, (float)cJSON_GetArrayItem(c, 2)->valuedouble);
        d->blood.darkness = jf(b, "darkness", d->blood.darkness);
        d->blood.amount = jf(b, "amount", d->blood.amount);
        d->blood.mist = jf(b, "mist", d->blood.mist);
        d->blood.decay = jf(b, "decay", d->blood.decay);
    }
    // Surfaces: players wear clothes on the torso and limbs by default, zombies rags.
    for (int i = 0; i < BP_COUNT; i++) d->part_surface[i] = CSURF_FLESH;
    if (d->kind == CK_PLAYER)
        for (int i = BP_CHEST; i <= BP_FOOT_R; i++) d->part_surface[i] = (i == BP_HAND_L || i == BP_HAND_R) ? CSURF_FLESH : CSURF_CLOTH;
    const cJSON *e;
    bool m[BP_COUNT];
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "surface")) {
        if (!cJSON_IsString(e) || !character_part_group(e->string, m)) continue;
        int s = CSURF_FLESH;
        for (int k = 0; k < CSURF_COUNT; k++) if (!strcmp(e->valuestring, char_surface_name(k))) s = k;
        for (int i = 0; i < BP_COUNT; i++) if (m[i]) d->part_surface[i] = (u8)s;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "armor")) {
        if (!character_part_group(e->string, m)) continue;
        int hits = cJSON_IsObject(e) ? (int)jf(e, "hits", 2) : (cJSON_IsNumber(e) ? (int)e->valuedouble : 2);
        for (int i = 0; i < BP_COUNT; i++) if (m[i]) { d->armor_hits[i] = (u8)DG_CLAMP(hits, 0, 255); d->part_surface[i] = CSURF_ARMOR; }
    }
}

// ------------------------------------------------------------------ parts

static void fbx_file_stem_c(const char *path, char *out, size_t n) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    dg_strcpy(out, n, b);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static const char *const k_slot_names[CSLOT_COUNT] = { "body", "head", "torso", "legs", "hands", "hair", "gear_0", "gear_1", "gear_2", "gear_3" };

const char *char_slot_name(int slot) { return slot >= 0 && slot < CSLOT_COUNT ? k_slot_names[slot] : "?"; }

int char_slot_from_name(const char *name) {
    for (int i = 0; i < CSLOT_COUNT; i++) if (!strcmp(k_slot_names[i], name)) return i;
    return -1;
}

u32 mesh_ref_hash(const MeshRef *r) {
    u32 h = anim_hash_str(r->path, ANIM_HASH_SEED);
    h = anim_hash_str(r->region, h);
    h = anim_hash(&r->inflate, sizeof(float), h);
    return anim_hash(&r->scale, sizeof(vec3), h);
}

bool mesh_ref_is_plain(const MeshRef *r) {
    return !r->region[0] && r->inflate == 0.0f && r->scale.x == 1.0f && r->scale.y == 1.0f && r->scale.z == 1.0f;
}

static vec3 jvec3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}

bool mesh_ref_parse(MeshRef *r, const cJSON *e) {
    memset(r, 0, sizeof(*r));
    r->scale = v3(1, 1, 1);
    if (cJSON_IsString(e)) { dg_strcpy(r->path, sizeof(r->path), e->valuestring); return r->path[0] != 0; }
    if (!cJSON_IsObject(e)) return false;
    dg_strcpy(r->path, sizeof(r->path), js(e, "mesh", ""));
    dg_strcpy(r->region, sizeof(r->region), js(e, "region", ""));
    r->inflate = jf(e, "inflate", 0.0f);
    r->scale = jvec3(e, "scale", v3(1, 1, 1));
    return r->path[0] != 0;
}

void mesh_ref_describe(const MeshRef *r, char *out, size_t n) {
    if (mesh_ref_is_plain(r)) snprintf(out, n, "%s", r->path);
    else snprintf(out, n, "%s [%s%s inflate %.3f scale %.2f %.2f %.2f]", r->path, r->region[0] ? "region " : "", r->region, r->inflate, r->scale.x, r->scale.y, r->scale.z);
}

static void parse_materials(const cJSON *obj, int *count, char names[][48], vec4 *colors) {
    const cJSON *e;
    cJSON_ArrayForEach(e, obj) {
        if (*count >= SKIN_MAX_MATERIALS || !cJSON_IsArray(e) || cJSON_GetArraySize(e) < 3) continue;
        dg_strcpy(names[*count], 48, e->string);
        float a = cJSON_GetArraySize(e) > 3 ? (float)cJSON_GetArrayItem(e, 3)->valuedouble : 1.0f;
        colors[(*count)++] =
            v4((float)cJSON_GetArrayItem(e, 0)->valuedouble, (float)cJSON_GetArrayItem(e, 1)->valuedouble, (float)cJSON_GetArrayItem(e, 2)->valuedouble, a);
    }
}

bool character_add_option(CharacterDef *d, int slot, const CharPartOption *o) {
    if (slot < 0 || slot >= CSLOT_COUNT || d->slots[slot].option_count >= CHAR_MAX_PART_OPTIONS) return false;
    d->slots[slot].options[d->slots[slot].option_count++] = *o;
    d->part_option_count++;
    return true;
}

static void option_defaults(CharPartOption *o, const CharacterDef *d) {
    memset(o, 0, sizeof(*o));
    o->ref.scale = v3(1, 1, 1);
    o->mesh = -1;
    o->skin = -1;
    o->weight = 1.0f;
    // The character's material tints apply to every part unless the part has its own.
    o->material_count = d->material_count;
    memcpy(o->material_names, d->material_names, sizeof(o->material_names));
    memcpy(o->material_colors, d->material_colors, sizeof(o->material_colors));
}

static void option_id_from_ref(CharPartOption *o) {
    if (o->id[0]) return;
    char stem[64];
    fbx_file_stem_c(o->ref.path, stem, sizeof(stem));
    if (o->ref.region[0]) snprintf(o->id, sizeof(o->id), "%.15s_%.15s", stem, o->ref.region);
    else dg_strcpy(o->id, sizeof(o->id), stem);
}

static bool parse_option(CharacterDef *d, int slot, const cJSON *e, const char *source, char *err, int err_size) {
    CharPartOption o;
    option_defaults(&o, d);
    if (!mesh_ref_parse(&o.ref, e)) {
        if (err) snprintf(err, (size_t)err_size, "%s: part '%s': expected a mesh path or { \"mesh\": ... }", source, char_slot_name(slot));
        return false;
    }
    if (cJSON_IsObject(e)) {
        dg_strcpy(o.id, sizeof(o.id), js(e, "id", ""));
        o.weight = jf(e, "weight", 1.0f);
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(e, "default");
        o.is_default = cJSON_IsTrue(def);
        const cJSON *mats = cJSON_GetObjectItemCaseSensitive(e, "materials");
        if (cJSON_IsObject(mats)) {
            // Part tints override the character's by name; others keep the character's.
            int n = 0;
            char names[SKIN_MAX_MATERIALS][48];
            vec4 cols[SKIN_MAX_MATERIALS];
            parse_materials(mats, &n, names, cols);
            for (int i = 0; i < n; i++) {
                int at = -1;
                for (int k = 0; k < o.material_count; k++) if (!strcmp(o.material_names[k], names[i])) at = k;
                if (at < 0 && o.material_count < SKIN_MAX_MATERIALS) { at = o.material_count++; dg_strcpy(o.material_names[at], 48, names[i]); }
                if (at >= 0) o.material_colors[at] = cols[i];
            }
        }
        const cJSON *h;
        cJSON_ArrayForEach(h, cJSON_GetObjectItemCaseSensitive(e, "hide"))
            if (cJSON_IsString(h) && o.hide_count < CHAR_MAX_HIDE) dg_strcpy(o.hide_names[o.hide_count++], 24, h->valuestring);
    }
    option_id_from_ref(&o);
    if (!character_add_option(d, slot, &o)) {
        if (err) snprintf(err, (size_t)err_size, "%s: slot '%s' has more than %d options", source, char_slot_name(slot), CHAR_MAX_PART_OPTIONS);
        return false;
    }
    return true;
}

static bool parse_parts(CharacterDef *d, const cJSON *root, const char *source, char *err, int err_size) {
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(root, "parts");
    int gear_next = CSLOT_GEAR_0;
    const cJSON *e;
    cJSON_ArrayForEach(e, parts) {
        int slot = char_slot_from_name(e->string);
        // "gear_<anything>" (gear_helmet, gear_backpack): the next free gear slot, in file order.
        if (slot < 0 && !strncmp(e->string, "gear_", 5) && gear_next <= CSLOT_GEAR_3) slot = gear_next++;
        else if (slot >= CSLOT_GEAR_0 && slot >= gear_next) gear_next = slot + 1;
        if (slot < 0) {
            if (err) snprintf(err, (size_t)err_size, "%s: unknown part slot '%s' (body, head, torso, legs, hands, hair, gear_*)", source, e->string);
            return false;
        }
        if (cJSON_IsArray(e)) {
            const cJSON *o;
            cJSON_ArrayForEach(o, e) if (!parse_option(d, slot, o, source, err, err_size)) return false;
        } else if (!parse_option(d, slot, e, source, err, err_size)) {
            return false;
        }
    }
    // Legacy single mesh = the body part (when "parts" doesn't fill the body slot itself).
    if (!d->slots[CSLOT_BODY].option_count && (cJSON_GetObjectItemCaseSensitive(root, "mesh") || !d->part_option_count)) {
        CharPartOption o;
        option_defaults(&o, d);
        dg_strcpy(o.ref.path, sizeof(o.ref.path), d->mesh_path);
        option_id_from_ref(&o);
        character_add_option(d, CSLOT_BODY, &o);
    }
    return true;
}

void character_resolve_parts(CharacterDef *d, const SkeletonAsset *a) {
    for (int s = 0; s < CSLOT_COUNT; s++)
        for (int i = 0; i < d->slots[s].option_count; i++) {
            CharPartOption *o = &d->slots[s].options[i];
            memset(&o->hide, 0, sizeof(o->hide));
            for (int h = 0; h < o->hide_count; h++) {
                int g = skel_asset_find_group(a, o->hide_names[h]);
                if (g >= 0) boneset_or(&o->hide, &a->groups[g].bones);
                else log_warn("character %s: part %s hides unknown bone group '%s'", d->id, o->id, o->hide_names[h]);
            }
        }
}

void character_pick_parts(const CharacterDef *d, u32 seed, u8 *out) {
    for (int s = 0; s < CSLOT_COUNT; s++) {
        const CharPartSlot *sl = &d->slots[s];
        out[s] = CHAR_PART_NONE;
        int def = -1;
        float total = 0;
        for (int i = 0; i < sl->option_count; i++) {
            if (sl->options[i].mesh < 0) continue;
            if (sl->options[i].is_default && def < 0) def = i;
            total += DG_MAX(sl->options[i].weight, 0.0f);
        }
        if (def >= 0) { out[s] = (u8)(def + 1); continue; }
        if (total <= 0) {
            // Only explicit-choice options: the first usable one.
            for (int i = 0; i < sl->option_count; i++) if (sl->options[i].mesh >= 0) { out[s] = (u8)(i + 1); break; }
            continue;
        }
        u32 h = anim_hash(&seed, sizeof(seed), anim_hash(&s, sizeof(s), ANIM_HASH_SEED));
        float r = (float)(h & 0xFFFFFF) / 16777216.0f * total;
        for (int i = 0; i < sl->option_count; i++) {
            if (sl->options[i].mesh < 0) continue;
            float w = DG_MAX(sl->options[i].weight, 0.0f);
            out[s] = (u8)(i + 1);
            if (r < w) break;
            r -= w;
        }
    }
}

int character_find_option(const CharacterDef *d, int slot, const char *name) {
    if (slot < 0 || slot >= CSLOT_COUNT) return -1;
    const CharPartSlot *sl = &d->slots[slot];
    for (int i = 0; i < sl->option_count; i++) if (!strcmp(sl->options[i].id, name) || !strcmp(sl->options[i].ref.path, name)) return i;
    for (int i = 0; i < sl->option_count; i++) {
        char stem[64];
        fbx_file_stem_c(sl->options[i].ref.path, stem, sizeof(stem));
        if (!SDL_strcasecmp(stem, name)) return i;
    }
    return -1;
}

const CharPartOption *character_part(const CharacterDef *d, const u8 *sel, int slot) {
    if (slot < 0 || slot >= CSLOT_COUNT || !sel || sel[slot] == CHAR_PART_NONE) return NULL;
    int i = sel[slot] - 1;
    if (i >= d->slots[slot].option_count || d->slots[slot].options[i].mesh < 0) return NULL;
    return &d->slots[slot].options[i];
}

void character_hidden_bones(const CharacterDef *d, const u8 *sel, int slot, BoneSet *out) {
    memset(out, 0, sizeof(*out));
    for (int s = 0; s < CSLOT_COUNT; s++) {
        if (s == slot) continue;
        const CharPartOption *o = character_part(d, sel, s);
        if (o) boneset_or(out, &o->hide);
    }
}

bool character_def_parse(CharacterDef *d, const char *text, const char *source, char *err, int err_size) {
    memset(d, 0, sizeof(*d));
    cJSON *root = cJSON_Parse(text);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "%s: JSON parse error", source); return false; }
    dg_strcpy(d->id, sizeof(d->id), js(root, "id", "character"));
    d->net_id = (u8)jf(root, "character_id", 0);
    const char *kind = js(root, "kind", "zombie");
    d->kind = !strcmp(kind, "player") ? CK_PLAYER : (!strcmp(kind, "dummy") ? CK_DUMMY : CK_ZOMBIE);
    dg_strcpy(d->mesh_path, sizeof(d->mesh_path), js(root, "mesh", "assets/human/Dummy.fbx"));
    const cJSON *lods = cJSON_GetObjectItemCaseSensitive(root, "lods");
    int li = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, lods) if (cJSON_IsString(e) && li < CHAR_LOD_FILES) dg_strcpy(d->lod_paths[li++], 128, e->valuestring);
    parse_materials(cJSON_GetObjectItemCaseSensitive(root, "materials"), &d->material_count, d->material_names, d->material_colors);
    if (!parse_parts(d, root, source, err, err_size)) { cJSON_Delete(root); return false; }
    dg_strcpy(d->anim_set, sizeof(d->anim_set), js(root, "anim_set", d->kind == CK_PLAYER ? "player" : "zombie"));
    part_mult_defaults(d->part_mult);
    part_mult_parse(d->part_mult, cJSON_GetObjectItemCaseSensitive(root, "part_mult"));
    parse_blood(d, root);
    d->ragdoll_scale = jf(root, "ragdoll_scale", 1.0f);
    d->scale = jf(root, "scale", 1.0f);
    d->hp = jf(root, "hp", d->kind == CK_PLAYER ? 100.0f : 120.0f);
    d->move_speed = jf(root, "move_speed", 1.3f);
    d->lod_bias = jf(root, "lod_bias", 1.0f);
    d->socket_count = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "sockets")) {
        if (!cJSON_IsObject(e) || d->socket_count >= CHAR_MAX_SOCKETS) continue;
        CharSocket *k = &d->sockets[d->socket_count++];
        memset(k, 0, sizeof(*k));
        dg_strcpy(k->name, sizeof(k->name), e->string);
        dg_strcpy(k->bone_name, sizeof(k->bone_name), js(e, "bone", "Head"));
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(e, "offset");
        if (cJSON_IsArray(o) && cJSON_GetArraySize(o) >= 3)
            k->offset = v3((float)cJSON_GetArrayItem(o, 0)->valuedouble, (float)cJSON_GetArrayItem(o, 1)->valuedouble, (float)cJSON_GetArrayItem(o, 2)->valuedouble);
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(e, "rotation");
        if (cJSON_IsArray(r) && cJSON_GetArraySize(r) >= 3)
            k->rotation_deg = v3((float)cJSON_GetArrayItem(r, 0)->valuedouble, (float)cJSON_GetArrayItem(r, 1)->valuedouble, (float)cJSON_GetArrayItem(r, 2)->valuedouble);
        k->bind_space = strcmp(js(e, "space", "bind"), "bone") != 0;
        k->bone = -1;
    }
    // "eye_offset": eye position relative to the Head bone (bind model axes): the first-person
    // camera and every hold point hang off it. Overrides the "camera" socket offset.
    const cJSON *eo = cJSON_GetObjectItemCaseSensitive(root, "eye_offset");
    if (cJSON_IsArray(eo) && cJSON_GetArraySize(eo) >= 3) {
        vec3 off = v3((float)cJSON_GetArrayItem(eo, 0)->valuedouble, (float)cJSON_GetArrayItem(eo, 1)->valuedouble, (float)cJSON_GetArrayItem(eo, 2)->valuedouble);
        CharSocket *k = NULL;
        for (int i = 0; i < d->socket_count; i++) if (!strcmp(d->sockets[i].name, "camera")) k = &d->sockets[i];
        if (!k && d->socket_count < CHAR_MAX_SOCKETS) {
            k = &d->sockets[d->socket_count++];
            memset(k, 0, sizeof(*k));
            dg_strcpy(k->name, sizeof(k->name), "camera");
            dg_strcpy(k->bone_name, sizeof(k->bone_name), "Head");
            k->bind_space = true;
            k->bone = -1;
        }
        if (k) k->offset = off;
    }
    d->hash = anim_hash_str(text, ANIM_HASH_SEED);
    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------------------ sockets

void character_resolve_sockets(CharacterDef *d, const SkeletonAsset *a) {
    const Skeleton *s = &a->skel;
    // Skeleton sockets the character doesn't override (eye_offset / "sockets" in its JSON win).
    for (int i = 0; i < a->socket_count && d->socket_count < CHAR_MAX_SOCKETS; i++)
        if (!character_socket(d, a->sockets[i].name)) d->sockets[d->socket_count++] = a->sockets[i];
    // Every character has an eye socket (hold points of remote players / dummies hang off it too).
    if (!character_socket(d, "camera") && d->socket_count < CHAR_MAX_SOCKETS) {
        CharSocket *k = &d->sockets[d->socket_count++];
        memset(k, 0, sizeof(*k));
        dg_strcpy(k->name, sizeof(k->name), "camera");
        dg_strcpy(k->bone_name, sizeof(k->bone_name), "Head");
        k->offset = v3(0, 0.10f, -0.10f);   // eyes: above / in front of the head joint
        k->bind_space = true;
    }
    for (int i = 0; i < d->socket_count; i++) skel_socket_resolve(&d->sockets[i], s);
}

const CharSocket *character_socket(const CharacterDef *d, const char *name) {
    for (int i = 0; i < d->socket_count; i++) if (!strcmp(d->sockets[i].name, name)) return &d->sockets[i];
    return NULL;
}

bool character_socket_world(const CharacterDef *d, const char *name, const Xform *bone_world, Xform *out) {
    const CharSocket *k = character_socket(d, name);
    if (!k || k->bone < 0) return false;
    *out = skel_socket_world(k, bone_world);
    return true;
}

// ------------------------------------------------------------------ rig validation

static void rep(RigReport *r, const char *fmt, ...) {
    if (r->len >= (int)sizeof(r->text) - 2) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->text + r->len, sizeof(r->text) - (size_t)r->len, fmt, ap);
    va_end(ap);
    if (n > 0) r->len = DG_MIN(r->len + n, (int)sizeof(r->text) - 1);
}

// Nearest ancestor of bone b (in skel s) that exists in other (by name); returns its name or NULL.
static const char *nearest_shared_ancestor(const Skeleton *s, int b, const Skeleton *other) {
    for (int p = s->parent[b]; p >= 0; p = s->parent[p])
        if (skel_find(other, s->names[p]) >= 0) return s->names[p];
    return NULL;
}

// Bone names equal ignoring the namespace prefix ("mixamorig8:Hips" == "mixamorig1:Hips").
static bool bone_name_eq(const char *a, const char *b) {
    const char *ca = strrchr(a, ':'), *cb = strrchr(b, ':');
    return !strcmp(ca ? ca + 1 : a, cb ? cb + 1 : b);
}

static bool has_canonical_descendant(const Skeleton *model, int b, const Skeleton *canon) {
    for (int i = b + 1; i < model->count; i++)
        if (skel_is_descendant(model, i, b) && skel_find(canon, model->names[i]) >= 0) return true;
    return false;
}

bool rig_validate(const Skeleton *canon, const Skeleton *model, float tol, RigReport *r) {
    memset(r, 0, sizeof(*r));
    // Canonical bones missing from the model.
    for (int c = 0; c < canon->count; c++) {
        if (skel_find(model, canon->names[c]) >= 0) continue;
        r->missing++;
        // A missing bone whose canonical descendants exist in the model breaks the hierarchy.
        bool child_present = false;
        for (int k = c + 1; k < canon->count && !child_present; k++)
            if (skel_is_descendant(canon, k, c) && skel_find(model, canon->names[k]) >= 0) child_present = true;
        if (child_present) {
            // Renamed? A model bone that isn't canonical, at the same place (same shared parent).
            const char *cp = canon->parent[c] >= 0 ? canon->names[canon->parent[c]] : NULL;
            const char *cand = NULL;
            for (int m = 0; m < model->count; m++) {
                if (skel_find(canon, model->names[m]) >= 0) continue;
                const char *mp = model->parent[m] >= 0 ? model->names[model->parent[m]] : NULL;
                if ((cp && mp && bone_name_eq(cp, mp)) || (!cp && !mp)) { cand = model->names[m]; break; }
            }
            r->hierarchy_errors++;
            if (cand) rep(r, "ERROR renamed bone: canonical '%s' appears as '%s'\n", canon->names[c], cand);
            else rep(r, "ERROR missing intermediate bone '%s' (its children are present)\n", canon->names[c]);
        } else {
            if (r->missing <= 24) rep(r, "  subset: '%s' missing (canonical bind pose used)\n", canon->names[c]);
        }
    }
    for (int m = 0; m < model->count; m++) {
        int c = skel_find(canon, model->names[m]);
        if (c < 0) {
            r->extra++;
            if (has_canonical_descendant(model, m, canon)) {
                // Inserted bone between canonical ones (or the renamed case, already reported).
                bool reported = false;
                for (int k = 0; k < canon->count && !reported; k++)
                    if (skel_find(model, canon->names[k]) < 0) reported = strstr(r->text, model->names[m]) != NULL;
                if (!reported) {
                    r->hierarchy_errors++;
                    rep(r, "ERROR extra bone '%s' has canonical children (hierarchy differs)\n", model->names[m]);
                }
            } else if (r->extra <= 24) {
                const char *anc = nearest_shared_ancestor(model, m, canon);
                rep(r, "  extra leaf bone '%s' follows '%s' (cosmetic layer)\n", model->names[m], anc ? anc : "(root)");
            }
            continue;
        }
        // Parent check against the nearest shared ancestors.
        const char *mp = nearest_shared_ancestor(model, m, canon);
        const char *cp = nearest_shared_ancestor(canon, c, model);
        if ((mp == NULL) != (cp == NULL) || (mp && cp && !bone_name_eq(mp, cp))) {
            r->hierarchy_errors++;
            rep(r, "ERROR bone '%s' parent differs: model '%s' vs canonical '%s'\n", model->names[m], mp ? mp : "(root)", cp ? cp : "(root)");
            continue;
        }
        // Proportions.
        if (model->parent[m] >= 0 && canon->parent[c] >= 0) {
            float lm = v3_len(model->bind[m].t), lc = v3_len(canon->bind[c].t);
            if (lc > 0.02f && fabsf(lm - lc) / lc > tol) {
                r->proportion_warnings++;
                if (r->proportion_warnings <= 16)
                    rep(r, "  warn: '%s' length %.3f m vs canonical %.3f m (%+.0f%%)\n", model->names[m], lm, lc, (lm - lc) / lc * 100.0f);
            }
        }
    }
    r->identical = r->missing == 0 && r->extra == 0 && r->hierarchy_errors == 0;
    r->ok = r->hierarchy_errors == 0;
    rep(r, "%s: %d missing, %d extra, %d hierarchy errors, %d proportion warnings\n",
        r->ok ? (r->identical ? "OK identical rig" : "OK compatible rig") : "REJECTED", r->missing, r->extra, r->hierarchy_errors,
        r->proportion_warnings);
    return r->ok;
}
