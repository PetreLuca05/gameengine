// Stats persistence (stats.h "persistence"): JSON from field tables. Appending a field to a struct
// is one row here; older files simply lack the key (zero), newer keys are skipped by older builds.
// Arrays are written up to their last non-zero element. Pure (cJSON only).
#include "stats.h"
#include "cJSON.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum StatsFieldType { SF_U8, SF_U16, SF_U32, SF_U64, SF_I16, SF_I32, SF_I64, SF_F32, SF_F64, SF_BOOL, SF_STR } StatsFieldType;
typedef struct StatsField { const char *key; u8 type; u16 count; u16 size; u32 offset; } StatsField;

#define FLD(st, key, m, t) { key, t, 1, (u16)sizeof(((st *)0)->m), (u32)offsetof(st, m) }
#define ARR(st, key, m, t) { key, t, (u16)(sizeof(((st *)0)->m) / sizeof(((st *)0)->m[0])), (u16)sizeof(((st *)0)->m[0]), (u32)offsetof(st, m) }

static const StatsField k_life[] = {
    FLD(PlayerStats, "profile", profile, SF_STR),
    FLD(PlayerStats, "kills", kills, SF_U32),
    FLD(PlayerStats, "deaths", deaths, SF_U32),
    FLD(PlayerStats, "headshots", headshots, SF_U32),
    FLD(PlayerStats, "damage_dealt", damage_dealt, SF_F64),
    FLD(PlayerStats, "damage_taken", damage_taken, SF_F64),
    FLD(PlayerStats, "best_killstreak", best_killstreak, SF_U32),
    FLD(PlayerStats, "revives_given", revives_given, SF_U32),
    FLD(PlayerStats, "times_downed", times_downed, SF_U32),
    FLD(PlayerStats, "time_played_sec", time_played_sec, SF_F64),
    FLD(PlayerStats, "total_distance_m", total_distance_m, SF_F64),
    FLD(PlayerStats, "steps", steps, SF_U64),
    FLD(PlayerStats, "jumps", jumps, SF_U32),
    FLD(PlayerStats, "matches_played", matches_played, SF_U32),
    FLD(PlayerStats, "matches_won", matches_won, SF_U32),
    FLD(PlayerStats, "highest_wave", highest_wave, SF_U32),
};

static const StatsField k_longest[] = {
    FLD(LongestKill, "dist_m", dist_m, SF_F32),
    FLD(LongestKill, "weapon", weapon, SF_I16),
    FLD(LongestKill, "map", map, SF_STR),
    FLD(LongestKill, "timestamp", timestamp, SF_I64),
};

static const StatsField k_weapon[] = {
    FLD(WeaponStats, "shots_fired", shots_fired, SF_U32),
    FLD(WeaponStats, "shots_hit", shots_hit, SF_U32),
    FLD(WeaponStats, "kills", kills, SF_U32),
    FLD(WeaponStats, "headshots", headshots, SF_U32),
    FLD(WeaponStats, "damage_dealt", damage_dealt, SF_F64),
    FLD(WeaponStats, "time_equipped_sec", time_equipped_sec, SF_F64),
    FLD(WeaponStats, "longest_kill_m", longest_kill_m, SF_F32),
    FLD(WeaponStats, "headshot_hits", headshot_hits, SF_U32),
    ARR(WeaponStats, "kill_dist", kill_dist, SF_U32),
};

