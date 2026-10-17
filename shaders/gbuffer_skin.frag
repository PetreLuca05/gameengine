#version 450
// G-buffer prepass for skinned characters (per-instance material, LOD cross-fade dither, the
// reconstruction dissolve: docs/REVIVE.md).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "recon.glsl"
#include "lod_fade.glsl"

layout(location = 4) flat in vec4 v_mat_color;
layout(location = 5) flat in vec4 v_mat_params;
layout(location = 6) flat in uint v_mat_flags;
layout(location = 9) flat in vec4 v_recon;

#define GB_FLAGS (v_mat_flags | GB_DYNAMIC_FLAG)
#define GB_PARAMS v_mat_params
#define GB_PRE_MAIN     float fade = v_mat_params.w;     if (lod_faded(fade, gl_FragCoord.xy)) discard;     if ((v_mat_flags & MAT_RECON) != 0u) {         vec3 ra = vec3(1.0), re;         if (!recon_eval(v_world, v_recon, false, ra, re)) discard;     }
#include "gbuffer_body.glsl"
