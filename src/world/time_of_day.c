#include "time_of_day.h"
#include "core/platform.h"
#include "core/log.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_TICK_HZ 60.0
#define D2R (3.14159265358979323846 / 180.0)
#define TWO_PI 6.28318530717958647692

static const char *k_curve_names[TOD_CURVE_COUNT] = { "fog_density", "exposure_comp", "cloud_cover", "star_brightness", "moon_size", "sun_size", "ambient_scale" };
const char *tod_curve_name(int curve) { return curve >= 0 && curve < TOD_CURVE_COUNT ? k_curve_names[curve] : "?"; }

// ------------------------------------------------------------------ config

static void curve_set(TodCurveKeys *k, int n, const float *hv) {
    k->count = n;
    for (int i = 0; i < n; i++) { k->hour[i] = hv[i * 2]; k->value[i] = hv[i * 2 + 1]; }
}

void tod_config_default(TodConfig *c) {
    memset(c, 0, sizeof(*c));
    c->latitude_deg = 47.0f;   // temperate (central Europe)
    c->axial_tilt_deg = 23.44f;
    c->day_of_year = 172;      // late June
    c->day_length_minutes = 24.0f;
    c->start_hours = 9.0f;
    c->moon_age_days = 10.0f;  // waxing gibbous on day 0 (visible in the evening)
    c->sun_illuminance = 3.0f;
    c->moon_illuminance = 0.02f;
    c->night_sky_floor = 0.0008f;
    c->night_floor_ev = -1.5f;
    c->day_max_ev = 7.0f;
    c->cloud_light_loss = 0.7f;
    c->cloud_altitude_m = 1800.0f;
    c->cloud_wind_mps = 8.0f;
    c->cloud_wind_dir_deg = 60.0f;
    static const float fog[] = { 0, 0.0015f, 3.5f, 0.003f, 5.75f, 0.030f, 7.5f, 0.010f, 12, 0.004f, 18, 0.004f, 21, 0.0015f };
    static const float expc[] = { 0, -0.7f, 5.0f, -0.5f, 7.0f, 0.0f, 17.5f, 0.0f, 19.5f, 0.25f, 21.0f, -0.4f };
    static const float cloud[] = { 0, 0.35f, 12, 0.45f, 18, 0.4f };
    static const float stars[] = { 0, 1.0f, 4.5f, 1.0f, 6.0f, 0.0f, 19.5f, 0.0f, 21.5f, 1.0f };
    static const float moon[] = { 0, 1.8f };
    static const float sun[] = { 0, 1.6f };
    static const float amb[] = { 0, 1.0f };   // 1 = physical (docs/SHADOWS.md): sun and sky share one atmosphere
    curve_set(&c->curves[TOD_FOG_DENSITY], 7, fog);
    curve_set(&c->curves[TOD_EXPOSURE_COMP], 6, expc);
    curve_set(&c->curves[TOD_CLOUD_COVER], 3, cloud);
    curve_set(&c->curves[TOD_STAR_BRIGHTNESS], 5, stars);
    curve_set(&c->curves[TOD_MOON_SIZE], 1, moon);
    curve_set(&c->curves[TOD_SUN_SIZE], 1, sun);
    curve_set(&c->curves[TOD_AMBIENT_SCALE], 1, amb);
    c->lamps.on_hour = 18.75f;
    c->lamps.off_hour = 6.5f;
    c->lamps.jitter_minutes = 25.0f;
    c->lamps.flicker = 0.0f;
}

float tod_parse_hours(const char *s) {
    if (!s) return -1.0f;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) >= 1 && h >= 0 && h <= 24 && m >= 0 && m < 60) return (float)h + (float)m / 60.0f;
    return -1.0f;
}

void tod_format_hours(float hours, char *buf, int size) {
    float h = fmodf(hours, 24.0f);
    if (h < 0) h += 24.0f;
    int mins = (int)floorf(h * 60.0f + 1e-3f);
    snprintf(buf, (size_t)size, "%02d:%02d", (mins / 60) % 24, mins % 60);
}

