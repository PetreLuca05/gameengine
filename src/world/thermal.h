// Temperature state layer (docs/NIGHT_OPTICS.md "Temperature state layer"): what the thermal scope
// sees. Every surface emits by its temperature and emissivity (8..14 um band, Planck), not by light.
//
//   surfaces   a thermal class per material (assets/thermal/materials.json): emissivity, inertia
//              (time constant), solar gain, night radiative loss, wet cooling, near-IR reflectance.
//              Each class integrates dT/dt = (T_eq - T) / tau on world time, T_eq driven by the air
//              temperature (weather), the sun, the clouds and the wetness: sun-warmed concrete stays
//              warm into the night, metal follows the air within minutes, water lags the most
//   entities   characters (skin / face / hands, clothing insulation; zombies a lower core: a gameplay
//              rule), corpses (Newtonian cooling from the death tick), vehicle parts (engine,
//              exhaust, brakes, tyres heat with use and cool per rate), weapon barrels (the muzzle
//              heat model), fires, lights (assets/thermal/entities.json)
//   sky        apparent sky temperature: very cold at the zenith on a clear night, the air
//              temperature at the horizon and under low cloud; what low-emissivity metal and water
//              at grazing angles reflect
//   radiance   band radiance B(T) over 8..14 um (the sensor's signal) and its inverse (the scope's
//              spot temperature readout)
//
// Pure: no world, no renderer (tests/test_night_optics.c). game_thermal.c feeds it the weather and
// the clock and builds the GPU tables.
#ifndef DG_THERMAL_H
#define DG_THERMAL_H

#include "core/base.h"

// Thermal classes. The order is fixed: the shaders index the class table by these values (4 bits in
// the material's thermal byte). THC_HOT is a self-heated emitter whose temperature comes with the
// draw (lamp bulbs, fires, hot spots).
typedef enum ThermalClass {
    THC_CONCRETE = 0,                // default for untagged materials
    THC_BRICK, THC_SOIL, THC_GRASS, THC_FOLIAGE, THC_ASPHALT, THC_WOOD,
    THC_METAL_PAINTED, THC_METAL_BARE, THC_GLASS, THC_WATER, THC_CLOTH, THC_SKIN, THC_SNOW,
    THC_PLASTIC, THC_HOT,
    THC_COUNT
} ThermalClass;

typedef struct ThermalClassDef {
    char name[24];
    float emissivity;                // 0..1 in 8..14 um (1 - reflectance: opaque in LWIR)
    float tau_h;                     // thermal inertia: first-order time constant, world hours
    float solar_gain;                // C above air at full sun on a level surface (clear sky)
    float direct_gain;               // C extra on a surface facing the sun (N.L = 1), sunlit
    float night_loss;                // C below air from radiating to a clear night sky
    float sky_facing_loss;           // C extra loss for an upward surface at night (sees the cold sky)
    float wet_cool;                  // C below air when fully wet (evaporation)
    float ir_reflectance;            // near-IR reflectance 0..1 (NVG); < 0: derived from the albedo
} ThermalClassDef;

// Human / zombie body model.
typedef struct ThermalBody {
    float core;                      // C
    float skin;                      // C, exposed skin at 20 C still air
    float face, hands;               // C offsets from skin (face warmer, hands cooler)
    float cold_k;                    // skin drops this much per C of air below 20 C
    float wind_k;                    // and this much per m/s of wind
} ThermalBody;

// Clothing insulation per character part role ("clo"-like, unitless): T = air + (skin - air) / (1 + clo * k).
typedef enum ThermalRole {
    THR_SKIN, THR_FACE, THR_HANDS, THR_TORSO, THR_LEGS, THR_FEET, THR_HEADWEAR, THR_ARMOR, THR_BACKPACK, THR_HAIR,
    THR_COUNT
} ThermalRole;

// A vehicle part's heating: towards air + rise (rise_idle + rise_load * load) while driven, the
// heating / cooling time constants differ (an engine warms in minutes, cools over a quarter hour).
typedef struct ThermalPartDef {
    float rise_idle, rise_load;      // C above air
    float heat_tau, cool_tau;        // s
} ThermalPartDef;

typedef enum ThermalVehPart { THV_ENGINE, THV_EXHAUST, THV_BRAKE, THV_TIRE, THV_COUNT } ThermalVehPart;

typedef struct ThermalData {
    ThermalClassDef cls[THC_COUNT];
    // surface name (SurfaceMaterial / level "material" / item material) -> class
    struct { char name[24]; u8 cls; } map[64];
    int map_count;
    // entities
    ThermalBody human, zombie;
    float clo[THR_COUNT];            // insulation per role
    float clo_k;                     // insulation strength
    float corpse_tau;                // s: corpse cooling time constant (gameplay-scaled)
    float blood_t0, blood_tau;       // C, s
    ThermalPartDef veh[THV_COUNT];
    float brake_heat;                // C/s per (m/s) while braking
    float tire_speed_k;              // C per m/s of speed (steady state)
    float barrel_k;                  // C per gram of powder of muzzle heat (bal_muzzle heat)
    float barrel_max;                // C cap
    float bulb, led, fire, muzzle, explosion, breath, exhaust_plume, footprint, footprint_tau, heat_decal_tau;
    // sky
    float sky_clear_depression;      // C below air at the zenith, clear sky
    float sky_cloud_keep;            // fraction of the depression left under full cloud
    float sky_exponent;              // shape of the zenith -> horizon blend
} ThermalData;

