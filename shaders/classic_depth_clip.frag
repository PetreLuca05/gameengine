#version 450
// Depth-only prepass of the deferred masked / distance-dithered classic draws (PF-0052, r_classic_zprepass):
// classic_gbuffer_def_clip.frag's test on the same texture and nothing else. Their G-buffer pass then
// runs classic_gbuffer_def_clip.frag at EQUAL depth without depth writes (early depth test): about one
// G-buffer shading per pixel instead of one per overlapping card.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
#include "classic_mask.glsl"
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 2) in vec2 v_uv;
layout(location = 9) flat in vec4 v_fade;
void main() {
    if (cl_faded(v_fade, gl_FragCoord.xy)) discard;
    if (pc.model[3].y > 0.0 && cl_mask_alpha(tex, v_uv * max(pc.params.w, 1e-3)) < pc.model[3].y) discard;
}
