#version 450
// Alpha test of masked classic draws in the shadow views (the material texture's alpha) and the LOD /
// shadow distance cross-fade dither of fading parts (docs/LOD.md; complementary: one shadow, no gap).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
#include "classic_mask.glsl"
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 2) in vec2 v_uv;
layout(location = 9) flat in vec4 v_fade;
void main() {
    if (frame.lod_fade.z > 0.5 && cl_faded(v_fade, gl_FragCoord.xy)) discard;   // local light tiles: no dither (the cuts from before docs/LOD.md)
    if (pc.model[3].y > 0.0 && cl_mask_alpha(tex, v_uv * max(pc.params.w, 1e-3)) < pc.model[3].y) discard;
}
