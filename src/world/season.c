// Seasons (src/world/season.h).
#include "season.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_ids[SEASON_COUNT] = { "spring", "summer", "autumn", "winter" };

const char *season_id(int s) { return s >= 0 && s < SEASON_COUNT ? k_ids[s] : "?"; }

int season_from_id(const char *id) {
    for (int i = 0; i < SEASON_COUNT; i++) if (id && !strcmp(id, k_ids[i])) return i;
    return -1;
}

static void def(SeasonDef *s, const char *id, const char *name, int doy, float temp, float snow, float wet, const char *const *w, const float *k, int n) {
    memset(s, 0, sizeof(*s));
    dg_strcpy(s->id, sizeof(s->id), id);
    dg_strcpy(s->name, sizeof(s->name), name);
    s->day_of_year = doy;
    s->temp_offset = temp;
    s->snow = snow;
    s->wetness = wet;
    for (int i = 0; i < n && i < SEASON_MAX_WEATHER; i++) { dg_strcpy(s->weather[i], sizeof(s->weather[0]), w[i]); s->weight[i] = k[i]; }
    s->weather_count = DG_MIN(n, SEASON_MAX_WEATHER);
}

// Built-in values (= assets/world/seasons.json as shipped), so a missing file still gives four seasons.
void seasons_defaults(SeasonTable *t) {
    static const char *const sp[] = { "clear", "little_cloudy", "cloudy", "overcast", "light_rain", "rain", "fog" };
    static const float spw[] = { 2, 3, 2, 1.5f, 2, 1, 1 };
    static const char *const su[] = { "clear", "little_cloudy", "cloudy", "light_rain", "rain", "storm" };
    static const float suw[] = { 4, 3, 1.5f, 0.5f, 0.4f, 0.6f };
    static const char *const au[] = { "clear", "little_cloudy", "cloudy", "overcast", "fog", "light_rain", "rain", "storm" };
    static const float auw[] = { 0.3f, 0.5f, 1.5f, 2, 1.5f, 3, 3, 0.5f };
    static const char *const wi[] = { "clear", "little_cloudy", "cloudy", "overcast", "fog", "light_snow", "snow", "blizzard" };
    static const float wiw[] = { 0.4f, 0.4f, 0.6f, 1.5f, 0.8f, 3, 3, 0.6f };
    def(&t->s[SEASON_SPRING], "spring", "Spring", 110, -2, 0, 0.35f, sp, spw, ARRAY_COUNT(sp));
    def(&t->s[SEASON_SUMMER], "summer", "Summer", 196, 8, 0, 0, su, suw, ARRAY_COUNT(su));
    def(&t->s[SEASON_AUTUMN], "autumn", "Autumn", 288, -4, 0, 0.5f, au, auw, ARRAY_COUNT(au));
    def(&t->s[SEASON_WINTER], "winter", "Winter", 15, -14, 0.7f, 0.1f, wi, wiw, ARRAY_COUNT(wi));
}

bool seasons_parse(SeasonTable *t, const char *json, char *err, int err_size) {
    if (err && err_size) err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "JSON syntax error"); return false; }
    SeasonTable out = *t;
    const char *bad = NULL;
    char where[24] = "";
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "seasons")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "id");
        int k = cJSON_IsString(v) ? season_from_id(v->valuestring) : -1;
        if (k < 0) { bad = "unknown \"id\" (spring, summer, autumn, winter)"; break; }
        SeasonDef *s = &out.s[k];
        dg_strcpy(where, sizeof(where), s->id);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "name"))) dg_strcpy(s->name, sizeof(s->name), v->valuestring);
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "day_of_year"))) s->day_of_year = v->valueint;
        if (s->day_of_year < 1 || s->day_of_year > 365) { bad = "\"day_of_year\" must be 1..365"; break; }
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "temperature_offset"))) s->temp_offset = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "snow"))) s->snow = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "wetness"))) s->wetness = (float)v->valuedouble;
        if (s->snow < 0 || s->snow > 1 || s->wetness < 0 || s->wetness > 1) { bad = "\"snow\" / \"wetness\" must be 0..1"; break; }
        const cJSON *wx = cJSON_GetObjectItemCaseSensitive(e, "weather");
        if (wx) {
            if (!cJSON_IsObject(wx)) { bad = "\"weather\" must be { \"preset\": weight }"; break; }
            s->weather_count = 0;
            const cJSON *p;
            cJSON_ArrayForEach(p, wx) {
                if (!cJSON_IsNumber(p) || p->valuedouble < 0) { bad = "weather weights must be numbers >= 0"; break; }
                if (s->weather_count >= SEASON_MAX_WEATHER) { bad = "too many weather presets"; break; }
                dg_strcpy(s->weather[s->weather_count], sizeof(s->weather[0]), p->string);
                s->weight[s->weather_count++] = (float)p->valuedouble;
            }
            if (bad) break;
            if (!s->weather_count) { bad = "\"weather\" lists no preset"; break; }
        }
    }
    cJSON_Delete(root);
    if (bad) { if (err) snprintf(err, (size_t)err_size, "%s%s%s", where, where[0] ? ": " : "", bad); return false; }
    *t = out;
    return true;
}

bool seasons_load(SeasonTable *t, const char *path) {
    seasons_defaults(t);
    char buf[512];
    char *text = platform_read_file(platform_data_path(path, buf, sizeof(buf)), NULL);
    if (!text) { log_warn("seasons: %s missing (built-in seasons)", path); return false; }
    char err[160];
    bool ok = seasons_parse(t, text, err, sizeof(err));
    free(text);
    if (!ok) log_error("seasons: %s: %s (built-in seasons)", path, err);
    return ok;
}

bool season_allows(const SeasonDef *s, const char *preset) {
    for (int i = 0; i < s->weather_count; i++) if (s->weight[i] > 0 && !strcmp(s->weather[i], preset)) return true;
    return false;
}

void season_weather_bias(const SeasonDef *s, const WeatherLib *lib, float *out) {
    for (int i = 0; i < WEATHER_MAX_PRESETS; i++) out[i] = 0.0f;
    for (int i = 0; i < s->weather_count; i++) {
        int k = weather_find_preset(lib, s->weather[i]);
        if (k >= 0 && k < WEATHER_MAX_PRESETS) out[k] = s->weight[i];
    }
}

const char *season_pick_weather(const SeasonDef *s, u32 seed) {
    float total = 0;
    for (int i = 0; i < s->weather_count; i++) total += s->weight[i];
    if (total <= 0) return "";
    float x = (float)(dg_hash_u32(seed ^ 0x5EA50Au) & 0xFFFFFF) / 16777216.0f * total;
    for (int i = 0; i < s->weather_count; i++) {
        if (s->weight[i] <= 0) continue;
        if (x < s->weight[i]) return s->weather[i];
        x -= s->weight[i];
    }
    for (int i = s->weather_count - 1; i >= 0; i--) if (s->weight[i] > 0) return s->weather[i];
    return "";
}
