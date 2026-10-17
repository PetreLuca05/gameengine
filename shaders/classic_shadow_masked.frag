#version 450
// Alpha test of masked classic draws in the shadow views (the material texture's alpha).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
#include "classic_mask.glsl"
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 2) in vec2 v_uv;
void main() {
    if (cl_mask_alpha(tex, v_uv * max(pc.params.w, 1e-3)) < pc.model[3].y) discard;
}
