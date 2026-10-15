#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 4) flat in vec4 v_mat_color;
layout(location = 5) flat in vec4 v_mat_params;
layout(location = 6) flat in uint v_mat_flags;

#define MAT_COLOR v_mat_color
#define MAT_PARAMS vec4(v_mat_params.xyz, 1.0)
#define MAT_FLAGS v_mat_flags

// LOD cross-fade: complementary screen-space dither between the outgoing (fade < 0) and the
// incoming (fade > 0) LOD so the swap never pops.
#define LIT_PRE_MAIN     float fade = v_mat_params.w;     if (fade != 0.0) {         float n = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));         if (fade > 0.0 ? n >= fade : n < 1.0 + fade) discard;     }

#include "lit_body.glsl"
