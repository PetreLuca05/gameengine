// Muzzle flash composed from physics (pure). Inputs are the cartridge (powder charge, gas
// temperature, the barrel length at which the powder is burnt, muzzle pressure), the weapon's
// barrel length, the muzzle device, the heat of recent fire, whether the muzzle is close to a
// surface and the scene exposure. Output is one parameter set per flash component
// (BalFlashComp); the game spawns one small effect per component. docs/BALLISTICS.md "Muzzle flash".
#ifndef DG_BAL_MUZZLE_H
#define DG_BAL_MUZZLE_H

#include "gameplay/ballistics/bal_data.h"

typedef struct BalMuzzleIn {
    const BalCartridge *cart;
    const BalDevice *dev;
    float barrel;          // m
    float speed;           // m/s at the muzzle
    float heat;            // bal_muzzle_heat_at(): g of powder "recently" burnt
    float can_heat;        // suppressor rounds-equivalent heat
    bool near_surface;     // muzzle within ~0.5 m of ground / wall
    bool pop;              // first round through a cold suppressor
    float exposure_gain;   // bal_flash_exposure_gain()
} BalMuzzleIn;

typedef struct BalFlashParams {
    float size;            // m (radius / length of the component)
    float dist;            // m in front of the muzzle along the bore
    float life;            // s
    float intensity;       // emissive / alpha multiplier (0 = do not spawn)
    float count;           // particle count multiplier (sparks, smoke puffs)
    vec3 rgb;              // linear tint (blackbody for the hot components)
} BalFlashParams;

typedef struct BalMuzzleOut {
    BalFlashParams c[BFC_COUNT];
    float unburnt;         // 0..1 fraction of the charge leaving unburnt
    float pressure;        // MPa at the muzzle
    float light_radius;    // m
    bool haze;             // lingering heat haze (sustained fire / hot can)
    bool dust;             // near-surface dust plume
    float dust_scale;
} BalMuzzleOut;

void bal_muzzle_compose(const BalMuzzleIn *in, BalMuzzleOut *out);

// Exposure awareness. A flash has a fixed physical brightness; what changes between night and day
// is the adaptation. The tonemapper scales by 2^-EV (the engine's EV range is compressed, so that
// alone is not enough); this gain cancels it relative to the night exposure and applies the
// eye's adaptation from the daylight level (0 night .. 1 full sun): the night look as authored,
// ~0.15x perceived in full daylight.
float bal_flash_exposure_gain(float ev, float ev_night, float daylight);
// Perceived brightness relative to night (gain × 2^-(ev-ev_night)); used by the tests / debug.
float bal_flash_perceived(float ev, float ev_night, float daylight);
#define BAL_FLASH_DAY_RATIO 0.15f

// Normalized linear RGB of a blackbody (Planck sampled at 610 / 550 / 465 nm, max = 1).
vec3 bal_blackbody_rgb(float kelvin);

// Heat of recent fire (per weapon holder, cosmetic): lazy exponential decay on the shot clock.
typedef struct BalMuzzleHeat {
    float heat;            // g of powder, τ = 8 s
    float can_heat;        // rounds, τ = device can_heat_tau
    double last_shot;      // s
    bool fired;
} BalMuzzleHeat;

// Registers a shot at time now (s). Returns true when this is a first-round pop (a suppressor
// that sat idle for pop_cold_s, or its first shot ever).
bool bal_muzzle_heat_shot(BalMuzzleHeat *h, const BalCartridge *c, const BalDevice *dev, double now);
float bal_muzzle_heat_at(const BalMuzzleHeat *h, double now);
float bal_muzzle_can_heat_at(const BalMuzzleHeat *h, const BalDevice *dev, double now);

#endif
