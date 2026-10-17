// Ballistics data: cartridges (assets/ballistics/cartridges.json), surface materials
// (assets/ballistics/materials.json) and muzzle devices (the "muzzle_device" block of the attachment
// JSON). Pure module: parses JSON text, no file system or engine state, so the tests load it
// directly. docs/BALLISTICS.md lists every field with its real-world basis.
#ifndef DG_BAL_DATA_H
#define DG_BAL_DATA_H

#include "core/base.h"
#include "core/dgmath.h"
#include "vfx/vfx_types.h"

#define BAL_MAX_CARTRIDGES 32
#define BAL_MAX_DEVICES 16
#define BAL_MV_POINTS 8
#define BAL_MAX_PORTS 4

typedef enum BalDragCurve { BAL_CURVE_G1, BAL_CURVE_G7, BAL_CURVE_GS, BAL_CURVE_COUNT } BalDragCurve;
typedef enum BalMatClass { BAL_HARD, BAL_SOFT } BalMatClass;
typedef enum BalTracerColor { BAL_TRC_RED, BAL_TRC_GREEN, BAL_TRC_WHITE, BAL_TRC_ORANGE, BAL_TRC_COUNT } BalTracerColor;

typedef struct BalTracerDef {
    u8 every;            // every Nth round of a magazine is a tracer (0 = no tracer loading)
    u8 last_n;           // the last N rounds of a magazine are tracers (low-ammo indicator)
    u8 color;            // BalTracerColor
    float ignition_m;    // dim igniter up to this distance
    float burn_m, burn_s;// the compound burns out at whichever comes first
    float mass, bc;      // tracer bullet overrides (kg, lb/in²); 0 = same as ball
} BalTracerDef;

typedef struct BalCartridge {
    char id[24];
    char name[40];
    u32 hash;                         // dg_hash_str(id)
    float mass;                       // kg
    float diameter, length, twist;    // m (twist: m per turn, 0 = smoothbore)
    u8 curve;                         // BalDragCurve
    float bc;                         // lb/in² against the curve
    float exp_k;                      // 1/m: exponential-model drag a = exp_k·v² (fitted at load)
    float mv_curve[BAL_MV_POINTS][2]; // barrel m, m/s
    int mv_count;
    float ref_barrel, mv_ref;         // m, m/s
    float e_ref;                      // J: ½·m·mv_ref², the penetration reference energy
    float powder;                     // g
    float gas_temp;                   // K
    float burn_length;                // m
    float muzzle_pressure;            // MPa at ref_barrel
    int pellets;                      // 1 for bullets
    float pen_hard, pen_soft;
    float fragment_v;                 // m/s, 0 = never fragments
    float yaw_depth;                  // m of tissue before the bullet yaws (0 = stays point-on)
    float transonic_k;                // rad/s
    float spin_sg;                    // Miller gyroscopic stability at mv_ref (0 = no spin drift)
    float max_range;                  // m
    BalTracerDef tracer;
} BalCartridge;

typedef struct BalMaterial {
    u8 cls;               // BalMatClass
    float ricochet_deg;   // incidence from the surface
    float depth_ref;      // m defeated by 5.56 M855 at its muzzle (x cartridge pen at its own muzzle)
    float hardness, porosity;
    float exit_spread_deg;
    float decal_scale;
    float thickness;      // m, assumed when the exit face cannot be measured
} BalMaterial;

// Muzzle flash components (bal_muzzle.c); a device scales each one.
typedef enum BalFlashComp {
    BFC_PRIMARY,     // dim orange glow at the crown
    BFC_MACH,        // shock bottle / intermediate flash, 2-6 cm out
    BFC_SECONDARY,   // the fireball 10-50 cm out (what devices fight)
    BFC_PRONGS,      // flash hider: dim forward cone / spikes between the prongs
    BFC_VENTS,       // compensator / brake: side and upward port jets
    BFC_VAPOR,       // suppressor: soft vapour wisp
    BFC_SMOKE,
    BFC_SPARKS,      // unburnt powder
    BFC_BLAST,       // heat-haze distortion + near-surface dust
    BFC_LIGHT,
    BFC_COUNT
} BalFlashComp;

typedef enum BalDeviceKind { BAL_DEV_BARE, BAL_DEV_HIDER, BAL_DEV_COMP, BAL_DEV_SUPPRESSOR, BAL_DEV_KIND_COUNT } BalDeviceKind;

typedef struct BalDevice {
    char id[24];
    u8 kind;                      // BalDeviceKind (also the VfxMuzzleDevice index)
    float mul[BFC_COUNT];
    float report_db;              // added to the report level (suppressor -28)
    float report_db_side;         // extra for listeners beside the muzzle (brakes)
    float velocity_mul;           // muzzle velocity
    float poi_moa[2];             // point-of-impact shift (x right, y up)
    int prongs;
    vec3 ports[BAL_MAX_PORTS];    // vent directions in muzzle space (x right, y up, -z forward)
    int port_count;
    float pop_cold_s;             // suppressor: a first-round pop after this long idle (0 = never)
    float pop_flash;              // pop secondary multiplier
    float can_heat_rounds;        // rounds before the can shimmers
    float can_heat_tau;           // s, can cooling time constant
} BalDevice;

typedef struct BalData {
    BalCartridge cart[BAL_MAX_CARTRIDGES];
    int cart_count;
    BalMaterial mat[SURF_COUNT];
    BalDevice dev[BAL_MAX_DEVICES];
    int dev_count;                // dev[0] is always the built-in "bare"
    u32 hash;                     // content hash of everything parsed (join manifest)
} BalData;

void bal_data_init(BalData *d);   // defaults: no cartridges, sane materials, the bare device
// Each parser adds to / overrides what is there; false on malformed JSON.
bool bal_data_parse_cartridges(BalData *d, const char *json);
bool bal_data_parse_materials(BalData *d, const char *json);
// One attachment file: picks up its "muzzle_device" object (a plain string is ignored).
bool bal_data_parse_attachment(BalData *d, const char *json);

int bal_cartridge_find(const BalData *d, const char *id);   // -1 if missing
int bal_device_find(const BalData *d, const char *id);      // 0 (bare) if missing
const char *bal_material_name(int surf);
int bal_material_from_name(const char *name);                // -1 if unknown
const char *bal_device_kind_name(int kind);

// Muzzle velocity for a barrel length (linear in the curve, clamped extrapolation).
float bal_cartridge_mv(const BalCartridge *c, float barrel_m);
// Derived fields (e_ref, spin_sg, exp_k); called by the parser, exposed for tests.
void bal_cartridge_finalize(BalCartridge *c);

#endif
