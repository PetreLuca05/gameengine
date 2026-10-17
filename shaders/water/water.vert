#version 450
#extension GL_GOOGLE_include_directive : require
// Water surface (docs/WATER.md "Rendering"): world-space vertices of the lake / river / pool meshes
// (flat at the body level) and the ocean grid (displaced by the shared swell, water_waves.glsl).
#include "common.glsl"
#define WATER_SET 1
#include "water/water_common.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in uint in_body;

layout(push_constant) uniform WaterPush {   // = water_surface.glsl, vk_water.c WaterPush
    vec4 grid;      // ocean grid: xy snap origin (world xz), z 1 = ocean draw, w unused
    mat4 inv_vp;
    vec4 misc, misc2;
} pc;

layout(location = 0) out vec3 v_world;
layout(location = 1) flat out uint v_body;
layout(location = 2) out vec4 v_cur_clip;
layout(location = 3) out vec4 v_prev_clip;
layout(location = 4) out float v_jac;

void main() {
    vec3 p = in_pos;
    vec3 prev_p = in_pos;
    float jac = 1.0;
    uint body = in_body;
    if (pc.grid.z > 0.5) {
        // Ocean: the grid follows the camera in whole cells (no swimming vertices); the swell
        // displaces it (fading out towards the beach is done per pixel against the scene depth).
        p.xz += pc.grid.xy;
        p.y = W.ocean.y;
        body = uint(W.ocean.x);
        if (W.info.z > 0.0) {
            float dist = length(p.xz - frame.camera_pos.xz);
            float fade = 1.0 - smoothstep(1500.0, 3000.0, dist);   // the far ring stays flat
            vec2 g = p.xz;
            p += wv_displace(g, W.info.x) * fade;
            // Last frame's surface point of this vertex (TAA / motion blur see the waves move).
            prev_p = vec3(g.x, W.ocean.y, g.y) + wv_displace(g, W.ocean.z) * fade;
        } else {
            prev_p = p;
        }
    }
    v_world = p;
    v_body = body;
    v_jac = jac;
    gl_Position = frame.view_proj * vec4(p, 1.0);
    v_cur_clip = frame.view_proj_nj * vec4(p, 1.0);
    v_prev_clip = frame.prev_view_proj_nj * vec4(prev_p, 1.0);
}
