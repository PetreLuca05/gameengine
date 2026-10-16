// VFX shared declarations (compute + graphics). Must match src/render/vk_vfx.c and
// src/vfx/vfx_sim.h (VfxGpuSpawn). VFX_SET: descriptor set index (0 in compute, 1 in graphics).
// Graphics stages see every buffer read-only (no vertexPipelineStoresAndAtomics needed).
#ifdef VFX_SET
#define VFX_RW readonly
#else
#define VFX_RW
#endif
#ifndef VFX_SET
#define VFX_SET 0
#endif

#define VFX_LUT_SAMPLES 32
#define VFX_CURVES 4
#define LUT_SIZE 0
#define LUT_COLOR 1
#define LUT_VEL 2
#define LUT_SPIN 3

// render modes / orient / lighting / collide / dir (src/vfx/vfx_types.h)
#define RM_ADD 0u
#define RM_ALPHA 1u
#define RM_DISTORT 2u
#define OR_BILLBOARD 0u
#define OR_STRETCH 1u
#define OR_CROSS 2u
#define OR_AXIS 3u
#define OR_NORMAL 4u
#define OR_SEGMENT 5u
#define LT_NONE 0u
#define LT_SIMPLE 1u
#define LT_SIXWAY 2u
#define EF_FRAME_OVER_LIFE 1u
#define EF_MOTION_VECTORS 2u
#define EF_KILL_ON_HIT 4u
#define EF_EXPOSURE 8u
#define EF_LOCAL 16u
#define EF_FRAME_RANDOM 32u
#define SF_NO_LIGHTING 1u
#define SF_NO_COLLIDE 2u
#define SF_VIEWMODEL 4u
#define PF_COLLIDED 8u

struct Emitter {
    vec4 spawn0;      // shape, radius, cone angle (deg), offset
    vec4 spawn1;      // box xyz, dir bias
    vec4 speed_life;  // speed min max, life min max
    vec4 size_rot;    // size min max, rotation min max (rad)
    vec4 spin_misc;   // spin min max (rad/s), inherit, dir mode
    vec4 color_a;
    vec4 color_b;
    vec4 phys0;       // gravity, drag, wind, turbulence
    vec4 phys1;       // turbulence freq, turbulence speed, bounce, friction
    vec4 render0;     // stretch, stretch max, soft distance, emissive
    vec4 render1;     // near fade, distortion, aspect, fps
    vec4 tex_uv;      // atlas rect u0 v0 u1 v1
    vec4 mv_uv;       // motion vectors rect
    vec4 lm0_uv;      // 6-way: right left top
    vec4 lm1_uv;      // 6-way: bottom back front
    vec4 nrm_uv;      // normal / distortion rect
    uvec4 modes;      // x render | orient << 4 | lighting << 8 | collide << 12 | flags << 16
                      // y pages: tex | mv << 8 | lm << 16 | nrm << 24
                      // z grid: cols | rows << 8 | frames << 16
                      // w lut row
    uvec4 sub;        // x sub on | count << 8, y z w: sub emitter + 1 (0 = none)
};

struct Spawn {
    vec4 pos_scale;
    vec4 rot;
    vec4 normal_energy;
    vec4 incoming_alpha;
    vec4 vel_time;
    uvec4 info;       // emitter, seed, count, variant (12) | flags (4) << 12 | instance << 16
    vec4 tint;        // rgb colour multiplier (per-character blood), w unused
};

struct SubSpawn { vec4 pos; vec4 normal; uvec4 info; };   // info.x emitter, y seed, z count

struct Counters {
    uint dead;
    uint alive[2];
    uint add;
    uint alpha;
    uint distort;
    uint sub;
    uint sub_pending;
    uvec4 update_args;
    uvec4 draw_add;       // vertexCount, instanceCount, firstVertex, firstInstance
    uvec4 draw_alpha;
    uvec4 draw_distort;
    uvec4 emitted;        // x particles emitted this frame, y dropped (pool full)
};

struct CpuSprite { vec4 pos_size; vec4 vel_rot; vec4 color; uvec4 info; };   // info.x emitter, y frame bits, z flags, w instance
struct Decal { vec4 pos_size; vec4 normal_depth; vec4 tangent_angle; vec4 uv; vec4 nuv; vec4 tint; uvec4 info; };

layout(std140, set = VFX_SET, binding = 0) uniform VfxFrame {
    mat4 view_proj;
    mat4 prev_view_proj;
    mat4 inv_view_proj;
    mat4 view;
    mat4 proj;
    vec4 cam_pos;       // xyz, w time
    vec4 cam_right;     // w = dt (scaled)
    vec4 cam_up;        // w = timescale
    vec4 cam_fwd;       // w = near fade master
    vec4 wind;          // xyz, w = exposure visibility (1 night .. small in daylight)
    vec4 screen;        // w, h, 1/w, 1/h
    vec4 depth_params;  // near, far, prev depth valid, collision thickness
    uvec4 counts;       // x cpu spawn particles, y spawn records, z sort n, w frame index
    uvec4 caps;         // x max particles, y max sort, z max distort, w flags (1 lighting, 2 collision)
    vec4 misc;          // x sort step j, y sort k (push in compute), z cpu sprite count, w decal count
} vf;

