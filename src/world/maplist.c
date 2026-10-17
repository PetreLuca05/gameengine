// Map catalogue (src/world/maplist.h).
#include "maplist.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool get_s(const cJSON *o, const char *k, char *dst, size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(v)) return false;
    dg_strcpy(dst, n, v->valuestring);
    return true;
}

static bool get_f(const cJSON *o, const char *k, float *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return false;
    *dst = (float)v->valuedouble;
    return true;
}

bool maplist_parse(const char *json, const char *id, MapInfo *out, MapFileExists file_exists, void *user, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
    if (err && err_size) err[0] = 0;
    dg_strcpy(out->id, sizeof(out->id), id);
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "%s: JSON syntax error", id); return false; }
    const char *bad = NULL;
    if (!id[0]) bad = "empty id";
    else if (!get_s(root, "name", out->name, sizeof(out->name)) || !out->name[0]) bad = "\"name\" missing";
    else if (!get_s(root, "level", out->level, sizeof(out->level)) || !out->level[0]) bad = "\"level\" missing";
    else if (!get_s(root, "thumbnail", out->thumbnail, sizeof(out->thumbnail)) || !out->thumbnail[0]) bad = "\"thumbnail\" missing";
    else if (!get_s(root, "biome", out->biome, sizeof(out->biome))) bad = "\"biome\" missing";
    else if (!get_s(root, "description", out->description, sizeof(out->description))) bad = "\"description\" missing";
    if (!bad) {
        out->hour = 9.0f;
        get_f(root, "time_of_day", &out->hour);
        if (out->hour < 0 || out->hour > 24) bad = "\"time_of_day\" must be 0..24";
        get_s(root, "weather", out->weather, sizeof(out->weather));
        char season[16] = "summer";
        get_s(root, "season", season, sizeof(season));
        static const char *const seasons[4] = { "spring", "summer", "autumn", "winter" };
        out->season = -1;
        for (int k = 0; k < 4; k++) if (!strcmp(season, seasons[k])) out->season = k;
        if (out->season < 0) bad = "\"season\" must be spring, summer, autumn or winter";
        get_f(root, "size_m", &out->size_m);
        if (out->size_m < 0) bad = "\"size_m\" must be >= 0 (0 = unbounded)";
        float pmin = 1, pmax = 4, order = 100;
        if (bad) { cJSON_Delete(root); if (err) snprintf(err, (size_t)err_size, "%s: %s", id, bad); return false; }
        const cJSON *rp = cJSON_GetObjectItemCaseSensitive(root, "players");
        if (cJSON_IsArray(rp) && cJSON_GetArraySize(rp) == 2) {
            pmin = (float)cJSON_GetArrayItem(rp, 0)->valuedouble;
            pmax = (float)cJSON_GetArrayItem(rp, 1)->valuedouble;
        } else if (rp) bad = "\"players\" must be [min, max]";
        out->players_min = (int)pmin;
        out->players_max = (int)pmax;
        if (!bad && (out->players_min < 1 || out->players_max < out->players_min || out->players_max > 8)) bad = "\"players\" must be 1 <= min <= max <= 8";
        get_f(root, "order", &order);
        out->order = (int)order;
        out->dev = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "dev"));
        get_s(root, "scene", out->scene, sizeof(out->scene));
    }
    if (!bad && file_exists && !file_exists(out->thumbnail, user)) bad = "thumbnail file missing";
    cJSON_Delete(root);
    if (bad) { if (err) snprintf(err, (size_t)err_size, "%s: %s", id, bad); return false; }
    return true;
}

static bool exists_cb(const char *path, void *user) {
    (void)user;
    char buf[512];
    return platform_file_exists(platform_data_path(path, buf, sizeof(buf)));
}

static int cmp_map(const void *a, const void *b) {
    const MapInfo *x = (const MapInfo *)a, *y = (const MapInfo *)b;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return strcmp(x->name, y->name);
}

int maplist_load(MapList *ml, const char *dir_rel) {
    memset(ml, 0, sizeof(*ml));
    char dir[512];
    platform_data_path(dir_rel, dir, sizeof(dir));
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    for (int i = 0; files && i < count && ml->count < MAPLIST_MAX; i++) {
        char rel[256], buf[512], id[32];
        snprintf(rel, sizeof(rel), "%s/%s", dir_rel, files[i]);
        dg_strcpy(id, sizeof(id), files[i]);
        char *dot = strrchr(id, '.');
        if (dot) *dot = 0;
        char *text = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
        if (!text) continue;
        char err[160];
        if (maplist_parse(text, id, &ml->maps[ml->count], exists_cb, NULL, err, sizeof(err))) ml->count++;
        else log_error("maps: %s", err);
        free(text);
    }
    SDL_free(files);
    qsort(ml->maps, (size_t)ml->count, sizeof(MapInfo), cmp_map);
    log_info("maps: %d in %s", ml->count, dir_rel);
    return ml->count;
}

int maplist_index(const MapList *ml, const char *id) {
    for (int i = 0; i < ml->count; i++) if (!strcmp(ml->maps[i].id, id)) return i;
    return -1;
}

const MapInfo *maplist_find(const MapList *ml, const char *id) {
    int i = maplist_index(ml, id);
    return i >= 0 ? &ml->maps[i] : NULL;
}

void maplist_size_text(const MapInfo *m, char *out, int size) {
    if (m->size_m <= 0) snprintf(out, (size_t)size, "Unbounded");
    else if (m->size_m >= 1000) snprintf(out, (size_t)size, "%g km x %g km", m->size_m / 1000.0f, m->size_m / 1000.0f);
    else snprintf(out, (size_t)size, "%.0f m x %.0f m", m->size_m, m->size_m);
}

u32 maplist_hash(const MapList *ml) {
    u32 h = 0x9E3779B9u;
    for (int i = 0; i < ml->count; i++) h = dg_hash_u32(h ^ dg_hash_str(ml->maps[i].id) ^ (dg_hash_str(ml->maps[i].level) * 31u) ^ (dg_hash_str(ml->maps[i].scene) * 131u));
    return h;
}
