// Water pass data (src/render/vk_water.c GpuWaterData, std430): per-body look, the swell's active
// Gerstner components, the ripple sim window, interactive ripple sources. Shared by the surface
// shaders (set WATER_SET) and the compute passes (post layout, set 0).
#ifndef WATER_COMMON_GLSL
#define WATER_COMMON_GLSL

#ifndef WATER_SET
#define WATER_SET 0
#endif

#define WATER_MAX_BODIES 64
#define WATER_MAX_RIPPLES 64

struct WaterBodyGpu {
    vec4 absorb;     // rgb 1/m, w turbidity
    vec4 scatter;    // rgb 1/m, w roughness
    vec4 params;     // x level, y kind (0 puddle, 1 lake, 2 ocean, 3 river), z ripple / wave scale, w foam
    vec4 flow;       // xy flow (m/s), z wind coupling, w unused
};

layout(std430, set = WATER_SET, binding = 13) readonly buffer WaterData {
    vec4 info;       // x wave time (s), y body count, z wave count, w rain on water 0..1
    vec4 wind;       // xy unit heading of the wind, z speed (m/s), w tier (r_water)
    vec4 ripple;     // xy world xz of the sim window's min corner, z texel size (m), w texels per side
    vec4 ripple2;    // xy previous window min corner, z source count, w frame index
    vec4 ocean;      // x ocean body (-1 none), y still level, z last frame's wave time, w unused
    vec4 waves[64];  // 2 per active component: (dx, dz, k, omega), (amp, qa, phase, 0)
    WaterBodyGpu body[WATER_MAX_BODIES];
    vec4 rip[WATER_MAX_RIPPLES * 2];   // sources: (x, z, radius, strength m), (vx, vz, 0, 0)
} W;

#define WAVE_N uint(W.info.z)
#define WAVE_A(i) W.waves[2u * (i)]
#define WAVE_B(i) W.waves[2u * (i) + 1u]
#include "water/water_waves.glsl"

#endif
