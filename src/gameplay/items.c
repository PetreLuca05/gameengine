#include "items.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include "gameplay/hitboxes.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static const char *jstr(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static bool jbool(const cJSON *o, const char *k, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static const cJSON *jobj(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsObject(v) ? v : NULL;
}

static vec3 jvec3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble,
              (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}

static vec4 jcolor(const cJSON *o, const char *k, vec4 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    float a = cJSON_GetArraySize(v) > 3 ? (float)cJSON_GetArrayItem(v, 3)->valuedouble : 1.0f;
    return v4((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble,
              (float)cJSON_GetArrayItem(v, 2)->valuedouble, a);
}

static VisShape shape_from(const char *s) {
    if (!strcmp(s, "cylinder")) return VIS_CYLINDER;
    if (!strcmp(s, "sphere")) return VIS_SPHERE;
    if (!strcmp(s, "capsule")) return VIS_CAPSULE;
    if (!strcmp(s, "none")) return VIS_NONE;
    return VIS_BOX;
}

static ItemKind kind_from(const char *s) {
    if (!strcmp(s, "ammo")) return ITEM_AMMO;
    if (!strcmp(s, "consumable")) return ITEM_CONSUMABLE;
    if (!strcmp(s, "weapon")) return ITEM_WEAPON;
    if (!strcmp(s, "key")) return ITEM_KEY;
    return ITEM_MISC;
}

static DamageType dmg_from(const char *s) {
    if (!strcmp(s, "bullet")) return DMG_BULLET;
    if (!strcmp(s, "melee")) return DMG_MELEE;
    if (!strcmp(s, "explosive")) return DMG_EXPLOSIVE;
    if (!strcmp(s, "fall")) return DMG_FALL;
    return DMG_GENERIC;
}

const char *damage_type_name(DamageType t) {
    static const char *n[DMG_COUNT] = { "generic", "bullet", "melee", "explosive", "fall" };
    return (t >= 0 && t < DMG_COUNT) ? n[t] : "?";
}

static cJSON *load_json(const char *path) {
    char buf[512];
    char *text = platform_read_file(platform_data_path(path, buf, sizeof(buf)), NULL);
    if (!text) { log_error("items: cannot read %s", path); return NULL; }
    cJSON *root = cJSON_Parse(text);
    if (!root) log_error("items: JSON parse error in %s near: %.40s", path, cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
    free(text);
    return root;
}

// Gun feel: top-level fire keys + the "recoil" block (angles in degrees, see docs/RECOIL.md).
// Missing keys keep recoil_params_default().
static void parse_recoil(const cJSON *w, RecoilParams *r) {
    recoil_params_default(r);
    r->rpm = fmaxf(1.0f, jnum(w, "rpm", jnum(w, "fire_rate", 10) * 60.0f));
    r->burst_rpm = jnum(w, "burst_rpm", r->rpm * 1.25f);
    r->burst_count = (int)jnum(w, "burst_count", 3);
    r->ads_time = jnum(w, "ads_time", r->ads_time);
    r->sprint_to_fire = jnum(w, "sprint_to_fire", r->sprint_to_fire);
    r->idle_sway = jnum(w, "idle_sway", r->idle_sway);
    r->scope_sway = jnum(w, "scope_sway", r->scope_sway);
    r->hold_breath_time = jnum(w, "hold_breath", r->hold_breath_time);
    const cJSON *fm = cJSON_GetObjectItemCaseSensitive(w, "fire_modes");
    if (cJSON_IsArray(fm)) {
        r->fire_modes = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, fm) {
            if (!cJSON_IsString(e)) continue;
            if (!strcmp(e->valuestring, "single") || !strcmp(e->valuestring, "semi")) r->fire_modes |= FIRE_SINGLE;
            else if (!strcmp(e->valuestring, "burst")) r->fire_modes |= FIRE_BURST;
            else if (!strcmp(e->valuestring, "auto")) r->fire_modes |= FIRE_AUTO;
        }
    } else {
        r->fire_modes = jbool(w, "automatic", false) ? (FIRE_AUTO | FIRE_SINGLE) : FIRE_SINGLE;
    }
    if (!r->fire_modes) r->fire_modes = FIRE_SINGLE;

    const cJSON *rc = jobj(w, "recoil");
    if (!rc) return;
    const cJSON *o;
    if ((o = jobj(rc, "vertical"))) {
        r->v_first = jnum(o, "first_shot", r->v_first);
        r->v_per = jnum(o, "per_shot", r->v_per);
        r->v_min = jnum(o, "min", r->v_min);
        r->v_max = jnum(o, "max", r->v_max);
    }
    if ((o = jobj(rc, "horizontal"))) {
        r->h_per = jnum(o, "per_shot", r->h_per);
        r->h_min = jnum(o, "min", r->h_min);
        r->h_max = jnum(o, "max", r->h_max);
        r->h_bias = jnum(o, "bias", r->h_bias);
    }
    const cJSON *pat = cJSON_GetObjectItemCaseSensitive(rc, "pattern");
    if (cJSON_IsArray(pat)) {
        r->pattern_count = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, pat) {
            if (r->pattern_count >= RECOIL_MAX_PATTERN) break;
            if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) < 2) continue;
            r->pattern[r->pattern_count++] = v2((float)cJSON_GetArrayItem(e, 0)->valuedouble, (float)cJSON_GetArrayItem(e, 1)->valuedouble);
        }
    }
    r->pattern_randomness = jnum(rc, "pattern_randomness", r->pattern_randomness);
    r->ramp_shots = (int)jnum(rc, "ramp_shots", (float)r->ramp_shots);
    r->seed_per_mag = jbool(rc, "randomize_seed_per_mag", r->seed_per_mag);
    if ((o = jobj(rc, "recovery"))) {
        r->recovery_delay = jnum(o, "delay", r->recovery_delay);
        r->recovery_speed = jnum(o, "speed", r->recovery_speed);
        r->recovery_fraction = saturate(jnum(o, "fraction", r->recovery_fraction));
        r->recovery_stop_on_input = jbool(o, "stop_on_input", r->recovery_stop_on_input);
    }
    if ((o = jobj(rc, "camera_kick"))) {
        r->cam_pitch = jnum(o, "pitch", r->cam_pitch);
        r->cam_yaw = jnum(o, "yaw", r->cam_yaw);
        r->cam_roll = jnum(o, "roll", r->cam_roll);
        r->cam_back = jnum(o, "back", r->cam_back);
        r->cam_spring.stiffness = jnum(o, "stiffness", r->cam_spring.stiffness);
        r->cam_spring.damping = jnum(o, "damping", r->cam_spring.damping);
    }
    if ((o = jobj(rc, "viewmodel_kick"))) {
        r->vm_back = jnum(o, "back", r->vm_back);
        r->vm_up = jnum(o, "up", r->vm_up);
        r->vm_pitch = jnum(o, "pitch", r->vm_pitch);
        r->vm_yaw = jnum(o, "yaw", r->vm_yaw);
        r->vm_roll = jnum(o, "roll", r->vm_roll);
        r->vm_random = jnum(o, "random", r->vm_random);
        r->vm_spring.stiffness = jnum(o, "stiffness", r->vm_spring.stiffness);
        r->vm_spring.damping = jnum(o, "damping", r->vm_spring.damping);
        r->vm_ads_scale = jnum(o, "ads_scale", r->vm_ads_scale);
    }
    if ((o = jobj(rc, "spread"))) {
        r->base_hip = jnum(o, "hip", r->base_hip);
        r->base_ads = jnum(o, "ads", r->base_ads);
        r->per_shot_bloom = jnum(o, "per_shot_bloom", r->per_shot_bloom);
        r->bloom_max = jnum(o, "bloom_max", r->bloom_max);
        r->bloom_recovery = jnum(o, "bloom_recovery", r->bloom_recovery);
        r->s_move = jnum(o, "move", r->s_move);
        r->s_crouch = jnum(o, "crouch", r->s_crouch);
        r->s_prone = jnum(o, "prone", r->s_prone);
        r->s_air = jnum(o, "air", r->s_air);
    }
    if ((o = jobj(rc, "modifiers"))) {
        r->ads_mult = jnum(o, "ads", r->ads_mult);
        r->crouch_mult = jnum(o, "crouch", r->crouch_mult);
        r->prone_mult = jnum(o, "prone", r->prone_mult);
        r->moving_mult = jnum(o, "moving", r->moving_mult);
        r->air_mult = jnum(o, "air", r->air_mult);
        r->fm_single = jnum(o, "single", r->fm_single);
        r->fm_burst = jnum(o, "burst", r->fm_burst);
        r->fm_auto = jnum(o, "auto", r->fm_auto);
        r->after_sprint_kick = jnum(o, "after_sprint", r->after_sprint_kick);
    }
}

