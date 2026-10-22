// Shared lit shading (lit.frag, skin.frag). The includer defines MAT_COLOR, MAT_PARAMS, MAT_FLAGS;
// PBR_MAPS adds the glTF material maps (shaders/pbr_maps.glsl), LIT_ALPHA_MASK the alphaMode MASK test.
#include "weather.glsl"
// LIT_CUSTOM_INPUTS: the includer provides v_world, v_normal, v_uv, v_color and LIT_FRONT_FACING
// itself (the vgeo lit resolve decodes them per pixel, shaders/vg_lit.frag).
#ifndef LIT_CUSTOM_INPUTS
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_color;
#define LIT_FRONT_FACING gl_FrontFacing
#endif
// The material texture (the vgeo lit resolve samples a bindless array instead).
#ifndef LIT_TEX
#define LIT_TEX(uv) texture(tex, (uv))
#endif
// A triplanar sample: uv from v_world, dx / dy its screen gradients from LIT_DPDX / LIT_DPDY (the vgeo
// lit resolve's analytic dP; it has no valid quad derivatives). Default: LIT_TEX (implicit derivatives,
// the gradient arguments are dropped unexpanded).
#ifndef LIT_TEX_W
#define LIT_TEX_W(uv, dx, dy) LIT_TEX(uv)
#endif

layout(set = 1, binding = 0) uniform sampler2D tex;
#ifdef LIT_SURFACES
#include "surfaces.glsl"
#include "terrain/terrain.glsl"
#endif
#ifdef PBR_MAPS
#include "pbr_maps.glsl"
#endif
// MAT_AO_BYTE: the material's AO byte (src/render/material_ao.h; default the pbr emissive word's alpha).
#ifndef MAT_AO_BYTE
#ifdef PBR_MAPS
#define MAT_AO_BYTE (PBR_PARAMS.z >> 24u)
#else
#define MAT_AO_BYTE 0u
#endif
#endif
// Screen-space inputs from the post stack (push descriptor, render resolution): ambient occlusion
// (indirect only), sun contact shadows, reflections (rgb + confidence), diffuse GI.
layout(set = 2, binding = 0) uniform sampler2D post_ao;
layout(set = 2, binding = 1) uniform sampler2D post_contact;
layout(set = 2, binding = 2) uniform sampler2D post_ssr;
layout(set = 2, binding = 3) uniform sampler2D post_ssgi;
layout(set = 2, binding = 4) uniform sampler3D post_froxel;   // integrated volumetric fog (blended draws)

layout(location = 0) out vec4 out_color;

#ifndef LIGHTING_PI
#define LIGHTING_PI
const float PI = 3.14159265;
#endif

#include "lit_surface_fn.glsl"

#include "lighting.glsl"
// LIT_COVER: ground cover shading (MAT_COVER materials only; the includer defines LIT_COVER_IN,
// LIT_COVER_PIVOT and COVER_TEX, shaders/cover.glsl).
#ifdef LIT_COVER
#include "cover.glsl"
#endif

