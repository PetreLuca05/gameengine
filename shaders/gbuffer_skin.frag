#version 450
// G-buffer prepass for skinned characters (per-instance material, LOD cross-fade dither).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 4) flat in vec4 v_mat_color;
layout(location = 5) flat in vec4 v_mat_params;
layout(location = 6) flat in uint v_mat_flags;

#define GB_FLAGS (v_mat_flags | GB_DYNAMIC_FLAG)
#define GB_PARAMS v_mat_params
#define GB_PRE_MAIN     float fade = v_mat_params.w;     if (fade != 0.0) {         float n = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));         if (fade > 0.0 ? n >= fade : n < 1.0 + fade) discard;     }
#include "gbuffer_body.glsl"