// ------------------------------------------------------------------ hold sockets

static quat frame_from(vec3 fingers, vec3 back) {
    // Palm frame: -Z = fingers, +Y = back of the hand (orthonormalized).
    vec3 z = v3_norm(v3_neg(fingers));
    vec3 y = v3_sub(back, v3_scale(z, v3_dot(back, z)));
    y = v3_len2(y) > 1e-8f ? v3_norm(y) : v3(0, 1, 0);
    vec3 x = v3_cross(y, z);
    // rotation from columns x, y, z
    float tr = x.x + y.y + z.z;
    quat q;
    if (tr > 0) {
        float r = sqrtf(tr + 1.0f) * 2;
        q.w = 0.25f * r; q.x = (y.z - z.y) / r; q.y = (z.x - x.z) / r; q.z = (x.y - y.x) / r;
    } else if (x.x > y.y && x.x > z.z) {
        float r = sqrtf(1.0f + x.x - y.y - z.z) * 2;
        q.w = (y.z - z.y) / r; q.x = 0.25f * r; q.y = (y.x + x.y) / r; q.z = (z.x + x.z) / r;
    } else if (y.y > z.z) {
        float r = sqrtf(1.0f + y.y - x.x - z.z) * 2;
        q.w = (z.x - x.z) / r; q.x = (y.x + x.y) / r; q.y = 0.25f * r; q.z = (z.y + y.z) / r;
    } else {
        float r = sqrtf(1.0f + z.z - x.x - y.y) * 2;
        q.w = (x.y - y.x) / r; q.x = (z.x + x.z) / r; q.y = (z.y + y.z) / r; q.z = 0.25f * r;
    }
    return quat_norm(q);
}

