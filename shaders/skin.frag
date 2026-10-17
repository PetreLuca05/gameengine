#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "recon.glsl"

layout(location = 4) flat in vec4 v_mat_color;
layout(location = 5) flat in vec4 v_mat_params;
layout(location = 6) flat in uint v_mat_flags;
layout(location = 9) flat in vec4 v_recon;

#define MAT_COLOR v_mat_color
#define MAT_PARAMS vec4(v_mat_params.xyz, 1.0)
#define MAT_FLAGS v_mat_flags

// LOD cross-fade: complementary screen-space dither between the outgoing (fade < 0) and the
// incoming (fade > 0) LOD so the swap never pops. Reconstruction (docs/REVIVE.md): the dissolve
// discards what is not built yet, the base hook tints the layer, the emissive hook adds the edge.
#define LIT_PRE_MAIN     float fade = v_mat_params.w;     if (fade != 0.0) {         float n = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));         if (fade > 0.0 ? n >= fade : n < 1.0 + fade) discard;     }     vec3 recon_emit = vec3(0.0);     vec3 recon_albedo = vec3(1.0);     bool recon_on = (v_mat_flags & MAT_RECON) != 0u;     if (recon_on && !recon_eval(v_world, v_recon, (v_mat_flags & MAT_RECON_FX) != 0u, recon_albedo, recon_emit)) discard;
#define LIT_BASE_HOOK     if (recon_on) { vec3 a = base.rgb, e; recon_eval(v_world, v_recon, false, a, e); base.rgb = a; }
#define LIT_EMISSIVE_HOOK recon_emit

#include "lit_body.glsl"
