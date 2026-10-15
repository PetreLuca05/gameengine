// VFX data model (pure, shared by the game, the renderer and the tests).
//
//   effect  = list of emitters (+ an optional effect light), loaded from assets/vfx/effects/*.json
//   emitter = spawn module + update modules + render module, parameterized by curves and seeded
//             random ranges
//
// Two simulation tiers:
//   GPU  mass particles: spawned by records the CPU queues each frame, simulated in compute
//        (vk_vfx.c, shaders/vfx_*.comp), never read back
//   CPU  few physics-interacting particles (casings, large debris, ribbons, deterministic
//        emitters): simulated here at the fixed tick with Jolt raycasts, rendered as meshes or as
//        CPU sprites through the same particle shaders
#ifndef DG_VFX_TYPES_H
#define DG_VFX_TYPES_H

#include "core/base.h"
#include "core/dgmath.h"

#define VFX_MAX_EFFECTS 256
#define VFX_MAX_EMITTERS_PER_EFFECT 12
#define VFX_MAX_EMITTERS 1024         // all effects
#define VFX_CURVE_KEYS 12
#define VFX_LUT_SAMPLES 32            // baked curve samples (GPU)
#define VFX_MAX_TEXTURES 256
#define VFX_NAME 48

// ------------------------------------------------------------------ curves / ranges

typedef enum VfxCurveType { VFX_CURVE_CONST, VFX_CURVE_LINEAR, VFX_CURVE_BEZIER } VfxCurveType;

// Up to 4 channels (scalar curves use x). Keys sorted by t in [0, 1].
typedef struct VfxCurve {
    u8 type;
    u8 channels;
    u8 key_count;
    float t[VFX_CURVE_KEYS];
    vec4 v[VFX_CURVE_KEYS];
    vec4 tan_in[VFX_CURVE_KEYS], tan_out[VFX_CURVE_KEYS];   // bezier (Hermite) tangents, dv/dt
} VfxCurve;

// Seeded random range: value = min + (max - min) * rand01.
typedef struct VfxRange { float min, max; } VfxRange;

// ------------------------------------------------------------------ surface materials

typedef enum SurfaceMaterial {
    SURF_DEFAULT = 0,   // untagged (treated as concrete)
    SURF_CONCRETE, SURF_BRICK, SURF_PLASTER, SURF_METAL_THIN, SURF_METAL_THICK, SURF_WOOD, SURF_DIRT,
    SURF_SAND, SURF_GRAVEL, SURF_GRASS, SURF_GLASS, SURF_WATER, SURF_FLESH, SURF_CLOTH, SURF_TILE, SURF_ASPHALT,
    SURF_COUNT
} SurfaceMaterial;

// ------------------------------------------------------------------ emitter definition

typedef enum VfxTier { VFX_TIER_GPU, VFX_TIER_CPU } VfxTier;
typedef enum VfxSpawnMode { VFX_SPAWN_BURST, VFX_SPAWN_RATE, VFX_SPAWN_DISTANCE } VfxSpawnMode;
typedef enum VfxShape { VFX_SHAPE_POINT, VFX_SHAPE_SPHERE, VFX_SHAPE_BOX, VFX_SHAPE_CONE, VFX_SHAPE_DISC, VFX_SHAPE_SURFACE } VfxShape;
// Base direction of the initial velocity (cone axis).
typedef enum VfxDir {
    VFX_DIR_SOCKET,     // effect transform forward (-Z): muzzle direction
    VFX_DIR_NORMAL,     // hit surface normal
    VFX_DIR_REFLECT,    // incoming direction reflected about the normal
    VFX_DIR_INCOMING,   // along the incoming direction (exit side / blood mist)
    VFX_DIR_UP,
    VFX_DIR_RANDOM,     // uniform sphere
    VFX_DIR_SIDE,       // perpendicular to the socket forward (compensator vents), alternating sides
    VFX_DIR_SURFACE,    // tangent to the surface, biased along the incoming direction (grazing hits)
} VfxDir;
typedef enum VfxRenderMode { VFX_RENDER_ADD, VFX_RENDER_ALPHA, VFX_RENDER_DISTORT, VFX_RENDER_MESH, VFX_RENDER_RIBBON, VFX_RENDER_NONE } VfxRenderMode;
typedef enum VfxOrient {
    VFX_ORIENT_BILLBOARD,  // camera facing, rotated by the particle rotation
    VFX_ORIENT_STRETCH,    // velocity stretched (sparks, chips)
    VFX_ORIENT_CROSS,      // camera facing + a quad containing the socket axis (flash cores)
    VFX_ORIENT_AXIS,       // quad containing the socket axis, rotated around it (side flashes)
    VFX_ORIENT_NORMAL,     // facing along the spawn normal (ripples, ground splats)
    VFX_ORIENT_SEGMENT,    // ribbon segment (CPU: pos -> pos + vel)
} VfxOrient;
typedef enum VfxLighting { VFX_LIGHT_NONE, VFX_LIGHT_SIMPLE, VFX_LIGHT_SIXWAY } VfxLighting;
typedef enum VfxCollide { VFX_COLLIDE_NONE, VFX_COLLIDE_DEPTH, VFX_COLLIDE_PHYSICS } VfxCollide;
typedef enum VfxSubOn { VFX_SUB_NONE, VFX_SUB_DEATH, VFX_SUB_COLLISION } VfxSubOn;

