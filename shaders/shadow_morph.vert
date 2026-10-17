#version 450
// Shadow casters with the terrain geomorph (MAT_TERRAIN_MORPH, docs/LOD.md): the chunk morphs towards
// its next coarser LOD exactly as in lit.vert, so the shadow matches the drawn surface.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "lod_fade.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;   // w: chunk LOD + 8 x LOD distance (lod_terrain_vertex)
    uvec4 flags;
} pc;

void main() {
    vec3 wp = (pc.model * vec4(in_pos, 1.0)).xyz, n = in_normal;
    if ((pc.flags.x & MAT_TERRAIN_MORPH) != 0u) lod_terrain_vertex(wp, n, in_uv, pc.params.w);
    gl_Position = shadow_pass_vp(pc.flags.w) * vec4(wp, 1.0);
}
