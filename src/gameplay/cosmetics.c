// Cosmetics (src/gameplay/cosmetics.h).
#include "cosmetics.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_slot_names[COS_SLOTS] = { "Body", "Head", "Torso", "Legs", "Hands", "Hair", "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Weapon skins" };
static const char *const k_rarity[CR_COUNT] = { "common", "uncommon", "rare", "epic", "legendary" };
static const char *const k_rarity_ui[CR_COUNT] = { "Common", "Uncommon", "Rare", "Epic", "Legendary" };

const char *cosmetics_slot_name(int slot) { return slot >= 0 && slot < COS_SLOTS ? k_slot_names[slot] : "?"; }
const char *cosmetics_rarity_name(int r) { return r >= 0 && r < CR_COUNT ? k_rarity_ui[r] : "?"; }

int cosmetics_rarity_from_name(const char *s) {
    for (int i = 0; i < CR_COUNT; i++) if (!strcmp(s, k_rarity[i])) return i;
    return -1;
}

static int slot_from_name(const char *s) {
    if (!strcmp(s, "weapon_skin")) return COS_SLOT_WEAPON;
    return char_slot_from_name(s);
}

bool cosmetics_parse(CosmeticDB *db, const char *json, const char *file, char *err, int err_size) {
    if (err && err_size) err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "%s: JSON syntax error", file); return false; }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    CosmeticItem tmp[COS_MAX_ITEMS];
    int n = 0;
    const char *bad = NULL;
    char where[48] = "";
    if (!cJSON_IsArray(items)) bad = "missing \"items\" array";
    const cJSON *e;
    cJSON_ArrayForEach(e, items) {
        if (bad) break;
        if (db->count + n >= COS_MAX_ITEMS) { bad = "too many items"; break; }
        CosmeticItem *it = &tmp[n];
        memset(it, 0, sizeof(*it));
        const cJSON *v;
        if (!cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "id")) || !v->valuestring[0]) { bad = "item without \"id\""; break; }
        dg_strcpy(it->id, sizeof(it->id), v->valuestring);
        dg_strcpy(where, sizeof(where), it->id);
        dg_strcpy(it->name, sizeof(it->name), it->id);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "name"))) dg_strcpy(it->name, sizeof(it->name), v->valuestring);
        if (!cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "slot")) || (it->slot = slot_from_name(v->valuestring)) < 0) { bad = "bad \"slot\""; break; }
        if (!cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "option")) || !v->valuestring[0]) { bad = "missing \"option\" (part option / skin id)"; break; }
        dg_strcpy(it->option, sizeof(it->option), v->valuestring);
        it->rarity = CR_COMMON;
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "rarity")) && (it->rarity = cosmetics_rarity_from_name(v->valuestring)) < 0) { bad = "bad \"rarity\""; break; }
        it->unlock = CU_DEFAULT;
        v = cJSON_GetObjectItemCaseSensitive(e, "unlock");
        if (cJSON_IsString(v)) {
            if (!strcmp(v->valuestring, "default")) it->unlock = CU_DEFAULT;
            else if (!strcmp(v->valuestring, "locked")) it->unlock = CU_LOCKED;
            else if (!strncmp(v->valuestring, "steam:", 6)) { it->unlock = CU_STEAM; it->steam_item = (u32)strtoul(v->valuestring + 6, NULL, 10); }
            else { bad = "bad \"unlock\" (default | locked | steam:<itemdef>)"; break; }
        }
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "icon"))) dg_strcpy(it->icon, sizeof(it->icon), v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "description"))) dg_strcpy(it->description, sizeof(it->description), v->valuestring);
        it->tint = 0xFF808080u;
        v = cJSON_GetObjectItemCaseSensitive(e, "tint");
        if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 3) {
            u32 c[3];
            for (int k = 0; k < 3; k++) c[k] = (u32)DG_CLAMP((int)(cJSON_GetArrayItem(v, k)->valuedouble * 255.0 + 0.5), 0, 255);
            it->tint = c[0] | (c[1] << 8) | (c[2] << 16) | 0xFF000000u;
        }
        for (int k = 0; k < db->count; k++) if (!strcmp(db->items[k].id, it->id)) bad = "duplicate id";
        for (int k = 0; k < n; k++) if (!strcmp(tmp[k].id, it->id)) bad = "duplicate id";
        if (bad) break;
        n++;
    }
    cJSON_Delete(root);
    if (bad) { if (err) snprintf(err, (size_t)err_size, "%s: %s%s%s", file, where[0] ? where : "", where[0] ? ": " : "", bad); return false; }
    memcpy(&db->items[db->count], tmp, sizeof(CosmeticItem) * (size_t)n);
    db->count += n;
    return true;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

