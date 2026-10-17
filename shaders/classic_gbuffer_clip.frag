#version 450
// Classic prepass, masked (alpha test) and / or distance-dithered materials (classic_lit_clip.frag
// repeats the test at depth EQUAL).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define PBR_MAPS   // glTF material maps (set 5)
#include "classic_common.glsl"
#include "classic_mask.glsl"
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 9) flat in float v_fade;
bool cl_clip(vec2 uv) {
    if (cl_faded(v_fade, gl_FragCoord.xy)) return true;
    return pc.model[3].y > 0.0 && cl_mask_alpha(tex, uv * max(pc.params.w, 1e-3)) < pc.model[3].y;
}
#define GB_FLAGS pc.flags.x
#define GB_PARAMS pc.params
#define GB_PRE_MAIN if (cl_clip(v_uv)) discard;
#include "gbuffer_body.glsl"
