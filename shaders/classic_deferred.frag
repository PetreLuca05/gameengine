#version 450
// Deferred lighting of classic content (PF-0039, shaders/classic_deferred.glsl): one full-screen pass
// lights every pixel a deferred classic material kept in the prepass, with the lit pass's own lighting
// (lit_body.glsl) on the terms that prepass stored, before the opaque pass. Other pixels are discarded:
// the opaque pass lights them (and draws over any pixel a later prepass draw took from classic content).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_deferred.glsl"
layout(set = 3, binding = 0) uniform sampler2D cld_normal;
layout(set = 3, binding = 1) uniform sampler2D cld_material;
layout(set = 3, binding = 2) uniform sampler2D cld_albedo;
layout(set = 3, binding = 3) uniform usampler2D cld_params;
layout(set = 3, binding = 4) uniform sampler2D cld_depth;
layout(push_constant) uniform CldPush { mat4 inv_view_proj; } cpc;   // the prepass's (jittered) view-projection, inverted

// lit_body.glsl's inputs, filled per pixel by cld_load (LIT_PRE_MAIN)
vec3 v_world, v_normal;
vec2 v_uv = vec2(0.0);
vec4 v_color;
vec4 cld_color;
uint cld_flags, cld_aob;
float cld_rough, cld_metal, cld_snow;
#define LIT_CUSTOM_INPUTS
#define LIT_FRONT_FACING true
#define LIT_TEX(uv) vec4(1.0)
#define MAT_COLOR cld_color
#define MAT_PARAMS vec4(0.0)     // no emissive (classic_deferred_ok); roughness / metallic come below
#define MAT_FLAGS cld_flags
#define MAT_AO_BYTE cld_aob      // with MAT_AO_BYTE_VERTEX: v_color.a carries the stored baked AO
#define LIT_COVER                // the translucency (g_cv from the stored terms; cover_base never runs)
#define LIT_COVER_IN vec4(0.0)
#define LIT_COVER_PIVOT vec3(0.0)
#define COVER_TEX(uv) vec4(0.0)
#define LIT_DEFERRED_MATERIAL float rough = cld_rough, metal = cld_metal; vec3 albedo = base.rgb; bool vm = false; float wx_snow = cld_snow;
void cld_load();
#define LIT_PRE_MAIN cld_load();
#include "lit_body.glsl"

void cld_load() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uvec4 prm = texelFetch(cld_params, p, 0);
    uint f = prm.w & 0xFFu;
    if ((f & CLD_DEFERRED) == 0u) discard;
    vec4 h = cpc.inv_view_proj * vec4(gl_FragCoord.xy * frame.screen.zw * 2.0 - 1.0, texelFetch(cld_depth, p, 0).r, 1.0);
    v_world = h.xyz / h.w;
    vec4 a = texelFetch(cld_albedo, p, 0);
    cld_color = vec4(a.rgb, 1.0);
    cld_rough = a.a;
    cld_metal = texelFetch(cld_material, p, 0).g;
    cld_snow = unpackHalf2x16(prm.y).x;
    v_color = vec4(1.0, 1.0, 1.0, unpackHalf2x16(prm.x).x);
    cld_aob = (prm.w >> CLD_AO_SHIFT) | MAT_AO_BYTE_VERTEX;
    cld_flags = ((f & CLD_UNLIT) != 0u ? MAT_UNLIT : 0u) | ((f & CLD_NO_RECEIVE) != 0u ? MAT_NO_RECEIVE : 0u);
    vec3 n = cld_oct_decode(texelFetch(cld_normal, p, 0).xy);
    if ((f & CLD_COVER) != 0u) {
        vec4 p2 = frame.cover[((f >> CLD_COVER_ID_SHIFT) & 7u) * 3u + 2u];
        g_cv.on = true;
        g_cv.thick = unpackHalf2x16(prm.z).x;
        g_cv.ao = 1.0;   // in the stored baked AO
        g_cv.sss = p2.x * frame.cover_info.z;
        g_cv.wrap = p2.y;
        g_cv.spec = p2.z;
        // The lit pass turns a blade facing down to face up before the weather; the G-buffer keeps the
        // card's normal (the screen-space passes read it). Turned here, then the weather's puddle normal
        // (the albedo and roughness after the weather are stored).
        if (n.y < 0.0) {
            n = normalize(vec3(n.x, -n.y, n.z));
            vec3 alb = vec3(0.5);
            float r = cld_rough;
            weather_surface(v_world, n, alb, r, cld_metal, false);
        }
    }
    v_normal = n;
}