// Defaults (the built-in table; the JSON overrides any field).
void thermal_defaults(ThermalData *d);
// Parses assets/thermal/materials.json / entities.json text (either may be NULL). False on a parse
// error (the defaults stay).
bool thermal_load_materials(ThermalData *d, const char *json_text);
bool thermal_load_entities(ThermalData *d, const char *json_text);
// Class of a surface / material name ("concrete", "metal_thin", "foliage", ...); THC_CONCRETE if unknown.
int thermal_class_of(const ThermalData *d, const char *name);
int thermal_class_by_name(const char *class_name);   // a class's own name, -1 if none
const char *thermal_class_name(int cls);

// The environment the surfaces see (from the weather + the clock).
typedef struct ThermalEnv {
    float air;                       // C (WP_TEMPERATURE)
    float sun;                       // 0..1 normalised solar input on a level surface (sin elevation, 0 below the horizon)
    float cloud;                     // 0..1 cover
    float wetness;                   // 0..1
    float wind;                      // m/s
} ThermalEnv;

float thermal_sun_input(float sun_elev_deg);
// Equilibrium temperature of a class.
float thermal_class_eq(const ThermalData *d, int cls, const ThermalEnv *env);
// Advances one class temperature over dt world seconds (exact for a constant environment).
float thermal_class_step(const ThermalData *d, int cls, float t, const ThermalEnv *env, float dt);

// Surface state: one temperature per class + the class-smoothed solar input (the directional term
// fades with the class's inertia, not instantly at sunset).
typedef struct ThermalSurfaces {
    float t[THC_COUNT];
    float solar[THC_COUNT];          // smoothed sun input 0..1
    bool valid;
} ThermalSurfaces;

// Environment at a world hour (for re-settling after a clock jump): the caller's sun model.
typedef void (*ThermalEnvFn)(double hours, ThermalEnv *out, void *user);
void thermal_surfaces_step(const ThermalData *d, ThermalSurfaces *s, const ThermalEnv *env, float dt);
// Re-solves the state at world hour `hours`: integrates the two days before it (10 min steps) so a
// time_set / join lands on the periodic solution instead of starting from the air temperature.
void thermal_surfaces_settle(const ThermalData *d, ThermalSurfaces *s, double hours, ThermalEnvFn fn, void *user);
// A surface of class cls with normal facing the sun by n_dot_l (0..1, 0 when shadowed) and
// sky_view (0..1, up = 1): the temperature the shader computes (CPU mirror).
float thermal_surface_temp(const ThermalData *d, const ThermalSurfaces *s, const ThermalEnv *env, int cls, float n_dot_l, float sky_view);

// Characters.
float thermal_skin(const ThermalBody *b, float air, float wind);
float thermal_role_temp(const ThermalData *d, const ThermalBody *b, int role, float air, float wind);
// Newtonian cooling after death (t = seconds since death).
float thermal_cooling(float t0, float air, float t, float tau);

// Vehicles: lumped part temperatures, local and cosmetic, from replicated usage.
typedef struct ThermalVehicle {
    float part[THV_COUNT];           // engine, exhaust, brake (hottest disc), tyre
    float brake_w[4], tire_w[4];     // per wheel
    bool valid;
} ThermalVehicle;
typedef struct ThermalVehInput {
    bool engine;                     // running
    float load;                      // 0..1 (rpm fraction)
    bool braking;
    float speed;                     // m/s
} ThermalVehInput;
void thermal_vehicle_init(ThermalVehicle *v, float air, float engine_c);
void thermal_vehicle_step(const ThermalData *d, ThermalVehicle *v, const ThermalVehInput *in, float air, float dt);

// Weapon barrel from the muzzle heat (grams of powder, bal_muzzle.h BalMuzzleHeat.heat).
float thermal_barrel_temp(const ThermalData *d, float heat_g, float air);

// Apparent sky temperature along a direction with cos_zenith (1 = up, <= 0 = horizon / below).
float thermal_sky_temp(const ThermalData *d, float air, float cloud, float cos_zenith);

// Band radiance (W / m2 / sr, 8..14 um) of a black body at t C, and its inverse.
double thermal_band_radiance_exact(double t_c);  // numeric Planck integral
float thermal_radiance(float t_c);               // LUT (what the GPU uses)
float thermal_temp_of_radiance(float l);         // inverse of the LUT
// The radiance a surface sends to the sensor: emissivity e at t, reflecting (1 - e) of t_refl.
float thermal_surface_radiance(float e, float t, float t_refl);
// Spot meter: the temperature a sensor reads from a radiance assuming emissivity e (real devices assume ~0.95).
float thermal_spot_temp(float l, float e, float t_refl);

#define THERMAL_LUT_N 256
#define THERMAL_LUT_T0 (-60.0f)      // C at entry 0
#define THERMAL_LUT_DT 6.5f          // C per entry (to 1597.5 C)
const float *thermal_lut(void);      // THERMAL_LUT_N radiances

#endif
