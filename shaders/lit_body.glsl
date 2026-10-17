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

layout(set = 1, binding = 0) uniform sampler2D tex;
#ifdef LIT_SURFACES
#include "surfaces.glsl"
#include "terrain/terrain.glsl"
#endif
#ifdef PBR_MAPS
#include "pbr_maps.glsl"
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

// Anti-aliased grid line intensity for coordinate c with spacing s and width w (in pixels).
float grid_line(float c, float s, float w) {
    float x = c / s;
    float d = abs(fract(x - 0.5) - 0.5) / max(fwidth(x), 1e-5);
    return 1.0 - clamp(d / w, 0.0, 1.0);
}

vec3 apply_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    vec2 axis_tint_sel;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.25, 0.8), grid_line(c.y, 0.25, 0.8));
    float major = max(grid_line(c.x, 1.0, 1.3), grid_line(c.y, 1.0, 1.3));
    float big   = max(grid_line(c.x, 5.0, 2.0), grid_line(c.y, 5.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.72, minor * 0.6);
    col = mix(col, col * 0.35, major);
    col = mix(col, vec3(0.9, 0.55, 0.1) * 0.5, big * 0.6);
    // Highlight the world origin axes on floors: X red, Z blue.
    if (an.y > 0.9) {
        float ax = grid_line(p.z, 1000.0, 2.5) * step(abs(p.z), 0.5);
        float az = grid_line(p.x, 1000.0, 2.5) * step(abs(p.x), 0.5);
        col = mix(col, vec3(0.8, 0.1, 0.1), ax);
        col = mix(col, vec3(0.1, 0.2, 0.9), az);
    }
    return col;
}

// Measuring grid for the recoil wall: 10 cm minor, 50 cm major, 1 m bold lines.
vec3 apply_fine_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.1, 0.9), grid_line(c.y, 0.1, 0.9));
    float half_m = max(grid_line(c.x, 0.5, 1.4), grid_line(c.y, 0.5, 1.4));
    float meter = max(grid_line(c.x, 1.0, 2.0), grid_line(c.y, 1.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.62, minor * 0.7);
    col = mix(col, vec3(0.1, 0.3, 0.6), half_m * 0.8);
    col = mix(col, vec3(0.75, 0.15, 0.1), meter);
    return col;
}

// Sighting-in target: 1 MOA squares at its distance (spacing s in metres), 5 MOA bold, 10 MOA red
// (level.c snaps the target centre onto a 10 MOA crossing; the aim square is its own box).
vec3 apply_moa_grid(vec3 base, vec3 p, vec3 n, float s) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    s = max(s, 1e-4);
    // Each level fades out once its squares get smaller than a few pixels (readable through a
    // scope, a clean board from far away).
    float px = max(length(fwidth(c)), 1e-6);
    float f1 = clamp((s / px - 4.0) / 4.0, 0.0, 1.0), f5 = clamp((s * 5.0 / px - 4.0) / 4.0, 0.0, 1.0), f10 = clamp((s * 10.0 / px - 4.0) / 4.0, 0.0, 1.0);
    float minor = max(grid_line(c.x, s, 0.8), grid_line(c.y, s, 0.8)) * f1;
    float five = max(grid_line(c.x, s * 5.0, 1.2), grid_line(c.y, s * 5.0, 1.2)) * f5;
    float ten = max(grid_line(c.x, s * 10.0, 1.6), grid_line(c.y, s * 10.0, 1.6)) * f10;
    vec3 col = base;
    col = mix(col, col * 0.55, minor * 0.8);
    col = mix(col, vec3(0.08, 0.1, 0.12), five * 0.85);
    col = mix(col, vec3(0.8, 0.08, 0.05), ten);
    return col;
}

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
    uint flags = MAT_FLAGS;
    vec3 n = normalize(v_normal);
    if (!LIT_FRONT_FACING) n = -n;
    vec4 base = MAT_COLOR * v_color;

    if ((flags & MAT_TEXTURED) != 0u) {
        vec4 t;
        if ((flags & MAT_TRIPLANAR) != 0u) {
            float s = MAT_PARAMS.w;
            vec3 w = abs(n);
            w /= (w.x + w.y + w.z);
            t = LIT_TEX(v_world.zy * s) * w.x + LIT_TEX(v_world.xz * s) * w.y + LIT_TEX(v_world.xy * s) * w.z;
        } else {
            t = LIT_TEX(v_uv * max(MAT_PARAMS.w, 1e-3));
        }
        base *= t;
    }
#ifdef LIT_ALPHA_MASK
    if (base.a < pbr_alpha_cutoff()) discard;
    base.a = 1.0;
#endif
    vec2 map_uv = v_uv * max(MAT_PARAMS.w, 1e-3);   // the albedo's uv (glTF maps share it)
#ifdef LIT_COVER
    if ((flags & MAT_COVER) != 0u) {
        cover_base(flags, LIT_COVER_IN, LIT_COVER_PIVOT, map_uv, base.rgb);
        // the back-face flip also turned the clump dome downward: blades are lit from above on both sides
        n = normalize(vec3(n.x, abs(n.y), n.z));
    }
#endif
#ifdef PBR_MAPS
    n = pbr_normal(flags, map_uv, v_world, n);
#endif
    float surf_rough = -1.0;   // >= 0: the surface textures' roughness replaces the material's
    float surf_metal = -1.0;   // >= 0: the metal layers' metallic replaces the material's
    float surf_ao = 1.0;       // the surface layers' baked AO (x SSAO)