static Xform socket_from_json(const cJSON *o, Xform def) {
    if (!o) return def;
    Xform x = def;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "pos");
    if (cJSON_IsArray(p) && cJSON_GetArraySize(p) >= 3)
        x.t = v3((float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 1)->valuedouble, (float)cJSON_GetArrayItem(p, 2)->valuedouble);
    vec3 f = jvec3(o, "fingers", v3(0, 0, 0)), b = jvec3(o, "back", v3(0, 0, 0));
    if (v3_len2(f) > 0 && v3_len2(b) > 0) x.r = frame_from(f, b);
    vec3 e = jvec3(o, "rot", v3(1e9f, 0, 0));
    if (e.x < 1e8f) x.r = quat_euler(e.y * DG_DEG2RAD, e.x * DG_DEG2RAD, e.z * DG_DEG2RAD);
    return x;
}

// Default sockets per hold style, from the viewmodel grip data (grip = item origin, support offset).
static void default_sockets(ItemSockets *k, const char *style, vec3 support, vec3 muzzle, const WeaponModelPart *parts, int part_count) {
    memset(k, 0, sizeof(*k));
    bool melee = !strncmp(style, "melee", 5);
    bool pistol = !strcmp(style, "pistol");
    bool small = !strcmp(style, "item_small") || !strcmp(style, "none");
    // Right hand around the grip: back of the hand to the right, fingers wrapping forward-down.
    k->grip_r = xf_make(v3(0, 0, 0), melee || small ? frame_from(v3(0, 0, -1), v3(1, 0, 0)) : frame_from(v3(0, -0.5f, -0.866f), v3(1, 0, 0)));
    // Left hand: under the handguard (palm up-right), cupping the right hand for pistols,
    // stacked below the right hand for two-handed melee.
    quat lrot = frame_from(v3(0.75f, -0.45f, -0.3f), v3(-0.5f, -0.85f, 0.0f));
    if (pistol) lrot = frame_from(v3(0, -0.5f, -0.866f), v3(-1, 0, 0));
    if (melee) lrot = frame_from(v3(0, 0, -1), v3(-1, 0, 0));
    k->grip_l = xf_make(support, lrot);
    k->has_left = v3_len2(support) > 1e-6f && !small;
    k->muzzle = muzzle;
    float top = 0.03f, len = 0.1f;
    for (int i = 0; i < part_count; i++) {
        const WeaponModelPart *p = &parts[i];
        if (fabsf(p->offset.x) < 0.03f) top = fmaxf(top, p->offset.y + p->half.y);
        len = fmaxf(len, -(p->offset.z - p->half.z));
        len = fmaxf(len, p->offset.y + p->half.y);   // melee: along +Y
    }
    k->sight_height = top;
    k->length = len;
}