static float j_hours(const cJSON *v, float def) {
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    if (cJSON_IsString(v)) { float h = tod_parse_hours(v->valuestring); return h >= 0 ? h : def; }
    return def;
}
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void parse_block(TodConfig *c, const cJSON *o) {
    if (!o) return;
    c->latitude_deg = jf(o, "latitude", c->latitude_deg);
    c->axial_tilt_deg = jf(o, "axial_tilt", c->axial_tilt_deg);
    c->day_of_year = (int)jf(o, "day_of_year", (float)c->day_of_year);
    c->day_length_minutes = jf(o, "day_length_minutes", c->day_length_minutes);
    c->start_hours = j_hours(cJSON_GetObjectItemCaseSensitive(o, "start_time"), c->start_hours);
    c->moon_age_days = jf(o, "moon_age_days", c->moon_age_days);
    c->sun_illuminance = jf(o, "sun_illuminance", c->sun_illuminance);
    c->moon_illuminance = jf(o, "moon_illuminance", c->moon_illuminance);
    c->night_sky_floor = jf(o, "night_sky_floor", c->night_sky_floor);
    c->cloud_light_loss = jf(o, "cloud_light_loss", c->cloud_light_loss);
    c->cloud_altitude_m = jf(o, "cloud_altitude_m", c->cloud_altitude_m);
    c->cloud_wind_mps = jf(o, "cloud_wind_mps", c->cloud_wind_mps);
    c->cloud_wind_dir_deg = jf(o, "cloud_wind_dir", c->cloud_wind_dir_deg);
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(o, "exposure");
    c->night_floor_ev = jf(e, "night_floor_ev", c->night_floor_ev);
    c->day_max_ev = jf(e, "day_max_ev", c->day_max_ev);
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(o, "light_schedule");
    if (l) {
        c->lamps.on_hour = j_hours(cJSON_GetObjectItemCaseSensitive(l, "on"), c->lamps.on_hour);
        c->lamps.off_hour = j_hours(cJSON_GetObjectItemCaseSensitive(l, "off"), c->lamps.off_hour);
        c->lamps.jitter_minutes = jf(l, "jitter_minutes", c->lamps.jitter_minutes);
        c->lamps.flicker = jf(l, "flicker", c->lamps.flicker);
    }
    const cJSON *cv = cJSON_GetObjectItemCaseSensitive(o, "curves");
    for (int i = 0; i < TOD_CURVE_COUNT && cv; i++) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(cv, k_curve_names[i]);
        if (!cJSON_IsArray(a)) continue;
        TodCurveKeys *k = &c->curves[i];
        k->count = 0;
        const cJSON *key;
        cJSON_ArrayForEach(key, a) {
            if (k->count >= TOD_MAX_CURVE_KEYS || !cJSON_IsArray(key) || cJSON_GetArraySize(key) < 2) continue;
            k->hour[k->count] = j_hours(cJSON_GetArrayItem(key, 0), 0);
            k->value[k->count] = (float)cJSON_GetArrayItem(key, 1)->valuedouble;
            k->count++;
        }
        // Keys sorted by hour (insertion sort, tiny arrays).
        for (int x = 1; x < k->count; x++)
            for (int y = x; y > 0 && k->hour[y] < k->hour[y - 1]; y--) {
                float th = k->hour[y], tv = k->value[y];
                k->hour[y] = k->hour[y - 1]; k->value[y] = k->value[y - 1];
                k->hour[y - 1] = th; k->value[y - 1] = tv;
            }
    }
}