#ifdef LIT_SURFACES
    if (frame.surf_info.y > 0.5) {
        if ((flags & MAT_TERRAIN) != 0u && frame.surf_map.w > 0.5) {
            if (frame.surf_info.z > 0.5) terrain_shade(v_world, n, base, 0.82 + 0.36 * v_color.a, surf_rough, surf_ao);   // docs/TERRAIN_SHADING.md
            else surf_terrain(v_world, n, base, 0.82 + 0.36 * v_color.a, surf_rough, surf_ao);
        }
        else if ((flags & MAT_LAYERED) != 0u) surf_layered(v_world, v_uv, v_color, n, base, surf_rough, surf_metal, surf_ao);
    }
    if ((flags & (MAT_TERRAIN | MAT_LAYERED)) != 0u) base.a = MAT_COLOR.a;   // the vertex alpha is a layer / variation
#endif
    if ((flags & MAT_CHECKER) != 0u) {
        vec2 c = floor(v_world.xz * 0.25);
        float k = mod(c.x + c.y, 2.0);
        base.rgb *= mix(0.8, 1.0, k);
    }
    if ((flags & MAT_GRID) != 0u && frame.params.x > 0.5) base.rgb = apply_grid(base.rgb, v_world, n);
    if ((flags & MAT_GRID_FINE) != 0u) base.rgb = apply_fine_grid(base.rgb, v_world, n);
    if ((flags & MAT_GRID_MOA) != 0u) base.rgb = apply_moa_grid(base.rgb, v_world, n, MAT_PARAMS.w);
#ifdef LIT_BASE_HOOK
    LIT_BASE_HOOK
#endif

    vec3 color;
    if ((flags & MAT_UNLIT) != 0u) {
        color = base.rgb;
    } else {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
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
        vec2 suv = gl_FragCoord.xy * frame.screen.zw;
        float ao = (!vm && frame.post_flags.x > 0.5) ? mix(1.0, texture(post_ao, suv).r, frame.post_params.z) : 1.0;
#ifdef PBR_MAPS
        ao *= pbr_occlusion(flags, map_uv);   // material AO x SSAO (indirect diffuse + specular occlusion)
#endif
        ao *= surf_ao;                        // the surface layers' baked AO, the same way
#ifdef LIT_COVER
        if (g_cv.on) {
            ao *= g_cv.ao;                    // clump AO (indirect only, as the material AO)
            g_spec_scale = g_cv.spec;
        }
#endif
        float contact = (!vm && frame.post_flags.y > 0.5) ? texture(post_contact, suv).r : 1.0;
        contact = mix(1.0, contact, frame.ssky0.y);   // contact shadows leave with the cast shadows (sky cutoff)
        // hemispheric ambient (indirect diffuse): AO only darkens indirect light
        float hemi = n.y * 0.5 + 0.5;
        vec3 ambient = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, hemi) * frame.sky_ambient.w;
        bool sky_env_on = frame.sky_flags.x > 0.5;
        if (sky_env_on) ambient = sky_irradiance(n) / PI * frame.sky_flags.y;   // time-of-day sky (Lambert: E / pi)
        if (!vm && frame.post_flags.w > 0.5) ambient += texture(post_ssgi, suv).rgb * frame.post_params.y;
        color = ambient * albedo * (1.0 - metal) * ao;
        // indirect specular: sky / ground by the reflection vector, replaced by SSR where it hit
        vec3 rv = reflect(-v, n);
        float ndv0 = max(dot(n, v), 1e-4);
        vec3 f0 = mix(vec3(0.04), albedo, metal);
        vec3 fr = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - ndv0, 5.0);
        vec3 env = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, rv.y * 0.5 + 0.5) * frame.sky_ambient.w;
        if (sky_env_on) env = textureLod(sky_env, sky_env_uv(rv), rough * frame.sky_flags.z).rgb * frame.sky_flags.y;
        vec3 spec_ind = env;
        if (!vm && frame.post_flags.z > 0.5) {
            vec4 ssr = texture(post_ssr, suv);
            spec_ind = mix(env, ssr.rgb, clamp(ssr.a * frame.post_params.x, 0.0, 1.0));
        }
        float spec_occ = clamp(pow(ndv0 + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
        color += spec_ind * fr * frame.post_params.w * spec_occ * g_spec_scale;
        // sun
        vec3 l = normalize(frame.sun_dir.xyz);
        float sh = shadow_factor(v_world, n) * contact * cloud_shadow_at(v_world);
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
        if (frame.fill_dir.w > 0.0) color += brdf(n, v, normalize(frame.fill_dir.xyz), albedo, rough, metal) * frame.fill_color.rgb * frame.fill_dir.w * moon_shadow(v_world, n);
        // spot lights (flashlights, cookie-projected)
        for (int i = 0; i < int(frame.spot_info.x); i++) {
            vec3 sl;
            vec3 sc = spot_light(i, v_world, sl);
            if (sc.r + sc.g + sc.b > 0.0) color += brdf(n, v, sl, albedo, rough, metal) * sc * spot_light_shadow(i, v_world, n);
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
            float lsh = dot(n, ldir) > 0.0 ? point_light_shadow(i, v_world, n) : 1.0;
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
        vec2 fuv = gl_FragCoord.xy * frame.screen.zw;
        float vz = max(dot(v_world - frame.camera_pos.xyz, -vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2])), 0.15);
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
    if (sh_view >= 2 && (flags & MAT_UNLIT) == 0u) color = shadow_debug_color(sh_view, v_world) * 0.6;
    out_color = vec4(color, base.a);
}
