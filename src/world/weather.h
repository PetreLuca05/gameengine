// Dynamic weather (docs/WEATHER.md): a set of continuous parameters, never an enum, so any state
// blends into any other. The host owns a WeatherState (current "from" set, "to" set, the start
// tick and the transition time) and replicates it (rarely: on changes and every few snapshots);
// every machine evaluates the same blend from the server tick, so everyone sees the same sky.
//
//   weather_eval      parameters at a tick (per-parameter durations and curves)
//   weather_integrate wetness / puddles / snow cover accumulate and dry (with their own rates)
//   weather_lightning deterministic strikes from (seed, tick, lightning rate)
//   presets           assets/world/weather/*.json parameter sets ("overcast+rain" composes)
//   scheduler         Markov-style random progression (host)
//
// Pure: no world, no renderer (tests/test_weather.c). The game glue (replication, queries, the
// renderer / audio hooks, console) is src/game/game_weather.c.
#ifndef DG_WEATHER_H
#define DG_WEATHER_H

#include "core/base.h"
#include "core/dgmath.h"

typedef enum WeatherParam {
    WP_CLOUD_COVER,      // 0..1
    WP_CLOUD_DENSITY,    // 0..1 (opacity / darkness of the cloud layer)
    WP_PRECIP,           // precipitation intensity 0..1
    WP_FOG,              // fog density (1/m, volumetrics + analytic fog), 0..0.2
    WP_FOG_HEIGHT,       // fog layer height (m)
    WP_TEMPERATURE,      // deg C (rain <-> snow, melting)
    WP_WIND_DIR,         // deg, the direction the wind blows towards (0 = north / -Z, 90 = east / +X)
    WP_WIND,             // m/s
    WP_GUST,             // 0..1
    WP_LIGHTNING,        // strikes per minute
    WP_COUNT
} WeatherParam;

typedef struct WeatherParams { float v[WP_COUNT]; } WeatherParams;

const char *weather_param_name(int p);
// Rain <-> snow from the temperature: 0 rain (above +2 C) .. 1 snow (below -1 C).
float weather_precip_type(float temperature);

#define WEATHER_MAX_PRESETS 24

typedef struct WeatherPreset {
    char name[32];
    WeatherParams p;
    u32 set_mask;        // which parameters the preset sets (composition)
    float transition;    // default transition time into it (s)
    float min_stay, max_stay;   // scheduler: how long it lasts (s)
} WeatherPreset;

typedef struct WeatherLib {
    WeatherPreset presets[WEATHER_MAX_PRESETS];
    int count;
} WeatherLib;

// Wind as the weather sees it. The weather writes defaults; a later wind system can override.
typedef struct WeatherWind { vec3 dir; float strength, gust; } WeatherWind;

typedef struct WeatherState {
    WeatherParams from, to;
    u32 start_tick;
    float transition;    // s (0 = instant)
    // Accumulated surface state (integrated on every machine, resynced with each pack).
    float wetness;       // 0..1 surfaces wet
    float puddles;       // 0..1 puddle fill
    float snow;          // 0..1 snow cover
    u32 seed;            // lightning / scheduler randomness
    u16 version;         // bumped on every change (replication)
    u8 preset;           // the target preset (index, 255 = custom)
    u8 locked;           // no random changes
    u8 random;           // scheduler on
    u8 pad[3];
    u32 next_change_tick;
} WeatherState;

#define WEATHER_PACK_SIZE 120

bool weather_lib_load(WeatherLib *lib, const char *dir);     // *.json in dir (built-in defaults first)
void weather_lib_defaults(WeatherLib *lib);
int weather_find_preset(const WeatherLib *lib, const char *name);
// "overcast+rain": the presets applied left to right on top of base (only their set parameters).
bool weather_compose(const WeatherLib *lib, const char *spec, const WeatherParams *base, WeatherParams *out, float *transition);

void weather_state_init(WeatherState *s, const WeatherParams *p, u32 seed);
// A new target from the current blend at tick; transition < 0 = the preset's own time.
void weather_set_target(WeatherState *s, const WeatherParams *target, u32 tick, float transition);
// Per-parameter duration of a transition of length T (clouds take minutes, rain 20-60 s, ...).
float weather_param_duration(int p, float transition);
void weather_eval(const WeatherState *s, u32 tick, float tick_dt, WeatherParams *out);
void weather_integrate(WeatherState *s, const WeatherParams *cur, float dt);
// Lightning at a tick: flash 0..1, and whether a strike starts on this tick (distance m for thunder).
float weather_lightning(const WeatherState *s, const WeatherParams *cur, u32 tick, float tick_dt, bool *strike_now, float *distance_m);
WeatherWind weather_wind(const WeatherParams *cur, double time_s);
// Scheduler (host): returns true when it picked a new target this tick.
bool weather_schedule(WeatherState *s, const WeatherLib *lib, u32 tick, float tick_dt);

void weather_pack(const WeatherState *s, u8 out[WEATHER_PACK_SIZE]);
void weather_unpack(WeatherState *s, const u8 in[WEATHER_PACK_SIZE]);

// Wet / snowy surface response (the same constants as shaders/weather.glsl): porosity 0..1 (rough
// dielectrics ~0.6-0.8, metal ~0), wetness / snow 0..1 -> albedo factor, roughness.
void weather_wet_material(vec3 albedo, float rough, float metal, float porosity, float wetness, float snow, vec3 *out_albedo, float *out_rough);

// Lens droplets (screen space, 0..1 with y down; radius in fractions of the screen height): rain
// that hits the camera spawns drops, which grow, merge, run down once heavy (stretched trails) and
// evaporate when it stops raining or the camera is under a roof. The renderer refracts through
// them (src/render/post/post_weather.c).
#define WEATHER_LENS_MAX 192
typedef struct WeatherDrop { float x, y, r, alpha, vy, age, stretch; } WeatherDrop;
typedef struct WeatherLens {
    WeatherDrop d[WEATHER_LENS_MAX];
    int count;
    float spawn_acc;
    u32 rng;
} WeatherLens;
typedef struct WeatherLensInput {
    float rain;            // rain intensity 0..1 (not snow)
    float exposed;         // 0 under a roof .. 1 open sky
    float look_up;         // sin(pitch): rain hits a lens facing up
    float forward_speed;   // m/s
    float side_speed;      // m/s (drops slide sideways when strafing / turning)
    float temperature;     // C
    bool wiped;            // clear all (respawn, entering a vehicle ...)
} WeatherLensInput;
void weather_lens_init(WeatherLens *l, u32 seed);
void weather_lens_step(WeatherLens *l, const WeatherLensInput *in, float dt);

#endif
