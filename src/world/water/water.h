// Water bodies and the world-wide water queries (docs/WATER.md). Every water consumer (physics,
// buoyancy, swimming, AI, VFX, audio, the renderer's water tile) asks the same functions:
//
//   water_height_at(w, x, z, t)   surface height (swell included for the ocean), or WATER_NONE
//   water_depth_at(w, x, z, t)    surface height minus the ground (needs a ground function), or 0
//   water_sample(w, x, z, t, s)   both + normal, water velocity (orbital + flow), the body
//
// Bodies: puddle (shading only: the weather puddle mask, never in the queries), lake (flat level,
// polygon or ellipse), ocean (infinite at its level, waves from the shared WaveModel), river (a
// callback: level profile + flow). A body may instead delegate its footprint to a callback (the
// open world's mapgen lakes / sea / rivers). Deterministic for (bodies, wind, seed, t): nothing here
// is replicated. t is waves_time() of the server tick.
//
// Pure: no engine, no renderer (tests/test_water.c).
#ifndef DG_WATER_H
#define DG_WATER_H

#include "core/base.h"
#include "water_waves.h"

#define WATER_MAX_BODIES 64
#define WATER_POLY_MAX 48
#define WATER_NONE -1e9f
#define WATER_DENSITY_FRESH 1000.0f
#define WATER_DENSITY_SEA 1025.0f

typedef enum WaterKind { WATER_PUDDLE, WATER_LAKE, WATER_OCEAN, WATER_RIVER, WATER_KIND_COUNT } WaterKind;
typedef enum WaterShape { WSHAPE_INFINITE, WSHAPE_POLYGON, WSHAPE_ELLIPSE, WSHAPE_CALLBACK } WaterShape;

// Per-body look and physics (assets/world/water/<preset>.json). Absorption / scattering are 1/m
// per RGB channel (Beer-Lambert): clear water absorbs red first.
typedef struct WaterParams {
    char preset[32];
    float absorb[3];                 // 1/m
    float scatter[3];                // 1/m (in-scattering colour of the medium)
    float turbidity;                 // 0..1: extra scattering, murk
    float roughness;                 // surface micro roughness (0.02 calm .. 0.12 choppy)
    float wave_scale;                // swell amplitude multiplier (ocean); lakes: wind ripple strength
    float wind_coupling;             // 0..1: how much the weather wind drives the waves / ripples
    float choppiness;                // 0..1: Gerstner horizontal displacement
    float fetch_km;                  // ocean: fetch of the wind (shorter = smaller waves)
    float foam;                      // shore / crest foam amount
    float flow_speed;                // m/s along the flow direction (rivers)
    float temperature;               // deg C (snow / ice stub: frozen below 0)
    float density;                   // kg/m3 (fresh 1000, sea 1025)
} WaterParams;

typedef struct WaterBody WaterBody;
// Callback footprint: true when (x, z) is in the body; level / flow out (flow may be left at 0).
typedef bool (*WaterBodyFn)(void *user, const WaterBody *b, float x, float z, float *level, float flow[2]);

struct WaterBody {
    char name[32];
    u8 kind;                         // WaterKind
    u8 shape;                        // WaterShape
    WaterParams p;
    float level;                     // still surface height (m)
    float bmin[2], bmax[2];          // xz bounds (ignored for WSHAPE_INFINITE)
    int poly_n;
    float poly[WATER_POLY_MAX][2];   // WSHAPE_POLYGON (x, z), any winding
    float center[2], radius[2], yaw; // WSHAPE_ELLIPSE (yaw in degrees)
    float flow[2];                   // constant flow (m/s) for polygon / ellipse bodies
    WaterBodyFn fn;                  // WSHAPE_CALLBACK
    void *user;
    int user_index;                  // free for the callback (e.g. the mapgen feature index)
};

// Ground height for depth queries (terrain raster, a physics raycast bake, a flat floor).
typedef float (*WaterGroundFn)(void *user, float x, float z);

typedef struct WaterWorld {
    WaterBody body[WATER_MAX_BODIES];
    int count;
    WaveModel waves;                 // the ocean swell (one model: there is one sea)
    WaterGroundFn ground;
    void *ground_user;
    float wind, wind_dir_deg;        // last replicated wind fed in
    int ocean;                       // body index of the ocean, -1 = none
} WaterWorld;

typedef struct WaterSample {
    float height;                    // surface height (m), WATER_NONE outside water
    float level;                     // still level of the body
    float ground;                    // ground height (WATER_NONE without a ground function)
    float depth;                     // height - ground (0 without water; 1e9 without ground)
    float normal[3];
    float vel[3];                    // water velocity: orbital + flow (m/s)
    float jacobian;
    int body;                        // index, -1 = none
    u8 kind;
} WaterSample;

WaterParams water_params_default(WaterKind kind);
const char *water_kind_name(WaterKind k);
int water_kind_parse(const char *s);       // -1 if unknown
// JSON preset over `p` (only the present keys change). False on a parse error.
bool water_params_parse(WaterParams *p, const char *json, char *err, int err_len);
// One body from a level JSON entry (cJSON object, typed void to keep cJSON out of the header):
// {"type":"lake","name":..,"preset":..,"level":2,"polygon":[[x,z],..]} | "ellipse":{center,radius,yaw}
// | {"type":"ocean","level":0}. Preset keys may be overridden inline ("absorb":[..], ...). The preset
// must already be applied by the caller (water_body_from_json takes the resolved params).
bool water_body_from_json(WaterBody *b, const void *cjson_obj, const WaterParams *preset, char *err, int err_len);

void water_world_init(WaterWorld *w, u32 seed);
void water_world_clear(WaterWorld *w);                    // keeps the seed / wind / ground
void water_set_ground(WaterWorld *w, WaterGroundFn fn, void *user);
// Replicated weather wind (m/s, heading degrees): re-weights the swell (no-op when unchanged).
void water_set_wind(WaterWorld *w, float wind, float dir_deg);
int water_add_body(WaterWorld *w, const WaterBody *b);    // index, -1 when full; computes bounds
void water_body_bounds(WaterBody *b);

// The body whose surface is highest at (x, z) (bodies overlap at lake mouths / river ends), -1 none.
int water_body_at(const WaterWorld *w, float x, float z, float *level, float flow[2]);
float water_height_at(const WaterWorld *w, float x, float z, float t);
float water_depth_at(const WaterWorld *w, float x, float z, float t);
bool water_sample(const WaterWorld *w, float x, float z, float t, WaterSample *s);
// A local surface patch around (x, z) (buoyancy): 5 surface points (water_patch order: centre, +x,
// -x, +z, -z at ~e m), h = WATER_NONE where a point is dry / outside the body. One wave evaluation
// for all 5 (waves_patch). False when none of the points is in water.
typedef struct WaterPatch {
    float x[5], z[5], h[5];
    float vel[5][3];
    float density;
    int body;
} WaterPatch;
bool water_patch(const WaterWorld *w, float x, float z, float t, float e, WaterPatch *p);
// Plane through the patch (least squares over the wet points): height at (x, z), slope, mean velocity.
bool water_patch_plane(const WaterPatch *p, float x, float z, float *h, float *gx, float *gz, float vel[3]);

// Wave parameters the ocean body would use for this wind (exposed for the GPU upload / tests).
WaveParams water_ocean_wave_params(const WaterWorld *w);

// Point in polygon (even-odd), exposed for the tests / tools.
bool water_poly_inside(const float (*poly)[2], int n, float x, float z);

#endif
