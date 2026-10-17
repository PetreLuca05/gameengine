// Vehicle definitions and the pure vehicle rules (docs/VEHICLES.md): assets/vehicles/<name>.json
// (body boxes, wheels, engine / gears, fuel, tires, seats, sockets), the fuel consumption curve,
// the tire model (pressure / health -> grip, pull, rim), one-handed steering clamp, quantization
// for replication. No world, no physics, no renderer (tests/test_vehicles.c). The game glue
// (physics, seats, replication, HUD, VFX) is src/game/game_vehicles.c.
#ifndef DG_VEHICLE_DEF_H
#define DG_VEHICLE_DEF_H

#include "core/base.h"
#include "core/dgmath.h"

#define VEH_MAX_WHEELS 4
#define VEH_MAX_SEATS 4
#define VEH_MAX_BOXES 4
#define VEH_MAX_GEARS 8
#define VEH_MAX_DEFS 8

typedef enum VehicleKind { VK_CAR = 0, VK_MOTO = 1 } VehicleKind;

typedef struct VehicleWheelDef {
    vec3 pos;                    // suspension attachment (vehicle space)
    float radius, width;
    float susp_min, susp_max, susp_freq, susp_damp;
    float max_steer_deg;         // 0 = not steered
    float max_brake, max_handbrake;
    bool driven;
    bool left;                   // left side (pull direction when flat)
} VehicleWheelDef;

typedef struct VehicleSeatDef {
    vec3 pos;                    // seat_n: hip point (vehicle space)
    vec3 exit;                   // exit_n: where the occupant is placed on exit (vehicle space)
    vec3 eye;                    // first-person eye relative to the hip point
    bool driver;
    bool can_shoot;              // passengers (and a one-handed driver)
    float yaw_limit_deg;         // free-look limit around the vehicle forward
} VehicleSeatDef;

typedef struct VehicleDef {
    char name[32], display[48];
    VehicleKind kind;
    float mass;
    int box_count;
    vec3 box_center[VEH_MAX_BOXES], box_half[VEH_MAX_BOXES];
    vec3 com_offset;
    int wheel_count;
    VehicleWheelDef wheels[VEH_MAX_WHEELS];
    float max_torque, min_rpm, max_rpm, diff_ratio, reverse_ratio, max_lean_deg;
    float gears[VEH_MAX_GEARS];
    int gear_count;
    // Fuel (litres): idle burn and full-throttle burn at max rpm (litres / hour).
    float fuel_capacity, fuel_idle_lph, fuel_full_lph;
    // Damage
    float hp;
    float impact_damage_speed;   // m/s of impact speed below which nothing breaks
    float impact_damage_scale;   // hp per (m/s over the threshold)
    // Tires
    float tire_bullet_damage;    // health lost per bullet
    float tire_leak_per_hit;     // pressure / s leak added by a puncture
    float tire_burst_health;     // health below which the tire bursts (pressure -> 0 at once)
    // Seats / sockets
    int seat_count;
    VehicleSeatDef seats[VEH_MAX_SEATS];
    vec3 steer_pivot, steer_axis;   // steering wheel / handlebar pivot and axis (vehicle space)
    float steer_ratio;              // steering wheel turns (rad) per wheel steer (rad)
    float steer_radius;             // rim radius (car) or half bar width (moto)
    vec3 grip_l, grip_r;            // hand grips at neutral steering (vehicle space)
    vec3 pedal_l, pedal_r;          // feet (leg IK)
    vec3 fuel_cap, exhaust, headlight_l, headlight_r;
    vec3 camera_tp;                 // third-person orbit offset (behind, up)
    float one_hand_steer_limit;     // max |steer| while shooting one-handed (0..1)
    float one_hand_spread_mult;     // spread multiplier one-handed while driving
    float passenger_spread_mult;    // spread multiplier for passengers (vehicle motion)
    float exit_ragdoll_speed;       // m/s: leaving faster ragdolls the occupant
    vec3 color;                     // placeholder paint
    // Water (docs/WATER.md "Vehicles", JSON "water"): the air that keeps it afloat while dry, the
    // cabin inlets (door sills) water floods through, the engine intake, a boat hull (stub).
    float float_volume;             // m3 displaced fully submerged while dry (sealed cabin + hull)
    float flood_inlet_y;            // m (vehicle space): water above this floods the cabin
    float flood_rate;               // cabin fill per second with 0.5 m of water over the inlet
    float flood_time;               // s with the intake under water (or a half-flooded cabin) before the engine dies
    float engine_intake_y;          // m (vehicle space)
    bool boat;                      // buoyant hull, never floods; throttle pushes at the stern (stub)
    float boat_thrust;              // N at full throttle
} VehicleDef;

typedef struct VehicleLib {
    VehicleDef defs[VEH_MAX_DEFS];
    int count;
} VehicleLib;

void vehicle_def_defaults(VehicleDef *d, VehicleKind kind);
// Parses one definition (JSON text). Missing keys keep the defaults for the kind.
bool vehicle_def_parse(VehicleDef *d, const char *json, size_t len);
bool vehicle_lib_load(VehicleLib *lib, const char *dir);   // car.json, moto.json (built-in defaults if missing)
const VehicleDef *vehicle_lib_find(const VehicleLib *lib, const char *name);
int vehicle_lib_index(const VehicleLib *lib, const char *name);

// ---- water (flooding)
// Cabin flood 0..1 after dt s with `over_inlet` m of water over the inlets (<= 0: drains slowly
// once the vehicle is out of the water).
float vehicle_flood_step(const VehicleDef *d, float flood, float over_inlet, float dt);
// Displaced volume at full submersion with the cabin `flood` full (water replaces the air).
float vehicle_float_volume(const VehicleDef *d, float flood);

// ---- fuel
// Litres per second at a throttle (0..1) and engine speed (0..1 of the rpm range).
float vehicle_fuel_rate(const VehicleDef *d, float throttle, float rpm_frac);

// ---- tires
typedef struct TireState {
    float pressure;              // 0 flat .. 1 inflated
    float health;                // 0 destroyed .. 1 new
    float leak;                  // pressure / s
} TireState;
void tire_init(TireState *t);
// A bullet hit; returns true if the tire burst.
bool tire_hit(TireState *t, const VehicleDef *d);
void tire_tick(TireState *t, float dt);
void tire_repair(TireState *t);      // tire_repair item: plug the leak, pressure back to 0.85
void tire_replace(TireState *t);     // spare tire: new
// Grip multipliers (longitudinal, lateral) and the rolling radius scale (rim at 0 pressure).
void tire_grip(const TireState *t, float *lon, float *lat);
float tire_radius_scale(const TireState *t);
bool tire_on_rim(const TireState *t);
// Steering pull (-1..1, + = right) from uneven deflation (left flat pulls left).
float vehicle_tire_pull(const VehicleDef *d, const TireState *tires);

// ---- driving
// Steering while shooting one-handed: clamped to the vehicle's one_hand_steer_limit.
float vehicle_steer_clamp(const VehicleDef *d, float steer, bool one_hand);
// Steering input smoothing (keyboard): moves cur towards target at a speed-dependent rate.
float vehicle_steer_smooth(float cur, float target, float speed_mps, float dt);
// Impact damage (hp) from a change of velocity (m/s).
float vehicle_impact_damage(const VehicleDef *d, float delta_v);

// ---- quantization (replication)
u8 veh_q01(float v);         // 0..1 -> 0..255
float veh_dq01(u8 q);
i8 veh_qs(float v);          // -1..1 -> -127..127
float veh_dqs(i8 q);

#endif