static bool streq_nocase(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

bool tod_config_parse(TodConfig *c, const cJSON *root, const char *level) {
    if (!root) return false;
    parse_block(c, root);
    const cJSON *lv = cJSON_GetObjectItemCaseSensitive(root, "levels");
    if (level && level[0] && lv) {
        // Level names are matched case-insensitively ("DevRoom" / "devroom").
        const cJSON *o;
        cJSON_ArrayForEach(o, lv) {
            if (o->string && streq_nocase(o->string, level)) parse_block(c, o);
        }
    }
    snprintf(c->level, sizeof(c->level), "%s", level ? level : "");
    return true;
}

bool tod_config_load(TodConfig *c, const char *path, const char *level) {
    char buf[512];
    char *text = platform_read_file(platform_data_path(path, buf, sizeof(buf)), NULL);
    if (!text) { log_warn("daynight: cannot read %s (defaults)", path); return false; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { log_error("daynight: JSON parse error in %s", path); return false; }
    tod_config_default(c);
    bool ok = tod_config_parse(c, root, level);
    cJSON_Delete(root);
    return ok;
}

static cJSON *hours_json(float h) {
    char b[16];
    tod_format_hours(h, b, sizeof(b));
    return cJSON_CreateString(b);
}

char *tod_config_to_json(const TodConfig *c) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "_comment", "Day / night cycle (docs/DAYNIGHT.md). Curves: [hour, value] keys, piecewise linear, wrapping at 24 h. Saved by the time-of-day editor (tod_editor).");
    cJSON_AddNumberToObject(o, "latitude", c->latitude_deg);
    cJSON_AddNumberToObject(o, "axial_tilt", c->axial_tilt_deg);
    cJSON_AddNumberToObject(o, "day_of_year", c->day_of_year);
    cJSON_AddNumberToObject(o, "day_length_minutes", c->day_length_minutes);
    cJSON_AddItemToObject(o, "start_time", hours_json(c->start_hours));
    cJSON_AddNumberToObject(o, "moon_age_days", c->moon_age_days);
    cJSON_AddNumberToObject(o, "sun_illuminance", c->sun_illuminance);
    cJSON_AddNumberToObject(o, "moon_illuminance", c->moon_illuminance);
    cJSON_AddNumberToObject(o, "night_sky_floor", c->night_sky_floor);
    cJSON_AddNumberToObject(o, "cloud_light_loss", c->cloud_light_loss);
    cJSON_AddNumberToObject(o, "cloud_altitude_m", c->cloud_altitude_m);
    cJSON_AddNumberToObject(o, "cloud_wind_mps", c->cloud_wind_mps);
    cJSON_AddNumberToObject(o, "cloud_wind_dir", c->cloud_wind_dir_deg);
    cJSON *e = cJSON_AddObjectToObject(o, "exposure");
    cJSON_AddNumberToObject(e, "night_floor_ev", c->night_floor_ev);
    cJSON_AddNumberToObject(e, "day_max_ev", c->day_max_ev);
    cJSON *l = cJSON_AddObjectToObject(o, "light_schedule");
    cJSON_AddItemToObject(l, "on", hours_json(c->lamps.on_hour));
    cJSON_AddItemToObject(l, "off", hours_json(c->lamps.off_hour));
    cJSON_AddNumberToObject(l, "jitter_minutes", c->lamps.jitter_minutes);
    cJSON_AddNumberToObject(l, "flicker", c->lamps.flicker);
    cJSON *cv = cJSON_AddObjectToObject(o, "curves");
    for (int i = 0; i < TOD_CURVE_COUNT; i++) {
        cJSON *a = cJSON_AddArrayToObject(cv, k_curve_names[i]);
        for (int k = 0; k < c->curves[i].count; k++) {
            cJSON *key = cJSON_CreateArray();
            cJSON_AddItemToArray(key, cJSON_CreateNumber(round(c->curves[i].hour[k] * 1000.0) / 1000.0));
            cJSON_AddItemToArray(key, cJSON_CreateNumber(round(c->curves[i].value[k] * 10000.0) / 10000.0));
            cJSON_AddItemToArray(a, key);
        }
    }
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    return s;
}

float tod_curve(const TodConfig *c, TodCurve curve, float hours) {
    const TodCurveKeys *k = &c->curves[curve];
    if (k->count <= 0) return 0.0f;
    if (k->count == 1) return k->value[0];
    float h = fmodf(hours, 24.0f);
    if (h < 0) h += 24.0f;
    // Wrap: between the last key and the first key + 24 h.
    for (int i = 0; i < k->count; i++) {
        float h0 = k->hour[i], v0 = k->value[i];
        float h1 = i + 1 < k->count ? k->hour[i + 1] : k->hour[0] + 24.0f;
        float v1 = i + 1 < k->count ? k->value[i + 1] : k->value[0];
        float hh = h < k->hour[0] ? h + 24.0f : h;
        if (hh >= h0 && hh < h1) return v0 + (v1 - v0) * (hh - h0) / fmaxf(h1 - h0, 1e-4f);
    }
    return k->value[k->count - 1];
}

// ------------------------------------------------------------------ state

void tod_state_init(TimeState *s, const TodConfig *c, u32 tick) {
    memset(s, 0, sizeof(*s));
    s->ref_tick = tick;
    float h = fmodf(fmaxf(c->start_hours, 0.0f), 24.0f);
    s->ref_ms = (u32)lrint((double)h / 24.0 * TOD_MS_PER_DAY) % TOD_MS_PER_DAY;
    s->ref_day = 0;
    s->day_of_year = (u16)DG_CLAMP(c->day_of_year, 1, 365);
    s->day_length = fmaxf(0.0f, c->day_length_minutes * 60.0f);
    s->version = 1;
}

