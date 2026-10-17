#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 4) in uvec4 in_joints;
layout(location = 5) in vec4 in_weights;
layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;   // w = shadow cascade
} pc;
layout(location = 0) flat out float v_fade;   // LOD / shadow-distance cross-fade (skin_shadow.frag; unused without it)

void main() {
    SkinInstance si = skin_inst.inst[gl_InstanceIndex];
    vec4 world = skin_matrix(si.info.x, in_joints, in_weights) * vec4(in_pos, 1.0);
    gl_Position = shadow_pass_vp(pc.flags.w) * world;
    v_fade = si.params.w;
    // A body still being reconstructed casts no shadow until it is mostly there (docs/REVIVE.md).
    if ((si.info.y & MAT_RECON) != 0u && si.recon.w < 0.85) gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
}