static void parse_hold(const cJSON *w, char *style, ItemSockets *k, const char *def_style, vec3 support, vec3 muzzle,
                       const WeaponModelPart *parts, int part_count) {
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(w, "hold");
    dg_strcpy(style, 16, jstr(h, "style", def_style));
    default_sockets(k, style, support, muzzle, parts, part_count);
    if (!h) return;
    const cJSON *sk = cJSON_GetObjectItemCaseSensitive(h, "sockets");
    if (!sk) return;
    k->grip_r = socket_from_json(cJSON_GetObjectItemCaseSensitive(sk, "grip_r"), k->grip_r);
    const cJSON *gl = cJSON_GetObjectItemCaseSensitive(sk, "grip_l");
    if (cJSON_IsNull(gl)) k->has_left = false;
    else if (gl) { k->grip_l = socket_from_json(gl, k->grip_l); k->has_left = true; }
    const cJSON *fg = cJSON_GetObjectItemCaseSensitive(sk, "foregrip");
    if (fg) { k->grip_l = socket_from_json(fg, k->grip_l); k->has_left = true; }
    k->muzzle = jvec3(sk, "muzzle", k->muzzle);
}

static const char *default_style_for(const cJSON *w, WeaponKind kind, const char *id) {
    const char *pose = jstr(w, "pose", "pistol");
    if (kind == WPN_MELEE) return strstr(id, "knife") ? "melee_1h" : "melee_2h";
    if (!strcmp(id, "shotgun")) return "shotgun";
    if (!strcmp(id, "smg")) return "smg";
    if (!strcmp(id, "lmg")) return "lmg";
    return !strcmp(pose, "rifle") ? "rifle" : "pistol";
}

