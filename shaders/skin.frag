#version 450
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
#define PBR_PARAMS uvec4(v_normal_scale, floatBitsToUint(1.0), 0x00FFFFFFu, 0u)   // AO byte 0 (render/material_ao.h)
#define PBR_MAPS

#define LIT_THERMAL_BODY   // thermal scope: a character's surface comes from its heat volumes
#define MAT_COLOR v_mat_color
#define MAT_PARAMS vec4(v_mat_params.xyz, 1.0)
#define MAT_FLAGS v_mat_flags

// LOD cross-fade (shaders/lod_fade.glsl, docs/LOD.md): complementary dither between the outgoing
// (fade < 0) and the incoming (fade > 0) LOD so the swap never pops. Reconstruction (docs/REVIVE.md):
// the dissolve discards what is not built yet, the base hook tints the layer, the emissive hook adds
// the edge. r_lod_fade_debug tints fading draws.
#define LIT_PRE_MAIN     float fade = v_mat_params.w;     if (lod_faded(fade, gl_FragCoord.xy)) discard;     vec3 recon_emit = vec3(0.0);     vec3 recon_albedo = vec3(1.0);     bool recon_on = (v_mat_flags & MAT_RECON) != 0u;     if (recon_on && !recon_eval(v_world, v_recon, (v_mat_flags & MAT_RECON_FX) != 0u, recon_albedo, recon_emit)) discard;
#define LIT_BASE_HOOK     if (recon_on) { vec3 a = base.rgb, e; recon_eval(v_world, v_recon, false, a, e); base.rgb = a; }     base.rgb = lod_fade_debug_tint(base.rgb, fade);
#define LIT_EMISSIVE_HOOK recon_emit

#include "lit_body.glsl"
