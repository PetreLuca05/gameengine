// VFX runtime (pure: no renderer, no world). Effect instances, deterministic tick-based spawn
// scheduling, the CPU particle tier (Jolt collision through a callback), decals and effect lights,
// and the GPU spawn records the renderer consumes each frame.
#ifndef DG_VFX_SIM_H
#define DG_VFX_SIM_H

#include "vfx_library.h"

#define VFX_MAX_INSTANCES 2048
#define VFX_MAX_CPU_PARTICLES 4096
#define VFX_MAX_GPU_SPAWNS 8192
#define VFX_MAX_DECALS_HARD 4096
#define VFX_MAX_LIGHTS 64

// Spawn record for the GPU emit kernel. Must match GpuSpawn in shaders/vfx_common.glsl (std430).
typedef struct VfxGpuSpawn {
    vec4 pos_scale;         // spawn position, w = size scale
    vec4 rot;               // effect rotation (quat)
    vec4 normal_energy;     // surface normal, w = velocity / energy scale
    vec4 incoming_alpha;    // incoming direction, w = alpha / intensity scale
    vec4 vel_time;          // inherited velocity, w = sub-tick time offset (s)
    u32 info[4];            // x emitter, y seed, z count, w variant (12) | flags (4) << 12 | instance slot << 16
} VfxGpuSpawn;

enum VfxSpawnFlags {
    VFX_SF_NO_LIGHTING = 1u << 0,   // LOD: lit paths render unlit
    VFX_SF_NO_COLLIDE = 1u << 1,    // LOD: no depth collision
    VFX_SF_VIEWMODEL = 1u << 2,     // spawned from the first-person muzzle (near fade off)
};

typedef struct VfxSpawnParams {
    vec3 pos;
    quat rot;               // forward = rot * -Z (muzzle axis); identity = world up / -Z
    vec3 normal;            // hit normal (impacts)
    vec3 incoming;          // incoming direction (impacts), unit
    vec3 velocity;          // owner velocity (inherit)
    float scale;            // size scale (caliber)
    float count_mul;        // count multiplier (caliber, quality)
    float energy;           // speed multiplier (caliber)
    float intensity;        // alpha / light multiplier
    u32 seed;               // replicated seed for deterministic emitters (0 = local RNG)
    int material;           // SurfaceMaterial (impacts, CPU collision sounds)
    bool decal_ok;          // surface accepts decals (static world, not characters)
    bool viewmodel;
    int socket;             // > 0: parented, the owner updates the transform via vfx_set_transform
} VfxSpawnParams;

typedef struct VfxInstance {
    bool active;
    int effect;
    u32 handle;             // generation | slot
    VfxSpawnParams p;
    u32 seed;
    u32 start_tick;
    float age;              // s
    float duration;         // resolved lifetime of the spawning part (s)
    vec3 last_pos;
    float spawn_accum[VFX_MAX_EMITTERS_PER_EFFECT];
    int bursts_done[VFX_MAX_EMITTERS_PER_EFFECT];
    u32 spawn_counter[VFX_MAX_EMITTERS_PER_EFFECT];
    float lod_mul;
    float dist;
    int cpu_alive;          // CPU particles still alive (the instance lingers until they die)
} VfxInstance;

typedef struct VfxCpuParticle {
    bool alive;
    bool resting;
    u8 sound_played;
    int emitter;            // library emitter index
    int instance;           // owning instance slot (-1 once the instance is gone)
    vec3 pos, vel, prev_pos;
    quat rot;               // mesh orientation
    vec3 ang_vel;
    float age, life;
    float size, rotation, spin;
    vec4 color;
    float frame;
    u32 seed;
    int material;
    float rest_time;
    vec3 trail[8];          // ribbon history
    int trail_count;
} VfxCpuParticle;

typedef struct VfxDecal {
    bool alive;
    vec3 pos, normal;
    float angle;            // rotation around the normal
    float size, depth;
    float age, life;
    int texture, normal_tex;
    int variant, variants;
    vec4 tint;
    u32 serial;
} VfxDecal;

typedef struct VfxLight {
    vec3 pos, color;
    float radius, intensity;
    int priority;
    float score;
} VfxLight;

// Gameplay callbacks (game layer: Jolt raycasts, sounds).
typedef bool (*VfxRaycastFn)(void *ctx, vec3 from, vec3 delta, vec3 *hit, vec3 *normal, int *material);
typedef void (*VfxSoundFn)(void *ctx, const char *name, vec3 pos, float volume);

typedef struct VfxConfig {
    float quality_mul;      // count multiplier (vfx_quality)
    int decal_max;
    int max_cpu;
    float timescale;
    vec3 camera;
    vec3 wind;
    bool lighting;
} VfxConfig;

typedef struct VfxStats {
    int instances, cpu_particles, decals, lights;
    int gpu_spawned_frame, gpu_spawn_records;
    int evicted, culled;
    u32 flashes_spawned;   // core flash bursts (tests: one per shot)
} VfxStats;

typedef struct VfxSystem {
    const VfxLibrary *lib;
    VfxConfig cfg;
    VfxInstance inst[VFX_MAX_INSTANCES];
    u32 inst_gen;
    int inst_live;
    VfxCpuParticle cpu[VFX_MAX_CPU_PARTICLES];
    int cpu_count;          // high-water mark
    VfxDecal decals[VFX_MAX_DECALS_HARD];
    int decal_head, decal_count;
    u32 decal_serial;
    VfxLight lights[VFX_MAX_LIGHTS];
    int light_count;
    VfxGpuSpawn spawns[VFX_MAX_GPU_SPAWNS];
    int spawn_count;        // consumed (reset) by the renderer each frame
    int last_variant[VFX_MAX_EFFECTS][VFX_MAX_EMITTERS_PER_EFFECT];
    u32 tick;
    u32 local_seed;
    VfxStats stats;
    VfxRaycastFn raycast;
    VfxSoundFn sound;
    void *cb_ctx;
} VfxSystem;

void vfx_sys_init(VfxSystem *s, const VfxLibrary *lib);
void vfx_sys_clear(VfxSystem *s);
// Spawns an effect; returns a handle (0 = not spawned: culled, over budget, unknown effect).
u32 vfx_sys_spawn(VfxSystem *s, int effect, const VfxSpawnParams *p);
bool vfx_sys_alive(const VfxSystem *s, u32 handle);
void vfx_sys_set_transform(VfxSystem *s, u32 handle, vec3 pos, quat rot, vec3 velocity);
void vfx_sys_stop(VfxSystem *s, u32 handle);
// One fixed simulation step (dt already scaled by the timescale).
void vfx_sys_tick(VfxSystem *s, float dt);
// Lights for this frame, best first (budgeted).
int vfx_sys_collect_lights(VfxSystem *s, VfxLight *out, int max);
// Deterministic state hash of the CPU tier (tests).
u32 vfx_sys_cpu_hash(const VfxSystem *s);
// Adds a decal directly (blood splats behind hits).
void vfx_sys_add_decal(VfxSystem *s, const VfxDecalDef *d, vec3 pos, vec3 normal, float scale, u32 seed);
// Default spawn params (identity rotation, scale 1, ...).
VfxSpawnParams vfx_spawn_params(vec3 pos);

#endif