typedef struct VfxSpawnDef {
    u8 mode;                 // VfxSpawnMode
    VfxRange count;          // burst count (per burst)
    float rate;              // particles / s
    float per_meter;         // particles / m (trails)
    float delay, duration;   // emitter active window (s); duration 0 = burst at delay / rest of effect
    int bursts;              // burst repeats (>= 1)
    float burst_interval;
    u8 shape;                // VfxShape
    float radius;
    vec3 box;                // half extents
    float angle;             // cone half angle (deg)
    u8 dir;                  // VfxDir
    float dir_bias;          // VFX_DIR_REFLECT / SURFACE: 0..1 blend towards the normal
    VfxRange speed;
    float inherit;           // fraction of the effect velocity inherited
    VfxRange life;
    VfxRange size;
    VfxRange rotation;       // deg
    VfxRange spin;           // deg / s
    vec4 color_a, color_b;   // initial color: random lerp a..b (linear RGB, alpha)
    float offset;            // spawn offset along the base direction (m)
    bool local;              // particles live in the effect's space (parented to its socket)
    bool frame_random;       // random start frame (flipbooks) / variant
    bool variant_no_repeat;  // never the same variant twice in a row (flash cores)
} VfxSpawnDef;

typedef struct VfxUpdateDef {
    float gravity;           // multiplier of 9.81 m/s^2 (negative = rises)
    float drag;              // 1/s
    float wind;              // response to the global wind
    float turbulence, turb_freq, turb_speed;
    VfxCurve size_curve;     // multiplier over life
    VfxCurve color_curve;    // rgba multiplier over life
    VfxCurve velocity_curve; // speed multiplier over life
    VfxCurve spin_curve;     // spin multiplier over life
    bool frame_over_life;    // flipbook frame advances over the particle's life
    float fps;               // or at a fixed rate (frames / s) when > 0
    bool motion_vectors;     // blend flipbook frames with the motion-vector texture
    u8 collide;              // VfxCollide
    float bounce, friction;
    bool kill_on_hit;
    vec3 attractor;          // effect-local point
    float attract;           // strength (m/s^2), negative repels
    float vortex;            // swirl around the effect up axis (rad/s)
    u8 sub_on;               // VfxSubOn
    char sub_effect[VFX_NAME];
    int sub_effect_id;
    int sub_count;
} VfxUpdateDef;

typedef struct VfxRenderDef {
    u8 mode;                 // VfxRenderMode
    u8 orient;               // VfxOrient
    u8 lighting;             // VfxLighting
    char texture[VFX_NAME];
    int texture_id;          // index in the VfxLibrary texture table (-1 = soft round sprite)
    char normal_tex[VFX_NAME];
    int normal_tex_id;       // distortion / decal normal map
    char mesh[VFX_NAME];     // CPU mesh particles ("casing_rifle", "shard_concrete", ...)
    float stretch;           // seconds of velocity per unit length (VFX_ORIENT_STRETCH)
    float stretch_max;       // max length multiplier
    float soft;              // soft particle depth fade distance (m)
    float emissive;          // HDR multiplier (additive / emissive part)
    bool exposure_aware;     // fade in bright scenes (muzzle flashes)
    float near_fade;         // fade when closer to the camera than this (m)
    float distortion;        // refraction strength
    float aspect;            // quad width / height
    float light_radius, light_intensity;   // CPU tier: per particle light (budgeted)
    vec3 light_color;
} VfxRenderDef;

typedef struct VfxEmitterDef {
    char name[VFX_NAME];
    u8 tier;                 // VfxTier
    bool deterministic;      // replicated seed + tick clock (CPU tier)
    bool enabled;
    float lod_cull;          // emitter skipped beyond this distance (0 = never)
    u32 hash;
    VfxSpawnDef spawn;
    VfxUpdateDef update;
    VfxRenderDef render;
    int gpu_index;           // index in the GPU emitter table
} VfxEmitterDef;

typedef struct VfxLightDef {
    bool enabled;
    vec3 color;
    float intensity, radius;
    float duration;          // s
    bool exposure_aware;
} VfxLightDef;

typedef struct VfxDecalDef {
    bool enabled;
    char texture[VFX_NAME];
    char normal[VFX_NAME];
    int texture_id, normal_id;
    VfxRange size;           // m
    float lifetime;          // s (fades over the last 20 %)
    float depth;             // projection depth (m)
    int variants;            // texture variants (flipbook cells)
    vec4 tint;
} VfxDecalDef;

typedef struct VfxEffectDef {
    char name[VFX_NAME];
    char source[256];
    u32 hash;
    float duration;          // s (instance lifetime; 0 = until the last particle dies)
    bool loop;
    int priority;            // 0 low .. 3 critical (budget eviction order)
    int max_instances;
    float cull_distance;     // no spawning beyond (m)
    float lod_dist[3];       // spawn multipliers 1 / lod_mul[0..2] beyond these distances
    float lod_mul[3];
    VfxLightDef light;
    VfxDecalDef decal;
    int emitter_count;
    int emitters[VFX_MAX_EMITTERS_PER_EFFECT];   // indices into the library emitter table
    char sound[VFX_NAME];
} VfxEffectDef;

// Texture registry entry (from assets/vfx/tex/<name>.meta.json + PNG).
typedef struct VfxTextureMeta {
    char name[VFX_NAME];
    char file[128];
    int w, h;
    int cols, rows, frames;  // flipbook grid (1x1 for sprites)
    float fps;
    bool loop;
    int variants;            // independent sprites in the grid (variants instead of frames)
    bool motion_vectors;     // <name>_mv.png present
    int lighting;            // VfxLighting: 6-way maps <name>_lm0.png / <name>_lm1.png
    bool premultiplied;
    bool placeholder;        // generated locally (vfxgen --placeholder)
    // atlas placement (filled by the packer)
    int page;
    vec4 uv;                 // u0, v0, u1, v1 in the page
    int mv_page, lm_page;
    vec4 mv_uv, lm0_uv, lm1_uv;
} VfxTextureMeta;

#endif