double tod_days(const TimeState *s, double tick) {
    double d = (double)s->ref_day + (double)s->ref_ms / (double)TOD_MS_PER_DAY;
    if (s->day_length > 0.0f) d += (tick - (double)s->ref_tick) / SIM_TICK_HZ / (double)s->day_length;
    return d < 0 ? 0 : d;
}

float tod_hours(const TimeState *s, double tick) {
    double d = tod_days(s, tick);
    return (float)((d - floor(d)) * 24.0);
}

static void anchor(TimeState *s, double tick, double days) {
    if (days < 0) days = 0;
    double whole = floor(days);
    s->ref_tick = (u32)(tick < 0 ? 0 : tick);
    s->ref_day = (u16)DG_MIN(whole, 65535.0);
    s->ref_ms = (u32)lrint((days - whole) * TOD_MS_PER_DAY) % TOD_MS_PER_DAY;
    s->version++;
}

void tod_set_time(TimeState *s, double tick, float hours) {
    double d = floor(tod_days(s, tick));
    anchor(s, tick, d + fmod(fmax(hours, 0.0), 24.0) / 24.0);
}

void tod_set_day_length(TimeState *s, double tick, float seconds) {
    anchor(s, tick, tod_days(s, tick));
    s->day_length = fmaxf(seconds, 0.0f);
}

void tod_add_days(TimeState *s, double tick, int days) { anchor(s, tick, tod_days(s, tick) + days); }

void tod_pack(const TimeState *s, u8 out[20]) {
    memcpy(out + 0, &s->ref_tick, 4);
    memcpy(out + 4, &s->ref_ms, 4);
    memcpy(out + 8, &s->ref_day, 2);
    memcpy(out + 10, &s->day_of_year, 2);
    memcpy(out + 12, &s->day_length, 4);
    memcpy(out + 16, &s->version, 2);
    memset(out + 18, 0, 2);
}

void tod_unpack(TimeState *s, const u8 in[20]) {
    memset(s, 0, sizeof(*s));
    memcpy(&s->ref_tick, in + 0, 4);
    memcpy(&s->ref_ms, in + 4, 4);
    memcpy(&s->ref_day, in + 8, 2);
    memcpy(&s->day_of_year, in + 10, 2);
    memcpy(&s->day_length, in + 12, 4);
    memcpy(&s->version, in + 16, 2);
}

// ------------------------------------------------------------------ astronomy

// Sun ecliptic longitude for a continuous calendar day (1 = Jan 1 00:00): mean anomaly from the
// perihelion (~Jan 3.5), equation of centre, longitude of perihelion 282.94 deg (a few hundredths
// of a degree against the NOAA algorithm, which is plenty for the sky).
static double sun_longitude(double doy) {
    double M = TWO_PI * (doy - 3.5) / 365.2422;
    double C = (1.9148 * sin(M) + 0.0200 * sin(2.0 * M) + 0.0003 * sin(3.0 * M)) * D2R;
    return M + C + 282.9372 * D2R;
}

static void ecl_to_eq(double lambda, double eps, double *ra, double *dec) {
    *dec = asin(sin(eps) * sin(lambda));
    *ra = atan2(cos(eps) * sin(lambda), cos(lambda));
}

// (hour angle, declination) -> world direction (Y up, -Z north, +X east).
static vec3 enu_dir(double lat, double H, double dec) {
    double e = -cos(dec) * sin(H);
    double n = cos(lat) * sin(dec) - sin(lat) * cos(dec) * cos(H);
    double u = sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(H);
    return v3((float)e, (float)u, (float)-n);
}

void tod_sun_angles(float latitude_deg, float tilt_deg, double day_of_year, float hours, float *elev_deg, float *azim_deg) {
    double ra, dec;
    ecl_to_eq(sun_longitude(day_of_year + hours / 24.0), tilt_deg * D2R, &ra, &dec);
    double H = (hours - 12.0) * 15.0 * D2R;
    vec3 d = enu_dir(latitude_deg * D2R, H, dec);
    *elev_deg = (float)(asin(fmax(-1.0, fmin(1.0, d.y))) / D2R);
    double az = atan2(d.x, -d.z) / D2R;
    if (az < 0) az += 360.0;
    *azim_deg = (float)az;
}

