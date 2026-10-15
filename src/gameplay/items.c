#include "items.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
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

bool items_load(ItemDB *db, const char *items_path, const char *weapons_path) {
    memset(db, 0, sizeof(*db));
    dg_strcpy(db->items[0].id, sizeof(db->items[0].id), "none");
    dg_strcpy(db->items[0].name, sizeof(db->items[0].name), "Empty");
    db->items[0].weapon = -1;
    db->item_count = 1;

    cJSON *wroot = load_json(weapons_path);
    cJSON *iroot = load_json(items_path);
    if (!wroot || !iroot) { cJSON_Delete(wroot); cJSON_Delete(iroot); return false; }

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
    }

    const cJSON *w;
    cJSON_ArrayForEach(w, cJSON_GetObjectItemCaseSensitive(wroot, "weapons")) {
        if (db->weapon_count >= MAX_WEAPON_DEFS) break;
        WeaponDef *d = &db->weapons[db->weapon_count];
        dg_strcpy(d->id, sizeof(d->id), jstr(w, "id", "weapon"));
        const char *k = jstr(w, "kind", "hitscan");
        d->kind = !strcmp(k, "melee") ? WPN_MELEE : (!strcmp(k, "projectile") ? WPN_PROJECTILE : WPN_HITSCAN);
        d->damage_type = dmg_from(jstr(w, "damage_type", d->kind == WPN_MELEE ? "melee" : "bullet"));
        d->damage = jnum(w, "damage", 10);
        d->fire_rate = jnum(w, "fire_rate", 2);
        d->automatic = jbool(w, "automatic", false);
        d->mag_size = (int)jnum(w, "mag_size", 0);
        d->ammo_item = item_find(db, jstr(w, "ammo", ""));
        d->reload_time = jnum(w, "reload_time", 1.5f);
        d->pellets = (int)jnum(w, "pellets", 1);
        d->spread_base = jnum(w, "spread_base", 0.5f);
        d->spread_move = jnum(w, "spread_move", 1.5f);
        d->spread_ads = jnum(w, "spread_ads", 0.3f);
        d->spread_per_shot = jnum(w, "spread_per_shot", 0.5f);
        d->spread_max = jnum(w, "spread_max", 6);
        d->spread_recover = jnum(w, "spread_recover", 8);
        d->recoil_pitch = jnum(w, "recoil_pitch", 1);
        d->recoil_yaw = jnum(w, "recoil_yaw", 0.3f);
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
        // Link to the item that carries this weapon.
        u16 item = item_find(db, jstr(w, "item", d->id));
        if (item) db->items[item].weapon = db->weapon_count;
        db->weapon_count++;
    }
    cJSON_Delete(wroot);
    cJSON_Delete(iroot);
    log_info("items: %d item defs, %d weapon defs", db->item_count - 1, db->weapon_count);
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