layout(std430, set = VFX_SET, binding = 1) VFX_RW buffer PPos { vec4 p_pos_age[]; };
layout(std430, set = VFX_SET, binding = 2) VFX_RW buffer PVel { vec4 p_vel_life[]; };
layout(std430, set = VFX_SET, binding = 3) VFX_RW buffer PMisc { vec4 p_misc[]; };      // size, rotation, spin, frame
layout(std430, set = VFX_SET, binding = 4) VFX_RW buffer PInfo { uvec4 p_info[]; };     // emitter | flags << 16, seed, color rgba8, instance | variant << 16
layout(std430, set = VFX_SET, binding = 5) VFX_RW buffer Dead { uint dead_list[]; };
layout(std430, set = VFX_SET, binding = 6) VFX_RW buffer AliveA { uint alive_a[]; };
layout(std430, set = VFX_SET, binding = 7) VFX_RW buffer AliveB { uint alive_b[]; };
layout(std430, set = VFX_SET, binding = 8) VFX_RW buffer CountersBuf { Counters ctr; };
layout(std430, set = VFX_SET, binding = 9) readonly buffer Emitters { Emitter emitters[]; };
layout(std430, set = VFX_SET, binding = 10) readonly buffer Luts { vec4 luts[]; };
layout(std430, set = VFX_SET, binding = 11) readonly buffer Spawns { Spawn spawns[]; };
layout(std430, set = VFX_SET, binding = 12) readonly buffer SpawnPrefix { uint spawn_first[]; };
layout(std430, set = VFX_SET, binding = 13) VFX_RW buffer ListAdd { uint list_add[]; };
layout(std430, set = VFX_SET, binding = 14) VFX_RW buffer SortKeys { uvec2 sort_keys[]; };
layout(std430, set = VFX_SET, binding = 15) VFX_RW buffer ListDistort { uint list_distort[]; };
layout(std430, set = VFX_SET, binding = 16) VFX_RW buffer SubSpawns { SubSpawn subs[]; };
layout(set = VFX_SET, binding = 17) uniform sampler2DArray vfx_atlas;
layout(set = VFX_SET, binding = 18) uniform sampler3D vfx_noise;
layout(set = VFX_SET, binding = 19) uniform sampler2D vfx_depth;
layout(set = VFX_SET, binding = 20) uniform sampler2D vfx_scene;
layout(std430, set = VFX_SET, binding = 21) readonly buffer CpuSprites { CpuSprite cpu_sprites[]; };
layout(std430, set = VFX_SET, binding = 22) readonly buffer Decals { Decal decals[]; };
layout(std430, set = VFX_SET, binding = 23) readonly buffer Instances { vec4 inst_xf[]; };   // [2i] pos, [2i+1] rot quat

// ------------------------------------------------------------------ helpers

uint vfx_hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float rand01(inout uint s) {
    s = vfx_hash(s + 0x9e3779b9U);
    return float(s >> 8) * (1.0 / 16777216.0);
}
float range2(vec2 r, inout uint s) { return r.x + (r.y - r.x) * rand01(s); }

vec3 quat_rot(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

vec4 lut(uint row, uint curve, float t) {
    float x = clamp(t, 0.0, 1.0) * float(VFX_LUT_SAMPLES - 1);
    uint i0 = uint(floor(x));
    uint i1 = min(i0 + 1u, uint(VFX_LUT_SAMPLES - 1));
    uint base = (row * VFX_CURVES + curve) * VFX_LUT_SAMPLES;
    return mix(luts[base + i0], luts[base + i1], fract(x));
}

uint pack_rgba8(vec4 c) {
    uvec4 u = uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24);
}
vec4 unpack_rgba8(uint p) { return vec4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, p >> 24) / 255.0; }

vec3 rand_unit(inout uint s) {
    float z = rand01(s) * 2.0 - 1.0, a = rand01(s) * 6.2831853;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), r * sin(a), z);
}
void basis(vec3 n, out vec3 t, out vec3 b) {
    vec3 up = abs(n.y) < 0.95 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}
vec3 cone_dir(vec3 axis, float angle_deg, inout uint s) {
    if (angle_deg <= 0.0) return axis;
    float ca = cos(min(angle_deg, 180.0) * 0.01745329);
    float z = 1.0 - rand01(s) * (1.0 - ca);
    float phi = rand01(s) * 6.2831853;
    float r = sqrt(max(0.0, 1.0 - z * z));
    vec3 t, b;
    basis(axis, t, b);
    return normalize(axis * z + t * (r * cos(phi)) + b * (r * sin(phi)));
}

// Flipbook cell rect inside a texture rect.
vec2 cell_uv(vec4 rect, uint grid, float cell, vec2 uv) {
    uint cols = max(grid & 255u, 1u), rows = max((grid >> 8) & 255u, 1u);
    uint c = uint(cell) % (cols * rows);
    vec2 cs = (rect.zw - rect.xy) / vec2(cols, rows);
    return rect.xy + cs * (vec2(c % cols, c / cols) + uv);
}