static void parse_weapon(ItemDB *db, const cJSON *w, const char *file) {
    if (db->weapon_count >= MAX_WEAPON_DEFS) { log_warn("items: too many weapons, skipping %s", file); return; }
    WeaponDef *d = &db->weapons[db->weapon_count];
    memset(d, 0, sizeof(*d));
    dg_strcpy(d->file, sizeof(d->file), file);
    dg_strcpy(d->id, sizeof(d->id), jstr(w, "id", "weapon"));
    const char *k = jstr(w, "kind", "hitscan");
    d->kind = !strcmp(k, "melee") ? WPN_MELEE : (!strcmp(k, "projectile") ? WPN_PROJECTILE : WPN_HITSCAN);
    d->damage_type = dmg_from(jstr(w, "damage_type", d->kind == WPN_MELEE ? "melee" : "bullet"));
    d->damage = jnum(w, "damage", 10);
    d->automatic = jbool(w, "automatic", false);
    d->mag_size = (int)jnum(w, "mag_size", 0);
    d->ammo_item = item_find(db, jstr(w, "ammo", ""));
    d->reload_time = jnum(w, "reload_time", 1.5f);
    d->pellets = (int)jnum(w, "pellets", 1);
    d->range = jnum(w, "range", 200);
    d->impulse = jnum(w, "impulse", 40);
    d->headshot_mult = jnum(w, "headshot_mult", 2);
    d->projectile_speed = jnum(w, "projectile_speed", 60);
    d->projectile_gravity = jnum(w, "projectile_gravity", 4);
    d->swing_time = jnum(w, "swing_time", 0.6f);
    d->active_start = jnum(w, "active_start", 0.15f);
    d->active_end = jnum(w, "active_end", 0.35f);
    d->reach = jnum(w, "reach", 1.8f);
    d->sweep_radius = jnum(w, "sweep_radius", 0.15f);
    d->knockback = jnum(w, "knockback", 200);
    d->swing_arc = jnum(w, "swing_arc", 90);
    d->ads_fov = jnum(w, "ads_fov", 0);
    d->bolt_action = jbool(w, "bolt_action", false);
    dg_strcpy(d->pose, sizeof(d->pose), jstr(w, "pose", "pistol"));
    dg_strcpy(d->sound_fire, sizeof(d->sound_fire), jstr(w, "sound_fire", "shot"));
    dg_strcpy(d->sound_reload, sizeof(d->sound_reload), jstr(w, "sound_reload", "reload"));
    d->grip_offset = jvec3(w, "grip", v3(0, 0, 0));
    d->support_offset = jvec3(w, "support", v3(0, 0, -0.1f));
    d->muzzle_offset = jvec3(w, "muzzle", v3(0, 0.05f, -0.2f));
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(w, "model")) {
        if (d->part_count >= 6) break;
        WeaponModelPart *mp = &d->parts[d->part_count++];
        mp->offset = jvec3(p, "offset", v3(0, 0, 0));
        mp->half = jvec3(p, "half", v3(0.02f, 0.02f, 0.1f));
        mp->color = jcolor(p, "color", v4(0.2f, 0.2f, 0.22f, 1));
    }
    if (d->kind != WPN_MELEE && d->part_count > 0 && !cJSON_GetObjectItemCaseSensitive(w, "muzzle_override")) {
        // Muzzle = front face of the forward-most model part (the barrel tip), so flashes and
        // tracers leave the visible barrel.
        int best = 0;
        for (int i = 1; i < d->part_count; i++)
            if (d->parts[i].offset.z - d->parts[i].half.z < d->parts[best].offset.z - d->parts[best].half.z) best = i;
        const WeaponModelPart *mp = &d->parts[best];
        d->muzzle_offset = v3(mp->offset.x, mp->offset.y, mp->offset.z - mp->half.z - 0.01f);
    }
    if (d->kind == WPN_MELEE) {
        d->fire_rate = jnum(w, "fire_rate", 2);
        recoil_params_default(&d->recoil);
    } else {
        parse_recoil(w, &d->recoil);
        d->fire_rate = d->recoil.rpm / 60.0f;
    }
    parse_hold(w, d->hold_style, &d->sockets, default_style_for(w, d->kind, d->id), d->support_offset, d->muzzle_offset, d->parts,
               d->part_count);
    part_mult_defaults(d->part_mult);
    part_mult_parse(d->part_mult, cJSON_GetObjectItemCaseSensitive(w, "part_mult"));
    const cJSON *fo = cJSON_GetObjectItemCaseSensitive(w, "falloff");
    d->falloff_start = jnum(fo, "start", d->range);
    d->falloff_end = jnum(fo, "end", d->range);
    d->falloff_min = jnum(fo, "min", 1.0f);
    // Link to the item that carries this weapon.
    u16 item = item_find(db, jstr(w, "item", d->id));
    if (item) db->items[item].weapon = db->weapon_count;
    else log_warn("items: weapon '%s' (%s) has no item", d->id, file);
    db->weapon_count++;
}