static const StatsField k_match[] = {
    FLD(MatchRecord, "t", timestamp, SF_I64),
    FLD(MatchRecord, "map", map, SF_STR),
    FLD(MatchRecord, "result", result, SF_U8),
    FLD(MatchRecord, "segment_sec", segment_sec, SF_U16),
    FLD(MatchRecord, "duration_sec", duration_sec, SF_F32),
    FLD(MatchRecord, "waves_reached", waves_reached, SF_U16),
    FLD(MatchRecord, "kills", kills, SF_U32),
    FLD(MatchRecord, "deaths", deaths, SF_U32),
    FLD(MatchRecord, "headshots", headshots, SF_U32),
    FLD(MatchRecord, "shots_fired", shots_fired, SF_U32),
    FLD(MatchRecord, "shots_hit", shots_hit, SF_U32),
    FLD(MatchRecord, "damage_dealt", damage_dealt, SF_F32),
    FLD(MatchRecord, "damage_taken", damage_taken, SF_F32),
    FLD(MatchRecord, "revives", revives, SF_U16),
    FLD(MatchRecord, "downs", downs, SF_U16),
    FLD(MatchRecord, "distance_m", distance_m, SF_F32),
    FLD(MatchRecord, "best_streak", best_streak, SF_U16),
    ARR(MatchRecord, "kills_per_weapon", kills_per_weapon, SF_U16),
    ARR(MatchRecord, "kills_per_wave", kills_per_wave, SF_U16),
    ARR(MatchRecord, "downs_per_wave", downs_per_wave, SF_U8),
    ARR(MatchRecord, "revives_per_wave", revives_per_wave, SF_U8),
    ARR(MatchRecord, "shots_per_weapon", shots_per_weapon, SF_U16),
    ARR(MatchRecord, "hits_per_weapon", hits_per_weapon, SF_U16),
    FLD(MatchRecord, "longest_kill_m", longest_kill_m, SF_F32),
    FLD(MatchRecord, "steps", steps, SF_U32),
    FLD(MatchRecord, "jumps", jumps, SF_U16),
};

static const StatsField k_day[] = {
    FLD(DailyRecord, "day", day, SF_I32),
    FLD(DailyRecord, "matches", matches, SF_U16),
    FLD(DailyRecord, "kills", kills, SF_U32),
    FLD(DailyRecord, "deaths", deaths, SF_U32),
    FLD(DailyRecord, "time_sec", time_sec, SF_F32),
    FLD(DailyRecord, "shots_fired", shots_fired, SF_U32),
    FLD(DailyRecord, "shots_hit", shots_hit, SF_U32),
};

static const StatsField k_cur[] = {
    FLD(MatchStats, "active", active, SF_BOOL),
    FLD(MatchStats, "streak", streak, SF_I32),
    FLD(MatchStats, "wave", wave, SF_I32),
    FLD(MatchStats, "waves", waves, SF_BOOL),
    FLD(MatchStats, "step_events", step_events, SF_U32),
};

// ------------------------------------------------------------------ values

static double get_num(const u8 *p, int type) {
    switch (type) {
    case SF_U8: return *(const u8 *)p;
    case SF_U16: { u16 v; memcpy(&v, p, 2); return v; }
    case SF_U32: { u32 v; memcpy(&v, p, 4); return v; }
    case SF_U64: { u64 v; memcpy(&v, p, 8); return (double)v; }
    case SF_I16: { i16 v; memcpy(&v, p, 2); return v; }
    case SF_I32: { i32 v; memcpy(&v, p, 4); return v; }
    case SF_I64: { i64 v; memcpy(&v, p, 8); return (double)v; }
    case SF_F32: { float v; memcpy(&v, p, 4); return v; }
    case SF_F64: { double v; memcpy(&v, p, 8); return v; }
    case SF_BOOL: return *(const bool *)p ? 1 : 0;
    default: return 0;
    }
}

static void set_num(u8 *p, int type, double v) {
    if (!isfinite(v)) v = 0;
    switch (type) {
    case SF_U8: *p = (u8)DG_CLAMP(v, 0, 255); break;
    case SF_U16: { u16 x = (u16)DG_CLAMP(v, 0, 65535); memcpy(p, &x, 2); break; }
    case SF_U32: { u32 x = (u32)DG_CLAMP(v, 0, 4294967295.0); memcpy(p, &x, 4); break; }
    case SF_U64: { u64 x = (u64)DG_CLAMP(v, 0, 9.0e15); memcpy(p, &x, 8); break; }
    case SF_I16: { i16 x = (i16)DG_CLAMP(v, -32768, 32767); memcpy(p, &x, 2); break; }
    case SF_I32: { i32 x = (i32)DG_CLAMP(v, -2147483648.0, 2147483647.0); memcpy(p, &x, 4); break; }
    case SF_I64: { i64 x = (i64)DG_CLAMP(v, -9.0e15, 9.0e15); memcpy(p, &x, 8); break; }
    case SF_F32: { float x = (float)v; memcpy(p, &x, 4); break; }
    case SF_F64: memcpy(p, &v, 8); break;
    case SF_BOOL: *(bool *)p = v != 0; break;
    default: break;
    }
}

