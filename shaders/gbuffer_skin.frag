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
layout(location = 10) flat in uint v_normal_scale;

// PBR maps of textured skins (set 3, docs/CHARACTERS.md "PBR maps"): normal_scale per instance;
// occlusion strength 1, emissive colour white, no alpha cutoff.
#define PBR_PARAMS uvec4(v_normal_scale, floatBitsToUint(1.0), 0xFFFFFFFFu, 0u)
#define PBR_MAPS

#define GB_FLAGS (v_mat_flags | GB_DYNAMIC_FLAG)
#define GB_PARAMS vec4(v_mat_params.xyz, 1.0)   // w is the LOD fade here, not a uv scale
#define GB_PRE_MAIN     float fade = v_mat_params.w;     if (lod_faded(fade, gl_FragCoord.xy)) discard;     if ((v_mat_flags & MAT_RECON) != 0u) {         vec3 ra = vec3(1.0), re;         if (!recon_eval(v_world, v_recon, false, ra, re)) discard;     }
#include "gbuffer_body.glsl"