void main() {
#ifdef LIT_PRE_MAIN
    LIT_PRE_MAIN
#endif
#include "lit_surface.glsl"

    vec3 color;
    if ((flags & MAT_UNLIT) != 0u) {
        color = base.rgb;
    } else {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
#ifdef LIT_DEFERRED_MATERIAL
        // The deferred lighting of classic content (classic_deferred.frag): the G-buffer holds the
        // material after the weather (albedo, roughness, metallic, normal, snow).
        LIT_DEFERRED_MATERIAL
#else
        float rough = clamp(surf_rough >= 0.0 ? surf_rough : MAT_PARAMS.x, 0.04, 1.0);
        float metal = clamp(surf_metal >= 0.0 ? surf_metal : MAT_PARAMS.y, 0.0, 1.0);
#ifdef PBR_MAPS
        pbr_metal_rough(flags, map_uv, rough, metal);
        rough = max(rough, 0.04);
#endif
        vec3 albedo = base.rgb;
        // Screen-space inputs (never on the viewmodel: it is excluded from the screen-space passes).
        bool vm = (flags & MAT_VIEWMODEL) != 0u;
        // Weather: wet / puddles / snow (shaders/weather.glsl; the G-buffer applies the same terms).
        float wx_snow = weather_surface(v_world, n, albedo, rough, metal, vm);
#endif
        vec2 suv = ss_main_uv(v_world, gl_FragCoord.xy * frame.screen.zw);
        // Ambient occlusion (docs/RENDERING_POST.md "Foliage-aware AO"), indirect light only (diffuse ambient,
        // SSGI, specular occlusion): the screen-space AO at the global intensity, weighted per material
        // (lerp(1, ao, ao_strength)), then the baked AO (vertex alpha, occlusion map, surface layers, grass
        // clump) multiplied in, or for foliage taken as min() (the card's baked term already holds the
        // self-occlusion GTAO sees; multiplying would count it twice).
        float ss_ao = (!vm && frame.post_flags.x > 0.5) ? mix(1.0, texture(post_ao, suv).r, frame.post_params.z) : 1.0;
        ss_ao = mix(1.0, ss_ao, mat_ao_strength(ao_b));
        float baked_ao = vertex_ao ? v_color.a : 1.0;
#ifdef PBR_MAPS
        baked_ao *= pbr_occlusion(flags, map_uv);   // material AO map
#endif
        baked_ao *= surf_ao;                        // the surface layers' baked AO
#ifdef LIT_COVER
        if (g_cv.on) {
            baked_ao *= g_cv.ao;                    // clump AO
            g_spec_scale = g_cv.spec;
        }
#endif
        float ao = (ao_b & MAT_AO_BYTE_MIN) != 0u ? min(ss_ao, baked_ao) : ss_ao * baked_ao;
        float contact = (!vm && frame.post_flags.y > 0.5) ? texture(post_contact, suv).r : 1.0;
        contact = mix(1.0, contact, frame.ssky0.y * frame.shadow_cfg3.w);   // contact shadows leave with the cast shadows (sky cutoff, Strength)
        bool receive = (flags & MAT_NO_RECEIVE) == 0u;   // Receive Shadows (docs/SHADOWS_UNITY_MODEL.md)
        if (!receive) contact = 1.0;
        // hemispheric ambient (indirect diffuse): AO only darkens indirect light
        float hemi = n.y * 0.5 + 0.5;
        vec3 ambient = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, hemi) * frame.sky_ambient.w;
        bool sky_env_on = frame.sky_flags.x > 0.5;
        if (sky_env_on) ambient = sky_irradiance(n) / PI * frame.sky_flags.y;   // time-of-day sky (Lambert: E / pi)
        float isky = interior_sky(vm ? frame.camera_pos.xyz : v_world);   // rooms see the sky through their glass only
        ambient = interior_bounce(ambient, isky);
        if (!vm && frame.post_flags.w > 0.5) ambient += texture(post_ssgi, suv).rgb * frame.post_params.y;
        color = ambient * albedo * (1.0 - metal) * ao;
        // indirect specular: sky / ground by the reflection vector, replaced by SSR where it hit
        vec3 rv = reflect(-v, n);
        float ndv0 = max(dot(n, v), 1e-4);
        vec3 f0 = mix(vec3(0.04), albedo, metal);
        vec3 fr = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - ndv0, 5.0);
        vec3 env = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, rv.y * 0.5 + 0.5) * frame.sky_ambient.w;
        if (sky_env_on) env = textureLod(sky_env, sky_env_uv(rv), rough * frame.sky_flags.z).rgb * frame.sky_flags.y;
        env = interior_bounce(env, isky);
        vec3 spec_ind = env;
        if (!vm && frame.post_flags.z > 0.5) {
            vec4 ssr = texture(post_ssr, suv);
            spec_ind = mix(env, ssr.rgb, clamp(ssr.a * frame.post_params.x, 0.0, 1.0));
        }
        float spec_occ = clamp(pow(ndv0 + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
        color += spec_ind * fr * frame.post_params.w * spec_occ * g_spec_scale;
        // sun
        vec3 l = normalize(frame.sun_dir.xyz);
        // The first-person rig: its own high-resolution tile + the world where it appears (lighting.glsl).
        float sh = receive ? (vm ? shadow_factor_vm(v_world, n) : shadow_factor(v_world, n)) * contact * cloud_shadow_at(v_world) : cloud_shadow_at(v_world);
        color += brdf(n, v, l, albedo, rough, metal) * frame.sun_color.rgb * frame.sun_dir.w * sh;
#ifdef LIT_COVER
        color += cover_translucency(n, v, l, albedo) * frame.sun_color.rgb * frame.sun_dir.w * sh;
#endif
        // Snow sparkle: tiny glints facing the light.
        if (wx_snow > 0.05) {
            float g = step(0.9965, wx_hash(floor(v_world.xz * 90.0) + floor(dot(v, vec3(13.0, 7.0, 3.0)) * 8.0)));
            color += vec3(g * wx_snow * 2.5) * frame.sun_color.rgb * frame.sun_dir.w * sh * max(dot(n, l), 0.0);
        }
        // Second light: the moon while the sun owns the cascades, shadowed in atlas tile 3.
        if (frame.fill_dir.w > 0.0) color += brdf(n, v, normalize(frame.fill_dir.xyz), albedo, rough, metal) * frame.fill_color.rgb * frame.fill_dir.w * (receive ? moon_shadow(v_world, n) : 1.0);
        // spot lights (flashlights, cookie-projected)
        for (int i = 0; i < int(frame.spot_info.x); i++) {
            vec3 sl;
            vec3 sc = spot_light(i, v_world, sl);
            if (sc.r + sc.g + sc.b > 0.0) color += brdf(n, v, sl, albedo, rough, metal) * sc * (receive ? spot_light_shadow(i, v_world, n) : 1.0);
        }
        // point lights
        int count = int(frame.ground_ambient.w);
        for (int i = 0; i < count; i++) {
            vec3 lp = frame.lights[i].pos_radius.xyz;
            float radius = frame.lights[i].pos_radius.w;
            vec3 dl = lp - v_world;
            float dist = length(dl);
            if (dist > radius) continue;
            vec3 ldir = dl / max(dist, 1e-4);
            float falloff = clamp(1.0 - pow(dist / radius, 4.0), 0.0, 1.0);
            falloff = falloff * falloff / (dist * dist + 1.0);
            float lsh = dot(n, ldir) > 0.0 && receive ? point_light_shadow(i, v_world, n) : 1.0;
            color += brdf(n, v, ldir, albedo, rough, metal) * frame.lights[i].color_intensity.rgb * frame.lights[i].color_intensity.w * falloff * lsh;
        }
    }
#ifdef PBR_MAPS
    if ((flags & MAT_EMISSIVE_TEX) != 0u) color += pbr_emissive(map_uv) * MAT_PARAMS.z;   // glTF: not tinted by the albedo
    else
#endif
    color += base.rgb * MAT_PARAMS.z; // emissive
#ifdef LIT_EMISSIVE_HOOK
    color += LIT_EMISSIVE_HOOK;
#endif
    if ((flags & (MAT_WINDOWS | MAT_FACADE)) != 0u) {
        bool grid = (flags & MAT_FACADE) != 0u;
        vec4 cw = city_window(v_world, normalize(v_normal), grid);
        if (grid) color *= 1.0 - 0.55 * cw.w;   // window rectangles read as dark glass by day
        color += cw.rgb;
    }

    if ((flags & MAT_HIGHLIGHT) != 0u) {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
        float rim = pow(1.0 - max(dot(n, v), 0.0), 2.0);
        float pulse = 0.5 + 0.5 * sin(frame.camera_pos.w * 6.0);
        color += vec3(1.0, 0.8, 0.3) * (0.15 + rim * 0.8) * (0.6 + 0.4 * pulse);
    }

    // exponential fog
    float fd = length(frame.camera_pos.xyz - v_world);
    float fog = 1.0 - exp(-fd * frame.fog.w);
    color = mix(color, frame.fog.rgb, clamp(fog, 0.0, 1.0));
#ifdef LIT_FROXEL_FOG
    // Blended surfaces are drawn after the volumetric fog pass: fog their own contribution with
    // the froxel volume (in-scattering + transmittance up to this depth).
    if (LIT_FROXEL_FOG && frame.post_fog.x > 0.5) {
        vec2 fuv = ss_main_uv(v_world, gl_FragCoord.xy * frame.screen.zw);
        float vz = max(ss_main_depth(v_world), 0.15);
        float w = clamp(log(vz / 0.15) / log(frame.post_fog.y / 0.15), 0.0, 1.0);
        vec4 fv = textureLod(post_froxel, vec3(fuv, w), 0.0);
        color = color * fv.a + fv.rgb;
    }
#endif

    if ((flags & MAT_CAUSTICS) != 0u) color += base.rgb * lit_caustic(v_world, frame.camera_pos.w) * 2.5;
    if ((flags & MAT_GLASS) != 0u) {
        // Glass: more opaque and brighter at grazing angles (Fresnel), clear face-on.
        vec3 vg = normalize(frame.camera_pos.xyz - v_world);
        float fr = pow(1.0 - abs(dot(n, vg)), 4.0);
        base.a = clamp(base.a + fr * 0.55, 0.0, 0.85);
        color += vec3(0.6, 0.85, 1.0) * fr * 0.6;
    }
    int sh_view = int(frame.ssky2.y);
    if (sh_view >= 2 && sh_view != 8 && (flags & MAT_UNLIT) == 0u) color = shadow_debug_color(sh_view, v_world, normalize(v_normal)) * 0.6;   // 8: lines only
    out_color = vec4(color, base.a);
}