static void fields_write(cJSON *obj, const void *base, const StatsField *f, int n) {
    const u8 *b = (const u8 *)base;
    for (int i = 0; i < n; i++) {
        const u8 *p = b + f[i].offset;
        if (f[i].type == SF_STR) { cJSON_AddStringToObject(obj, f[i].key, (const char *)p); continue; }
        if (f[i].count == 1) {
            double v = get_num(p, f[i].type);
            if (v != 0 || f[i].type == SF_BOOL) cJSON_AddNumberToObject(obj, f[i].key, v);
            continue;
        }
        int last = -1;
        for (int k = 0; k < f[i].count; k++)
            if (get_num(p + (size_t)k * f[i].size, f[i].type) != 0) last = k;
        if (last < 0) continue;
        cJSON *a = cJSON_AddArrayToObject(obj, f[i].key);
        for (int k = 0; k <= last; k++) cJSON_AddItemToArray(a, cJSON_CreateNumber(get_num(p + (size_t)k * f[i].size, f[i].type)));
    }
}

static void fields_read(const cJSON *obj, void *base, const StatsField *f, int n) {
    u8 *b = (u8 *)base;
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, f[i].key);
        if (!v) continue;
        u8 *p = b + f[i].offset;
        if (f[i].type == SF_STR) {
            if (cJSON_IsString(v)) dg_strcpy((char *)p, f[i].size, v->valuestring);
        } else if (f[i].count == 1) {
            if (cJSON_IsNumber(v)) set_num(p, f[i].type, v->valuedouble);
            else if (cJSON_IsBool(v)) set_num(p, f[i].type, cJSON_IsTrue(v) ? 1 : 0);
        } else if (cJSON_IsArray(v)) {
            int k = 0;
            const cJSON *e;
            cJSON_ArrayForEach(e, v) {
                if (k >= f[i].count) break;
                if (cJSON_IsNumber(e)) set_num(p + (size_t)k * f[i].size, f[i].type, e->valuedouble);
                k++;
            }
        }
    }
}

// ------------------------------------------------------------------ files

char *stats_to_json(const PlayerStats *ps, const MatchStats *cur, bool pretty) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", STATS_VERSION);
    fields_write(root, ps, k_life, ARRAY_COUNT(k_life));
    fields_write(cJSON_AddObjectToObject(root, "longest_kill"), &ps->longest_kill, k_longest, ARRAY_COUNT(k_longest));
    cJSON *wa = cJSON_AddArrayToObject(root, "weapons");
    for (int i = 0; i < ps->weapon_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", ps->weapon_ids[i]);
        fields_write(o, &ps->weapons[i], k_weapon, ARRAY_COUNT(k_weapon));
        cJSON_AddItemToArray(wa, o);
    }
    cJSON *ma = cJSON_AddArrayToObject(root, "matches");
    for (int i = 0; i < ps->match_count; i++) {
        cJSON *o = cJSON_CreateObject();
        fields_write(o, stats_match_at(ps, i), k_match, ARRAY_COUNT(k_match));
        cJSON_AddItemToArray(ma, o);
    }
    cJSON *da = cJSON_AddArrayToObject(root, "days");
    for (int i = 0; i < ps->day_count; i++) {
        cJSON *o = cJSON_CreateObject();
        fields_write(o, stats_day_at(ps, i), k_day, ARRAY_COUNT(k_day));
        cJSON_AddItemToArray(da, o);
    }
    // The match in progress (autosave): a crash leaves it recorded as abandoned on the next load.
    if (cur && cur->active) {
        cJSON *c = cJSON_AddObjectToObject(root, "current");
        fields_write(c, cur, k_cur, ARRAY_COUNT(k_cur));
        fields_write(cJSON_AddObjectToObject(c, "rec"), &cur->rec, k_match, ARRAY_COUNT(k_match));
    }
    char *text = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return NULL;
    // cJSON's allocator may differ from the caller's free(): hand back a malloc'd copy.
    size_t n = strlen(text);
    char *out = (char *)malloc(n + 1);
    if (out) memcpy(out, text, n + 1);
    cJSON_free(text);
    return out;
}

