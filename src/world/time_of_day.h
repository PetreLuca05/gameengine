// Time of day (docs/DAYNIGHT.md): host-authoritative world time, the sun / moon / star sky
// positions for a latitude and date, time-of-day curves, events and light schedules.
//
// Pure module: no engine state. The host owns a TimeState (World.tod) and re-anchors it on every
// change (time_set / time_scale / time_pause / time_days); clients receive it in every snapshot
// and evaluate it at their interpolated server tick, so every machine shows the same sky.
//
//   time (days) = ref_day + ref_ms / 86 400 000 + (tick - ref_tick) / SIM_HZ / day_length
//
// Conventions: world axes Y up, -Z north, +X east. Local apparent solar time (no longitude /
// time zone / equation of time): 12:00 is solar noon. Angles in radians unless named _deg.
#ifndef DG_TIME_OF_DAY_H
#define DG_TIME_OF_DAY_H

#include "core/base.h"
#include "core/dgmath.h"

struct cJSON;

#define TOD_MS_PER_DAY 86400000u
#define TOD_MAX_CURVE_KEYS 24
#define TOD_SYNODIC_MONTH 29.530589

// Replicated world-state field (snapshot header, 20 bytes on the wire).
typedef struct TimeState {
    u32 ref_tick;          // server tick of the anchor
    u32 ref_ms;            // time of day at the anchor (ms since midnight)
    u16 ref_day;           // day number at the anchor (0 = first day)
    u16 day_of_year;       // calendar day of day 0 (1..365)
    float day_length;      // real seconds per in-game day (0 = frozen / paused)
    u16 version;           // bumped on every change
    u16 pad;
} TimeState;

typedef enum TodCurve {
    TOD_FOG_DENSITY, TOD_EXPOSURE_COMP, TOD_CLOUD_COVER, TOD_STAR_BRIGHTNESS, TOD_MOON_SIZE, TOD_SUN_SIZE,
    TOD_AMBIENT_SCALE, TOD_CURVE_COUNT
} TodCurve;

typedef struct TodCurveKeys { float hour[TOD_MAX_CURVE_KEYS], value[TOD_MAX_CURVE_KEYS]; int count; } TodCurveKeys;

typedef struct TodLightSchedule {
    float on_hour, off_hour;     // lamps on in [on, off) (wraps over midnight)
    float jitter_minutes;        // per-light random offset (deterministic from the light id)
    float flicker;               // default flicker 0..1 (cosmetic)
} TodLightSchedule;

typedef struct TodConfig {
    float latitude_deg, axial_tilt_deg;
    int day_of_year;             // calendar day of day 0
    float day_length_minutes;    // real minutes per in-game day (0 = frozen)
    float start_hours;
    float moon_age_days;         // lunar age on day 0 (0 = new moon, 14.77 = full)
    // Lighting (engine units: the lit shaders' sun illuminance is ~2.4 at noon)
    float sun_illuminance;       // above the atmosphere
    float moon_illuminance;      // full moon, above the atmosphere
    float night_sky_floor;       // airglow / starlight sky radiance floor
    float night_floor_ev;        // auto exposure never brightens past this EV100 (night stays night)
    float day_max_ev;
    float cloud_light_loss;      // sun / moon intensity lost under full cloud cover (0..1)
    float cloud_altitude_m, cloud_wind_mps, cloud_wind_dir_deg;
    TodCurveKeys curves[TOD_CURVE_COUNT];
    TodLightSchedule lamps;
    char level[32];              // level the overrides were applied for
} TodConfig;

// Celestial state at a time.
typedef struct TodSky {
    double days;                 // continuous day count (fraction = time of day)
    float hours;                 // 0..24
    vec3 sun_dir, moon_dir;      // towards the body (world)
    float sun_elev, sun_azim;    // radians; azimuth from north, clockwise (east = +90 deg)
    float moon_elev;
    float declination;
    float moon_age;              // days since new moon
    float moon_phase;            // 0..1 (0 new, 0.5 full)
    float moon_illum;            // illuminated fraction 0..1
    bool waxing;
    vec3 cel_x, cel_y, cel_z;    // world -> celestial (equatorial) rows: RA 0 h, RA 6 h, north celestial pole
} TodSky;

typedef enum TodEvent { TOD_EV_SUNRISE = 1, TOD_EV_SUNSET = 2, TOD_EV_MIDNIGHT = 4, TOD_EV_NOON = 8 } TodEvent;

// Config: defaults, JSON (assets/world/daynight.json), per-level overrides ("levels": {"name": {...}}).
void tod_config_default(TodConfig *c);
bool tod_config_parse(TodConfig *c, const struct cJSON *root, const char *level);
bool tod_config_load(TodConfig *c, const char *path, const char *level);
char *tod_config_to_json(const TodConfig *c);   // caller frees (cJSON_free)
float tod_curve(const TodConfig *c, TodCurve curve, float hours);   // piecewise linear, wraps at 24 h
const char *tod_curve_name(int curve);

// State (host).
void tod_state_init(TimeState *s, const TodConfig *c, u32 tick);
double tod_days(const TimeState *s, double tick);                  // continuous day count
float tod_hours(const TimeState *s, double tick);
void tod_set_time(TimeState *s, double tick, float hours);          // keeps the day number
void tod_set_day_length(TimeState *s, double tick, float seconds);  // 0 = pause
void tod_add_days(TimeState *s, double tick, int days);
// Wire format (20 bytes).
void tod_pack(const TimeState *s, u8 out[20]);
void tod_unpack(TimeState *s, const u8 in[20]);

// Sky positions for a time.
void tod_sky(const TodConfig *c, double days, TodSky *out);
// Sun elevation / azimuth (degrees) for latitude, calendar day and solar time (tests, tools).
void tod_sun_angles(float latitude_deg, float tilt_deg, double day_of_year, float hours, float *elev_deg, float *azim_deg);
// Events crossed between two times (TodEvent bits).
u32 tod_events_between(const TodConfig *c, double days_a, double days_b);
const char *tod_event_name(u32 ev);
bool tod_is_night(const TodSky *s);          // sun below -6 deg (civil twilight over)
// Light schedule: on at `hours` for light `id` (stable random offset from the id)?
bool tod_light_on(const TodLightSchedule *ls, float hours, u32 id);
// "HH:MM" -> hours (-1 if invalid).
float tod_parse_hours(const char *s);
void tod_format_hours(float hours, char *buf, int size);

#endif
