#version 450
// Box-projected decals: one unit cube (36 vertices) per decal, instanced from the decal buffer.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define VFX_SET 1
#include "vfx_common.glsl"
layout(location = 0) flat out uint v_decal;
const vec3 k_corners[8] = vec3[8](vec3(-1, -1, -1), vec3(1, -1, -1), vec3(1, 1, -1), vec3(-1, 1, -1),
                                  vec3(-1, -1, 1), vec3(1, -1, 1), vec3(1, 1, 1), vec3(-1, 1, 1));
const uint k_idx[36] = uint[36](0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5);
void main() {
    Decal d = decals[gl_InstanceIndex];
    vec3 n = d.normal_depth.xyz;
    vec3 t = d.tangent_angle.xyz;
    vec3 b = cross(n, t);
    vec3 c = k_corners[k_idx[gl_VertexIndex]];
    float hs = d.pos_size.w * 0.5;
    vec3 wp = d.pos_size.xyz + t * c.x * hs + b * c.y * hs + n * c.z * d.normal_depth.w;
    v_decal = gl_InstanceIndex;
    gl_Position = frame.view_proj * vec4(wp, 1.0);
}