static double num_of(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

bool stats_from_json(PlayerStats *ps, MatchStats *cur, const char *json, char *err, int err_size) {
    char profile[40];
    dg_strcpy(profile, sizeof(profile), ps->profile[0] ? ps->profile : "local");
    stats_init_profile(ps, profile);
    if (cur) memset(cur, 0, sizeof(*cur));
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!cJSON_IsObject(root)) {
        if (err) snprintf(err, (size_t)err_size, "not a JSON object");
        cJSON_Delete(root);
        return false;
    }
    // Version 0: the old profile.json (its own "version", no "profile", the stat stubs under "stats").
    const cJSON *legacy = cJSON_HasObjectItem(root, "profile") ? NULL : cJSON_GetObjectItemCaseSensitive(root, "stats");
    int version = cJSON_IsObject(legacy) ? 0 : (int)num_of(root, "version");
    if (version > STATS_VERSION && err) snprintf(err, (size_t)err_size, "version %d is newer than %d (unknown fields skipped)", version, STATS_VERSION);
    fields_read(root, ps, k_life, ARRAY_COUNT(k_life));
    const cJSON *lk = cJSON_GetObjectItemCaseSensitive(root, "longest_kill");
    if (cJSON_IsObject(lk)) fields_read(lk, &ps->longest_kill, k_longest, ARRAY_COUNT(k_longest));
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "weapons")) {
        if (ps->weapon_count >= STATS_WEAPON_COUNT) break;
        int i = ps->weapon_count++;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
        dg_strcpy(ps->weapon_ids[i], STATS_WEAPON_ID, cJSON_IsString(id) ? id->valuestring : "");
        fields_read(e, &ps->weapons[i], k_weapon, ARRAY_COUNT(k_weapon));
    }
    // Matches oldest first (only the last STATS_MAX_MATCHES stay); days likewise.
    const cJSON *ma = cJSON_GetObjectItemCaseSensitive(root, "matches");
    int mn = cJSON_GetArraySize(ma), mk = 0;
    cJSON_ArrayForEach(e, ma) {
        if (mk++ < mn - STATS_MAX_MATCHES) continue;
        MatchRecord *m = &ps->matches[ps->match_head];
        memset(m, 0, sizeof(*m));
        fields_read(e, m, k_match, ARRAY_COUNT(k_match));
        ps->match_head = (ps->match_head + 1) % STATS_MAX_MATCHES;
        ps->match_count++;
    }
    const cJSON *da = cJSON_GetObjectItemCaseSensitive(root, "days");
    int dn = cJSON_GetArraySize(da), dk = 0;
    cJSON_ArrayForEach(e, da) {
        if (dk++ < dn - STATS_MAX_DAYS) continue;
        DailyRecord *d = &ps->days[ps->day_head];
        memset(d, 0, sizeof(*d));
        fields_read(e, d, k_day, ARRAY_COUNT(k_day));
        ps->day_head = (ps->day_head + 1) % STATS_MAX_DAYS;
        ps->day_count++;
    }
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (cur && cJSON_IsObject(c)) {
        fields_read(c, cur, k_cur, ARRAY_COUNT(k_cur));
        const cJSON *rec = cJSON_GetObjectItemCaseSensitive(c, "rec");
        if (cJSON_IsObject(rec)) fields_read(rec, &cur->rec, k_match, ARRAY_COUNT(k_match));
    }
    if (version < 1 && cJSON_IsObject(legacy)) {
        ps->kills = (u32)DG_MAX(0.0, num_of(legacy, "kills"));
        ps->deaths = (u32)DG_MAX(0.0, num_of(legacy, "deaths"));
        ps->matches_played = (u32)DG_MAX(0.0, num_of(legacy, "matches"));
        ps->time_played_sec = DG_MAX(0.0, num_of(legacy, "playtime_s"));
    }
    cJSON_Delete(root);
    stats_migrate(ps, version);
    return true;
}

// Struct-level fixes between versions (the keys themselves need none: missing ones read as zero).
void stats_migrate(PlayerStats *ps, int from_version) {
    if (from_version < 1) {
        // Version 0 (profile.json stubs): no weapons, no history; the longest kill has no weapon.
        ps->longest_kill.weapon = -1;
    }
    // Every version: the registry and the rings stay consistent with what was read.
    if (ps->longest_kill.weapon >= ps->weapon_count) ps->longest_kill.weapon = -1;
    ps->version = STATS_VERSION;
    ps->revision++;
}

u32 stats_crc32(const void *data, size_t size) {
    static u32 table[256];
    static bool init;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    u32 crc = 0xFFFFFFFFu;
    const u8 *p = (const u8 *)data;
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
