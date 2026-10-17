// Player profile (src/game/profile.h).
#include "profile.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void profile_default(Profile *p) {
    memset(p, 0, sizeof(*p));
    p->version = PROFILE_VERSION;
    dg_strcpy(p->lan_name, sizeof(p->lan_name), "Survivor");
    dg_strcpy(p->last_map, sizeof(p->last_map), "devroom");
    lobby_settings_default(&p->lobby);
}

char *profile_to_json(const Profile *p) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", PROFILE_VERSION);
    cJSON_AddStringToObject(root, "lan_name", p->lan_name);
    cJSON_AddStringToObject(root, "last_map", p->last_map);
    cJSON_AddStringToObject(root, "last_session", p->last_session);
    cJSON *eq = cJSON_AddObjectToObject(root, "equipped");
    for (int s = 0; s < COS_SLOTS; s++)
        if (p->loadout.equipped[s][0]) cJSON_AddStringToObject(eq, s == COS_SLOT_WEAPON ? "weapon_skin" : char_slot_name(s), p->loadout.equipped[s]);
    cJSON *un = cJSON_AddArrayToObject(root, "unlocked");
    for (int i = 0; i < p->unlocks.count; i++) cJSON_AddItemToArray(un, cJSON_CreateString(p->unlocks.ids[i]));
    cJSON *lb = cJSON_AddObjectToObject(root, "lobby");
    cJSON_AddStringToObject(lb, "map", p->lobby.map);
    cJSON_AddNumberToObject(lb, "difficulty", p->lobby.difficulty);
    cJSON_AddNumberToObject(lb, "max_players", p->lobby.max_players);
    cJSON_AddNumberToObject(lb, "privacy", p->lobby.privacy);
    cJSON_AddNumberToObject(lb, "start_hour", p->lobby.start_hour);
    cJSON_AddNumberToObject(lb, "season", p->lobby.season);
    cJSON_AddBoolToObject(lb, "dynamic_weather", p->lobby.dynamic_weather);
    cJSON_AddStringToObject(lb, "weather", p->lobby.weather);
    cJSON_AddBoolToObject(lb, "pvp", p->lobby.pvp);
    cJSON_AddBoolToObject(lb, "late_join", p->lobby.late_join);
    cJSON *st = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(st, "kills", p->kills);
    cJSON_AddNumberToObject(st, "deaths", p->deaths);
    cJSON_AddNumberToObject(st, "matches", p->matches);
    cJSON_AddNumberToObject(st, "playtime_s", p->playtime_s);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

static void str_of(const cJSON *o, const char *k, char *dst, size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) dg_strcpy(dst, n, v->valuestring);
}

static double num_of(const cJSON *o, const char *k, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

bool profile_from_json(Profile *p, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    p->version = (int)num_of(root, "version", PROFILE_VERSION);
    str_of(root, "lan_name", p->lan_name, sizeof(p->lan_name));
    str_of(root, "last_map", p->last_map, sizeof(p->last_map));
    str_of(root, "last_session", p->last_session, sizeof(p->last_session));
    const cJSON *eq = cJSON_GetObjectItemCaseSensitive(root, "equipped");
    if (cJSON_IsObject(eq)) {
        memset(&p->loadout, 0, sizeof(p->loadout));
        const cJSON *e;
        cJSON_ArrayForEach(e, eq) {
            int s = !strcmp(e->string, "weapon_skin") ? COS_SLOT_WEAPON : char_slot_from_name(e->string);
            if (s >= 0 && cJSON_IsString(e)) dg_strcpy(p->loadout.equipped[s], sizeof(p->loadout.equipped[0]), e->valuestring);
        }
    }
    const cJSON *un = cJSON_GetObjectItemCaseSensitive(root, "unlocked");
    if (cJSON_IsArray(un)) {
        p->unlocks.count = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, un) if (cJSON_IsString(e) && p->unlocks.count < COS_MAX_UNLOCKS) dg_strcpy(p->unlocks.ids[p->unlocks.count++], sizeof(p->unlocks.ids[0]), e->valuestring);
    }
    const cJSON *lb = cJSON_GetObjectItemCaseSensitive(root, "lobby");
    if (cJSON_IsObject(lb)) {
        str_of(lb, "map", p->lobby.map, sizeof(p->lobby.map));
        p->lobby.difficulty = (u8)DG_CLAMP((int)num_of(lb, "difficulty", p->lobby.difficulty), 0, LD_COUNT - 1);
        p->lobby.max_players = (u8)DG_CLAMP((int)num_of(lb, "max_players", p->lobby.max_players), 1, LOBBY_MAX_MEMBERS);
        p->lobby.privacy = (u8)DG_CLAMP((int)num_of(lb, "privacy", p->lobby.privacy), 0, LP_COUNT - 1);
        p->lobby.start_hour = (u8)DG_CLAMP((int)num_of(lb, "start_hour", p->lobby.start_hour), 0, 23);
        p->lobby.season = (u8)DG_CLAMP((int)num_of(lb, "season", p->lobby.season), 0, 3);
        str_of(lb, "weather", p->lobby.weather, sizeof(p->lobby.weather));
        const cJSON *b;
        if (cJSON_IsBool(b = cJSON_GetObjectItemCaseSensitive(lb, "pvp"))) p->lobby.pvp = cJSON_IsTrue(b);
        if (cJSON_IsBool(b = cJSON_GetObjectItemCaseSensitive(lb, "late_join"))) p->lobby.late_join = cJSON_IsTrue(b);
        if (cJSON_IsBool(b = cJSON_GetObjectItemCaseSensitive(lb, "dynamic_weather"))) p->lobby.dynamic_weather = cJSON_IsTrue(b);
    }
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "stats");
    if (cJSON_IsObject(st)) {
        p->kills = (u32)num_of(st, "kills", p->kills);
        p->deaths = (u32)num_of(st, "deaths", p->deaths);
        p->matches = (u32)num_of(st, "matches", p->matches);
        p->playtime_s = num_of(st, "playtime_s", p->playtime_s);
    }
    cJSON_Delete(root);
    return true;
}

const char *profile_path(void) {
    static char path[512];
    snprintf(path, sizeof(path), "%ssaves/profile.json", platform_pref_dir());
    return path;
}

bool profile_load(Profile *p) {
    profile_default(p);
    char *text = platform_read_file(profile_path(), NULL);
    if (!text) return false;
    bool ok = profile_from_json(p, text);
    free(text);
    if (!ok) log_warn("profile: %s is not valid JSON (defaults used)", profile_path());
    return ok;
}

bool profile_save(const Profile *p) {
    if (!config_enabled()) return false;   // --test runs never touch the user's files
    char dir[512];
    snprintf(dir, sizeof(dir), "%ssaves", platform_pref_dir());
    SDL_CreateDirectory(dir);
    char *text = profile_to_json(p);
    if (!text) return false;
    bool ok = platform_write_file(profile_path(), text, strlen(text));
    cJSON_free(text);
    if (!ok) log_warn("profile: cannot write %s", profile_path());
    return ok;
}