int cosmetics_load(CosmeticDB *db, const char *dir_rel) {
    memset(db, 0, sizeof(*db));
    char dir[512];
    platform_data_path(dir_rel, dir, sizeof(dir));
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    char *names[64];
    int nn = files ? DG_MIN(count, ARRAY_COUNT(names)) : 0;
    for (int i = 0; i < nn; i++) names[i] = files[i];
    qsort(names, (size_t)nn, sizeof(names[0]), cmp_str);
    for (int i = 0; i < nn; i++) {
        char rel[256], buf[512], err[160];
        snprintf(rel, sizeof(rel), "%s/%s", dir_rel, names[i]);
        char *text = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
        if (!text) continue;
        if (!cosmetics_parse(db, text, rel, err, sizeof(err))) log_error("cosmetics: %s", err);
        free(text);
    }
    SDL_free(files);
    log_info("cosmetics: %d items in %s", db->count, dir_rel);
    return db->count;
}

const CosmeticItem *cosmetics_find(const CosmeticDB *db, const char *id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < db->count; i++) if (!strcmp(db->items[i].id, id)) return &db->items[i];
    return NULL;
}

bool cosmetics_steam_owns(u32 item_def) {
    (void)item_def;   // ISteamInventory::GetAllItems once the item definitions are published
    return false;
}

bool cosmetics_owned(const CosmeticItem *it, const CosmeticUnlocks *u) {
    if (!it) return false;
    if (it->unlock == CU_DEFAULT) return true;
    if (u) for (int i = 0; i < u->count; i++) if (!strcmp(u->ids[i], it->id)) return true;
    return it->unlock == CU_STEAM && cosmetics_steam_owns(it->steam_item);
}

bool cosmetics_equip(const CosmeticDB *db, CosmeticLoadout *lo, const CosmeticUnlocks *u, const char *id) {
    const CosmeticItem *it = cosmetics_find(db, id);
    if (!it || !cosmetics_owned(it, u)) return false;
    dg_strcpy(lo->equipped[it->slot], sizeof(lo->equipped[0]), it->id);
    return true;
}

void cosmetics_unequip(CosmeticLoadout *lo, int slot) {
    if (slot >= 0 && slot < COS_SLOTS) lo->equipped[slot][0] = 0;
}

bool cosmetics_is_equipped(const CosmeticLoadout *lo, const char *id) {
    for (int s = 0; s < COS_SLOTS; s++) if (lo->equipped[s][0] && !strcmp(lo->equipped[s], id)) return true;
    return false;
}

int cosmetics_to_parts(const CosmeticDB *db, const CosmeticLoadout *lo, const CharacterDef *def, int *out) {
    int missing = 0;
    for (int s = 0; s < CSLOT_COUNT; s++) {
        out[s] = -1;
        const CosmeticItem *it = cosmetics_find(db, lo->equipped[s]);
        if (!it || it->slot != s) continue;
        int o = def ? character_find_option(def, s, it->option) : -1;
        if (o < 0) { missing++; continue; }
        out[s] = o;
    }
    return missing;
}

void cosmetics_from_parts(const CosmeticDB *db, const CharacterDef *def, const u8 *sel, CosmeticLoadout *out) {
    memset(out, 0, sizeof(*out));
    for (int s = 0; s < CSLOT_COUNT; s++) {
        const CharPartOption *o = character_part(def, sel, s);
        if (!o) continue;
        for (int i = 0; i < db->count; i++) {
            const CosmeticItem *it = &db->items[i];
            if (it->slot == s && !strcmp(it->option, o->id)) { dg_strcpy(out->equipped[s], sizeof(out->equipped[0]), it->id); break; }
        }
    }
}

void cosmetics_parts_string(const CosmeticDB *db, const CosmeticLoadout *lo, char *out, int size) {
    int len = 0;
    out[0] = 0;
    for (int s = CSLOT_BODY + 1; s < CSLOT_COUNT; s++) {
        const CosmeticItem *it = cosmetics_find(db, lo->equipped[s]);
        if (!it || it->slot != s) continue;
        len += snprintf(out + len, (size_t)DG_MAX(0, size - len), "%s%s:%s", len ? " " : "", char_slot_name(s), it->option);
        if (len >= size) { out[size - 1] = 0; break; }
    }
}

const char *cosmetics_body_skin(const CosmeticDB *db, const CosmeticLoadout *lo) {
    const CosmeticItem *it = cosmetics_find(db, lo->equipped[CSLOT_BODY]);
    return it && it->slot == CSLOT_BODY ? it->option : "";
}

u32 cosmetics_hash(const CosmeticDB *db) {
    u32 h = 0x85EBCA6Bu;
    for (int i = 0; i < db->count; i++) h = dg_hash_u32(h ^ dg_hash_str(db->items[i].id) ^ (dg_hash_str(db->items[i].option) * 31u) ^ (u32)db->items[i].slot);
    return h;
}