void tod_sky(const TodConfig *c, double days, TodSky *out) {
    memset(out, 0, sizeof(*out));
    out->days = days;
    double frac = days - floor(days);
    out->hours = (float)(frac * 24.0);
    double lat = c->latitude_deg * D2R, eps = c->axial_tilt_deg * D2R;
    double doy = (double)DG_CLAMP(c->day_of_year, 1, 365) + days;
    doy = fmod(doy - 1.0, 365.2422) + 1.0;
    double ra_s, dec_s;
    double lam_s = sun_longitude(doy);
    ecl_to_eq(lam_s, eps, &ra_s, &dec_s);
    double H_s = (out->hours - 12.0) * 15.0 * D2R;
    out->sun_dir = v3_norm(enu_dir(lat, H_s, dec_s));
    out->declination = (float)dec_s;
    out->sun_elev = asinf(clampf(out->sun_dir.y, -1.0f, 1.0f));
    out->sun_azim = atan2f(out->sun_dir.x, -out->sun_dir.z);
    if (out->sun_azim < 0) out->sun_azim += 2.0f * DG_PI;
    // Moon: elongation from the lunar age; on the ecliptic (the 5 deg inclination is ignored).
    double age = fmod(c->moon_age_days + days, TOD_SYNODIC_MONTH);
    if (age < 0) age += TOD_SYNODIC_MONTH;
    double E = TWO_PI * age / TOD_SYNODIC_MONTH;
    double ra_m, dec_m;
    ecl_to_eq(lam_s + E, eps, &ra_m, &dec_m);
    double H_m = H_s - (ra_m - ra_s);
    out->moon_dir = v3_norm(enu_dir(lat, H_m, dec_m));
    out->moon_elev = asinf(clampf(out->moon_dir.y, -1.0f, 1.0f));
    out->moon_age = (float)age;
    out->moon_phase = (float)(age / TOD_SYNODIC_MONTH);
    out->moon_illum = (float)((1.0 - cos(E)) * 0.5);
    out->waxing = E < 3.14159265358979323846;
    // Celestial frame: local sidereal time = hour angle of the sun + its right ascension.
    double lst = H_s + ra_s;
    out->cel_x = v3_norm(enu_dir(lat, lst, 0.0));
    out->cel_y = v3_norm(enu_dir(lat, lst - 0.5 * 3.14159265358979323846, 0.0));
    out->cel_z = v3_norm(enu_dir(lat, 0.0, 0.5 * 3.14159265358979323846));
}

bool tod_is_night(const TodSky *s) { return s->sun_elev < -6.0f * DG_DEG2RAD; }

static float sun_elev_at(const TodConfig *c, double days) {
    TodSky s;
    tod_sky(c, days, &s);
    return s.sun_elev * DG_RAD2DEG;
}

u32 tod_events_between(const TodConfig *c, double a, double b) {
    if (b <= a) return 0;
    u32 ev = 0;
    if (floor(b) > floor(a)) ev |= TOD_EV_MIDNIGHT;
    if (floor(b - 0.5) > floor(a - 0.5)) ev |= TOD_EV_NOON;
    // Sunrise / sunset: the upper limb crosses the horizon (refraction included, -0.833 deg).
    // Long steps (time_days) only report the day boundaries.
    if (b - a < 0.25) {
        float ea = sun_elev_at(c, a), eb = sun_elev_at(c, b);
        if (ea < -0.833f && eb >= -0.833f) ev |= TOD_EV_SUNRISE;
        if (ea >= -0.833f && eb < -0.833f) ev |= TOD_EV_SUNSET;
    }
    return ev;
}

const char *tod_event_name(u32 ev) {
    if (ev & TOD_EV_SUNRISE) return "sunrise";
    if (ev & TOD_EV_SUNSET) return "sunset";
    if (ev & TOD_EV_MIDNIGHT) return "midnight";
    if (ev & TOD_EV_NOON) return "noon";
    return "?";
}

static u32 hash_u32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

bool tod_light_on(const TodLightSchedule *ls, float hours, u32 id) {
    float off = ((float)(hash_u32(id * 0x9E3779B1u + 17u) & 0xFFFF) / 65535.0f * 2.0f - 1.0f) * ls->jitter_minutes / 60.0f;
    float on_h = fmodf(ls->on_hour + off + 48.0f, 24.0f), off_h = fmodf(ls->off_hour + off * 0.5f + 48.0f, 24.0f);
    float h = fmodf(hours + 48.0f, 24.0f);
    if (on_h < off_h) return h >= on_h && h < off_h;
    return h >= on_h || h < off_h;
}
