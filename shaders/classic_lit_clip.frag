#version 450
// Classic lit pass of masked / dithered materials (depth EQUAL): the alpha test and fade dither run
// again, since distant cards (of one tree or of neighbours) can share a depth value with the
// surface the prepass kept.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
#include "classic_mask.glsl"
layout(location = 9) flat in float v_fade;
#define MAT_COLOR pc.color
#define MAT_PARAMS pc.params
#define MAT_FLAGS pc.flags.x
#define LIT_PRE_MAIN if (cl_faded(v_fade, gl_FragCoord.xy) || (pc.model[3].y > 0.0 && cl_mask_alpha(tex, v_uv * max(pc.params.w, 1e-3)) < pc.model[3].y)) discard;
#include "lit_body.glsl"