static void parse_weapon_root(ItemDB *db, const cJSON *root, const char *file) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "weapons");
    if (cJSON_IsArray(arr)) {
        const cJSON *w;
        cJSON_ArrayForEach(w, arr) parse_weapon(db, w, file);
    } else if (cJSON_IsObject(root)) {
        parse_weapon(db, root, file);
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static bool load_weapons(ItemDB *db, const char *weapons_path) {
    for (int i = 0; i < db->item_count; i++) db->items[i].weapon = -1;
    db->weapon_count = 0;
    size_t n = strlen(weapons_path);
    if (n > 5 && !strcmp(weapons_path + n - 5, ".json")) {
        cJSON *root = load_json(weapons_path);
        if (!root) return false;
        parse_weapon_root(db, root, weapons_path);
        cJSON_Delete(root);
        return true;
    }
    char dir[512];
    platform_data_path(weapons_path, dir, sizeof(dir));
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    if (!files || count == 0) { log_error("items: no weapon files in %s", dir); SDL_free(files); return false; }
    // Sorted so weapon indices (replicated) are identical on every machine.
    char *names[MAX_WEAPON_DEFS * 2];
    int nn = DG_MIN(count, ARRAY_COUNT(names));
    for (int i = 0; i < nn; i++) names[i] = files[i];
    qsort(names, (size_t)nn, sizeof(names[0]), cmp_str);
    for (int i = 0; i < nn; i++) {
        char rel[256];
        snprintf(rel, sizeof(rel), "%s/%s", weapons_path, names[i]);
        cJSON *root = load_json(rel);
        if (!root) continue;
        parse_weapon_root(db, root, rel);
        cJSON_Delete(root);
    }
    SDL_free(files);
    return true;
}

bool items_load(ItemDB *db, const char *items_path, const char *weapons_path) {
    memset(db, 0, sizeof(*db));
    dg_strcpy(db->items[0].id, sizeof(db->items[0].id), "none");
    dg_strcpy(db->items[0].name, sizeof(db->items[0].name), "Empty");
    db->items[0].weapon = -1;
    db->item_count = 1;

    cJSON *iroot = load_json(items_path);
    if (!iroot) return false;
    // Items first (weapons reference ammo items by id).
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(iroot, "items")) {
        if (db->item_count >= MAX_ITEM_DEFS) break;
        ItemDef *d = &db->items[db->item_count++];
        dg_strcpy(d->id, sizeof(d->id), jstr(it, "id", "unnamed"));
        dg_strcpy(d->name, sizeof(d->name), jstr(it, "name", d->id));
        dg_strcpy(d->desc, sizeof(d->desc), jstr(it, "desc", ""));
        d->kind = kind_from(jstr(it, "kind", "misc"));
        d->max_stack = (int)jnum(it, "stack", 1);
        d->color = jcolor(it, "color", v4(0.8f, 0.8f, 0.8f, 1));
        d->shape = shape_from(jstr(it, "shape", "box"));
        d->size = jvec3(it, "size", v3(0.1f, 0.1f, 0.1f));
        d->heal = jnum(it, "heal", 0);
        d->armor = jnum(it, "armor", 0);
        d->use_time = jnum(it, "use_time", 0);
        d->weapon = -1;
        // Held non-weapon items: small by default, sockets from the pickup size.
        dg_strcpy(d->hold_style, sizeof(d->hold_style), jstr(cJSON_GetObjectItemCaseSensitive(it, "hold"), "style", "item_small"));
        WeaponModelPart part = { v3(0, d->size.y, -0.03f), d->size, d->color };
        default_sockets(&d->sockets, d->hold_style, v3(0, 0, 0), v3(0, 0, -d->size.z), &part, 1);
    }
    cJSON_Delete(iroot);
    bool ok = load_weapons(db, weapons_path);
    log_info("items: %d item defs, %d weapon defs", db->item_count - 1, db->weapon_count);
    return ok;
}

bool weapons_reload_defs(ItemDB *db, const char *weapons_path) {
    static WeaponDef backup[MAX_WEAPON_DEFS];
    int links[MAX_ITEM_DEFS];
    int before = db->weapon_count;
    memcpy(backup, db->weapons, sizeof(backup));
    for (int i = 0; i < db->item_count; i++) links[i] = db->items[i].weapon;
    bool ok = load_weapons(db, weapons_path);
    bool same = ok && db->weapon_count == before;
    for (int i = 0; same && i < before; i++) same = !strcmp(backup[i].id, db->weapons[i].id);
    if (!same) {
        // Weapon indices are referenced by live state: refuse changes to the weapon set.
        memcpy(db->weapons, backup, sizeof(backup));
        db->weapon_count = before;
        for (int i = 0; i < db->item_count; i++) db->items[i].weapon = links[i];
        log_warn("weapon_reload: load failed or the weapon set changed (restart to add/remove weapons)");
        return false;
    }
    log_info("weapon_reload: %d weapon defs reloaded", db->weapon_count);
    return true;
}

u16 item_find(const ItemDB *db, const char *id) {
    if (!id || !id[0]) return ITEM_NONE;
    for (int i = 1; i < db->item_count; i++)
        if (!strcmp(db->items[i].id, id)) return (u16)i;
    return ITEM_NONE;
}

const ItemDef *item_def(const ItemDB *db, u16 item) {
    return (item < db->item_count) ? &db->items[item] : &db->items[0];
}

const WeaponDef *item_weapon(const ItemDB *db, u16 item) {
    const ItemDef *d = item_def(db, item);
    return (d->kind == ITEM_WEAPON && d->weapon >= 0) ? &db->weapons[d->weapon] : NULL;
}
