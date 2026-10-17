// Buoyancy, water drag and wave lift for dynamic Jolt bodies (docs/WATER.md "Buoyancy").
//
// Each buoyant body carries a set of probes generated from its collision shape: cells of a grid over
// the shape, each with a body-space centre, half extents and a share of the displaced volume. Per
// tick and per probe, the submerged share of the cell is the clamp of the water height across the
// cell's vertical extent (exact for an upright box), and the probe gets
//   buoyancy   F = rho * V_sub * g, up, at the submerged centroid
//   drag       on the point velocity relative to the water (orbital + flow): linear (1/s, tunable
//              `drag` / `angular_drag` for the rotational part) + quadratic (rho Cd A |v| v)
//   wave lift  a push down the surface slope, proportional to V_sub (floating debris drifts)
// summed into one impulse + one angular impulse about the centre of mass. The water surface is a
// plane per body (BuoyWaterFn: one 5-point patch of the wave model), so the cost is one query per
// body, not per probe.
//
// Floating vs sinking: `density` (kg/m3) sets the displaced volume as mass / density (0: the shape's
// own volume). A body of density 500 in fresh water floats half submerged.
//
// buoy_compute is pure (no Jolt: tests/test_water.c checks it analytically); the system functions
// read / write the bodies through physics.h. Host: props and vehicles; clients: their own ragdolls
// and the vehicle they predict (the same code, so prediction matches).
#ifndef DG_BUOYANCY_H
#define DG_BUOYANCY_H

#include "physics.h"

#define BUOY_MAX_PROBES 32
#define BUOY_MAX_BODIES 512

typedef struct BuoyProbe {
    vec3 local;              // cell centre, body space (relative to the body origin)
    vec3 half;               // cell half extents, body space
    float volume;            // shape volume of the cell (m3)
    float area;              // drag cross-section (m2): volume^(2/3), set by buoy_setup
} BuoyProbe;

typedef struct BuoyDesc {
    float density;           // kg/m3 of the body as a whole (0 = from the shape volume and mass)
    float drag;              // 1/s linear water drag at full submersion (default 0.8)
    float angular_drag;      // 1/s on the rotational point velocity (default 1.2)
    float quad_drag;         // Cd of the quadratic drag (default 0.6)
    float lift;              // wave lift factor (default 0.35)
} BuoyDesc;

// The water surface around a body as a plane: height at (x, z), slope (dh/dx, dh/dz), water
// velocity, density. False = no water within `extent` of (x, z). The game implements it with
// water_patch + water_patch_plane (one wave evaluation per body).
typedef struct BuoyWaterPlane { float height, gx, gz; float vel[3]; float density; } BuoyWaterPlane;
typedef bool (*BuoyWaterFn)(void *user, float x, float z, float extent, BuoyWaterPlane *out);

typedef struct BuoyWater {
    BuoyWaterFn plane;
    void *user;
    float gravity;           // m/s2 (9.81)
} BuoyWater;

typedef struct Buoyancy {
    BodyId body;
    int n;
    BuoyProbe probe[BUOY_MAX_PROBES];
    float shape_volume;      // sum of the probe volumes
    float volume;            // displaced volume at full submersion (m3)
    float mass;              // kg (read from the body when added)
    BuoyDesc d;
    float extent;            // horizontal half size (m) for the surface fit
    // last step (debug / gameplay: vehicles flooding, splash on entry, ragdolls)
    float submerged;         // 0..1 of `volume`
    float water_h;           // fitted surface height at the centre of mass
    vec3 impulse, ang_impulse;
    float prev_submerged;
    bool in_water;
} Buoyancy;

typedef struct BuoyBodyState {
    vec3 pos;                // body origin
    quat rot;
    vec3 com;                // world centre of mass
    vec3 v, w;               // linear / angular velocity
} BuoyBodyState;

typedef struct BuoyResult {
    vec3 impulse;            // total linear impulse (N s), apply at the centre of mass
    vec3 ang_impulse;        // total angular impulse about the centre of mass
    float submerged;         // 0..1
    float water_h;
    bool in_water;
    int probes_wet;
} BuoyResult;

BuoyDesc buoy_desc_default(void);
// Probes for a shape (box grid, sphere / capsule / cylinder layered cells, mesh: its AABB at 60 %
// fill). Returns the count (<= max).
int buoy_probes_from_shape(const ShapeDesc *s, BuoyProbe *out, int max);
// Appends an nx x ny x nz grid of cells over a body-space box (vehicle compounds).
int buoy_probes_box(vec3 center, vec3 half, int nx, int ny, int nz, BuoyProbe *out, int max);
// Auto grid resolution for a box: ~0.5 m cells, 1..4 per axis, at most `max` cells.
void buoy_box_grid(vec3 half, int max, int *nx, int *ny, int *nz);

void buoy_setup(Buoyancy *b, BodyId body, const BuoyProbe *probes, int n, float mass, const BuoyDesc *d);
// Pure: forces for one tick (dt s) on a body in state st.
void buoy_compute(const Buoyancy *b, const BuoyBodyState *st, const BuoyWater *water, float dt, BuoyResult *out);

// ------------------------------------------------------------------ system (physics bodies)
typedef struct BuoySystem {
    Buoyancy item[BUOY_MAX_BODIES];
    int count;
    // per-step scratch (parallel compute, serial apply)
    BuoyBodyState st[BUOY_MAX_BODIES];
    BuoyResult res[BUOY_MAX_BODIES];
    double last_ms;          // CPU time of the last buoy_step (ms): read + compute + apply
    double read_ms, compute_ms, apply_ms;
} BuoySystem;

void buoy_system_init(BuoySystem *s);
// Adds (or replaces) the body: probes from its shape, mass from the body. -1 when full / not dynamic.
int buoy_add(BuoySystem *s, PhysicsWorld *pw, BodyId body, const BuoyDesc *d);
int buoy_add_probes(BuoySystem *s, PhysicsWorld *pw, BodyId body, const BuoyProbe *probes, int n, const BuoyDesc *d);
void buoy_remove(BuoySystem *s, BodyId body);
Buoyancy *buoy_find(BuoySystem *s, BodyId body);
// Samples, computes (in parallel when jobs run) and applies the impulses; call before phys_step.
void buoy_step(BuoySystem *s, PhysicsWorld *pw, const BuoyWater *water, float dt);

#endif
