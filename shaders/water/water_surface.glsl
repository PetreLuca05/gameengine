// Water surface shading body (water.frag, water_vel.frag: + the velocity attachment)
// (docs/WATER.md "Rendering"), one shader for lakes, rivers, pools and the
// ocean: Fresnel between
//   reflection   screen-space ray march against the pre-water depth (scene snapshot), falling back
//                to the sky environment (r_water >= 1)
//   refraction   the scene snapshot under the surface, offset by the normal (with a chromatic split),
//                attenuated by Beer-Lambert absorption over the underwater path and filled with the
//                medium's in-scattering; caustics on the lit bottom (r_water >= 2)
// plus the sun's GGX highlight, a sub-surface glow on wave crests (ocean), foam (shore, crest
// jacobian), a soft shoreline, rain rings, wind ripples and the interactive ripple sim. Writes HDR,
// depth (the attachment) and velocity.
#include "common.glsl"
#include "weather.glsl"
#include "lighting.glsl"
#define WATER_SET 1
#include "water/water_common.glsl"
#include "thermal.glsl"

layout(location = 0) in vec3 v_world;
layout(location = 1) flat in uint v_body;
layout(location = 2) in vec4 v_cur_clip;
layout(location = 3) in vec4 v_prev_clip;
layout(location = 4) in float v_jac;

layout(set = 1, binding = 0) uniform sampler2D scene_color;   // lit scene before the water (refraction, SSR)
layout(set = 1, binding = 1) uniform sampler2D scene_depth;   // its depth (R32F)
layout(set = 1, binding = 2) uniform sampler2D ripple_sim;    // R height (m), repeat-wrapped world texels
layout(set = 1, binding = 3) uniform sampler3D froxel;        // integrated volumetric fog

layout(push_constant) uniform WaterPush {
    vec4 grid;      // ocean grid origin, z 1 = ocean
    mat4 inv_vp;    // inverse of the jittered view-projection (depth -> world)
    vec4 misc;      // x SSR on, y ripple sim on, z caustics on, w refraction on
    vec4 misc2;     // x scene depth valid, y shore fade (m), z unused, w unused
} pc;

layout(location = 0) out vec4 out_color;
#ifdef WATER_VELOCITY
layout(location = 1) out vec2 out_velocity;
#endif

vec3 world_at(vec2 uv, float d) {
    vec4 w = pc.inv_vp * vec4(uv * 2.0 - 1.0, d, 1.0);
    return w.xyz / w.w;
}

// Value noise with derivatives (wind ripple detail).
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = wx_hash(i), b = wx_hash(i + vec2(1, 0)), c = wx_hash(i + vec2(0, 1)), d = wx_hash(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Wind ripples: a few short wind-driven wavelets + noise, slope (dh/dx, dh/dz) scaled by `amp`.
vec2 wind_slope(vec2 p, float t, vec2 wdir, float wspeed, float amp) {
    vec2 g = vec2(0.0);
    float s = clamp(wspeed / 8.0, 0.08, 1.4) * amp;
    vec2 perp = vec2(-wdir.y, wdir.x);
    const float lens[4] = float[4](1.7, 0.93, 0.51, 0.29);
    for (int i = 0; i < 4; i++) {
        vec2 dir = normalize(wdir + perp * (float(i) - 1.5) * 0.45);
        float k = 6.2831853 / lens[i];
        float w = sqrt(9.81 * k);
        float ph = k * dot(dir, p) - w * t + float(i) * 1.7;
        float a = 0.06 * lens[i] / 6.2831853;   // steepness 0.06
        g += dir * (-a * k * sin(ph));
    }
    // Break the repetition: noise slopes advected with the wind.
    vec2 q = p * 2.3 - wdir * t * 0.6;
    float e = 0.05;
    float n0 = vnoise(q), nx = vnoise(q + vec2(e, 0.0)), nz = vnoise(q + vec2(0.0, e));
    g += vec2(nx - n0, nz - n0) / e * 0.02;
    return g * s;
}

// Caustics: two warped interference layers (a cheap stand-in for the focused light of the waves),
// sharpened, ~1 at the bright lines, ~0 between.
float caustics(vec2 p, float t) {
    float c = 0.0;
    for (int i = 0; i < 2; i++) {
        vec2 q = p * (i == 0 ? 1.1 : 1.9) + float(i) * 7.3;
        q += 0.55 * vec2(sin(q.y * 1.3 + t * 0.9), cos(q.x * 1.2 - t * 0.8));
        q += 0.3 * vec2(sin(q.y * 2.6 - t * 1.3), cos(q.x * 2.3 + t * 1.1));
        float v = abs(sin(q.x * 2.0) * sin(q.y * 2.0));
        c += pow(1.0 - v, 6.0);
    }
    return c * 0.5;
}

// Screen-space reflection against the pre-water scene: exponential steps, a binary refine. rgb, a =
// confidence (0 off screen / missed / facing the camera).
vec4 trace_ssr(vec3 p, vec3 r) {
    float t = 0.12;
    vec3 prev = p;
    for (int i = 0; i < 22; i++) {
        vec3 q = p + r * t;
        vec4 c = frame.view_proj * vec4(q, 1.0);
        if (c.w <= 0.05) break;
        vec3 ndc = c.xyz / c.w;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float sd = textureLod(scene_depth, uv, 0.0).r;
        if (ndc.z > sd) {
            // Behind the scene: refine between prev and q, accept when close to the surface.
            vec3 a = prev, b = q;
            for (int k = 0; k < 5; k++) {
                vec3 m = (a + b) * 0.5;
                vec4 mc = frame.view_proj * vec4(m, 1.0);
                vec3 mn = mc.xyz / mc.w;
                vec2 muv = mn.xy * 0.5 + 0.5;
                if (mn.z > textureLod(scene_depth, muv, 0.0).r) b = m; else a = m;
            }
            vec4 bc = frame.view_proj * vec4(b, 1.0);
            vec2 buv = bc.xy / bc.w * 0.5 + 0.5;
            vec3 hit = world_at(buv, textureLod(scene_depth, buv, 0.0).r);
            float thick = length(hit - b);
            if (thick > 0.35 + t * 0.06) return vec4(0.0);
            vec2 edge = smoothstep(vec2(0.0), vec2(0.08), buv) * (1.0 - smoothstep(vec2(0.92), vec2(1.0), buv));
            float conf = edge.x * edge.y * (1.0 - float(i) / 22.0 * 0.5);
            return vec4(textureLod(scene_color, buv, 0.0).rgb, conf);
        }
        prev = q;
        t *= 1.32;
    }
    return vec4(0.0);
}

vec3 sky_reflection(vec3 r, float rough) {
    vec3 env = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, r.y * 0.5 + 0.5) * frame.sky_ambient.w;
    if (frame.sky_flags.x > 0.5) env = textureLod(sky_env, sky_env_uv(r), rough * frame.sky_flags.z).rgb * frame.sky_flags.y;
    return env;
}

void main() {
    WaterBodyGpu B = W.body[min(v_body, uint(WATER_MAX_BODIES - 1))];
    float tier = W.wind.w;
    float t = W.info.x;
    vec3 eye = frame.camera_pos.xyz;
    vec3 v = normalize(eye - v_world);
    bool below = !gl_FrontFacing;   // the camera under the surface sees its underside
    vec2 suv = gl_FragCoord.xy * frame.screen.zw;

    // ---- normal: swell (ocean) + wind ripples + rain rings + the interactive ripple sim
    vec3 n = vec3(0.0, 1.0, 0.0);
    float jac = 1.0;
    float kind = B.params.y;
    if (kind > 1.5 && kind < 2.5 && W.info.z > 0.0) {
        vec4 s = wv_sample(v_world.xz, t, jac);
        n = s.xyz;
    }
    vec2 slope = wind_slope(v_world.xz, frame.wx5.z, W.wind.xy, W.wind.z * B.flow.z, max(B.params.z, 0.15));
    float dist = length(eye - v_world);
    slope *= 1.0 - smoothstep(40.0, 160.0, dist);   // distant ripples average out (roughness takes over)
    if (pc.misc.y > 0.5) {
        float tx = W.ripple.z, size = W.ripple.w * tx;
        vec2 ruv = v_world.xz / size;
        vec2 rel = (v_world.xz - W.ripple.xy) / size;
        float in_win = smoothstep(0.0, 0.08, min(min(rel.x, rel.y), min(1.0 - rel.x, 1.0 - rel.y)));
        float h0 = texture(ripple_sim, ruv).r;
        float hx = texture(ripple_sim, ruv + vec2(1.0 / W.ripple.w, 0.0)).r;
        float hz = texture(ripple_sim, ruv + vec2(0.0, 1.0 / W.ripple.w)).r;
        slope += vec2(hx - h0, hz - h0) / tx * in_win * 1.5;
    }
    float rain = W.info.w * wx_exposed(v_world);
    if (rain > 0.01) {
        vec3 rn = wx_ripple_normal(v_world.xz, frame.wx1.z, rain);
        slope += -rn.xz / max(rn.y, 0.2) * (1.0 - smoothstep(10.0, 35.0, dist));
    }
    n = normalize(vec3(n.x / n.y - slope.x, 1.0, n.z / n.y - slope.y));
    if (below) n = -n;
    // Thermal scope PiP (docs/NIGHT_OPTICS.md): water is opaque in LWIR. It emits at its own (lagging)
    // temperature with an emissivity of ~0.96 looking down, falling with Fresnel towards grazing, where it
    // mirrors the cold sky: a lake reads cold at a distance.
    if (frame.thermal.x > 0.5) {
        float cv = clamp(dot(n, v), 0.0, 1.0);
        float e = frame.thermal_cls[THC_WATER].y * (1.0 - pow(1.0 - cv, 5.0));
        vec3 rv = reflect(-v, n);
        float tw = frame.thermal_cls[THC_WATER].x;
        float rad = e * th_radiance(tw) + (1.0 - e) * th_radiance(rv.y > 0.0 ? th_sky(rv.y) : frame.thermal.z);
        out_color = vec4(rad, tw, e, 1.0);
#ifdef WATER_VELOCITY
        out_velocity = vec2(0.0);
#endif
        return;
    }
    float rough = clamp(B.scatter.w + min(W.wind.z * 0.004, 0.06) + smoothstep(30.0, 400.0, dist) * 0.08, 0.02, 0.5);

    // ---- what is under the surface (the scene snapshot) and how much water the view crosses
    float d_scene = textureLod(scene_depth, suv, 0.0).r;
    vec3 p_scene = world_at(suv, d_scene);
    float path = d_scene >= 0.99999 ? 1e4 : max(dot(p_scene - v_world, -v), 0.0);
    float refr_k = pc.misc.w > 0.5 ? clamp(path * 0.5, 0.0, 1.0) * 0.035 : 0.0;
    vec2 off = n.xz * refr_k / max(dist * 0.05 + 0.5, 0.5);
    vec2 ruv = suv + off;
    float d_off = textureLod(scene_depth, ruv, 0.0).r;
    if (d_off < gl_FragCoord.z || any(lessThan(ruv, vec2(0.0))) || any(greaterThan(ruv, vec2(1.0)))) { ruv = suv; d_off = d_scene; }
    vec3 p_bottom = world_at(ruv, d_off);
    float path_r = d_off >= 0.99999 ? 1e4 : max(length(p_bottom - v_world), 0.0);
    vec3 refr;
    if (pc.misc.w > 0.5) {
        vec2 ca = off * 0.25;   // chromatic split of the refraction
        refr = vec3(textureLod(scene_color, ruv - ca, 0.0).r, textureLod(scene_color, ruv, 0.0).g, textureLod(scene_color, ruv + ca, 0.0).b);
    } else {
        refr = textureLod(scene_color, suv, 0.0).rgb;
    }

    // ---- light at the surface
    vec3 l = normalize(frame.sun_dir.xyz);
    float sh = shadow_factor(v_world, vec3(0, 1, 0)) * cloud_shadow_at(v_world);
    vec3 sun = frame.sun_color.rgb * frame.sun_dir.w;
    vec3 amb = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, 0.8) * frame.sky_ambient.w;
    if (frame.sky_flags.x > 0.5) amb = sky_irradiance(vec3(0, 1, 0)) / PI * frame.sky_flags.y;

    // ---- caustics on the lit bottom
    float depth_below = max(v_world.y - p_bottom.y, 0.0);
    if (pc.misc.z > 0.5 && path_r < 60.0 && !below) {
        float cst = caustics(p_bottom.xz + l.xz * depth_below * 0.5, t * 1.1);
        float fade = smoothstep(0.05, 0.6, depth_below) * exp(-depth_below * 0.25);
        refr *= 1.0 + cst * fade * 1.6 * sh * clamp(frame.sun_dir.w * 0.35, 0.0, 1.0) * exp(-B.absorb.g * depth_below);
    }

    // ---- the medium: Beer-Lambert over the path + single scattering of sun and sky
    vec3 sigma_s = B.scatter.rgb * (1.0 + B.absorb.w * 12.0);
    vec3 ext = B.absorb.rgb + sigma_s;
    vec3 trans = exp(-ext * path_r);
    vec3 albedo_m = sigma_s / max(ext, vec3(1e-4));
    vec3 inscatter = albedo_m * (1.0 - trans) * (sun * sh * 0.3 * max(l.y, 0.0) + amb);
    vec3 under = refr * trans + inscatter;

    // ---- reflection
    vec3 r = reflect(-v, n);
    if (!below && r.y < 0.02) r.y = 0.02;   // never reflect the underside of the surface
    vec3 refl = sky_reflection(r, rough);
    if (pc.misc.x > 0.5 && !below) {
        vec4 ssr = trace_ssr(v_world, normalize(r));
        refl = mix(refl, ssr.rgb, ssr.a);
    }
    float ndv = max(dot(n, v), 1e-3);
    float fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
    if (below) {
        // From below: total internal reflection past the critical angle (~48.6 deg), the sky in
        // Snell's window inside it.
        float cos_c = sqrt(1.0 - 1.0 / (1.333 * 1.333));
        fresnel = ndv < cos_c ? 1.0 : 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
        refl = (B.scatter.rgb / max(ext, vec3(1e-4))) * amb * 0.8;   // the deep water around reflected
    }
    vec3 color = mix(under, refl, fresnel);

    // ---- sun highlight (GGX) and the sub-surface glow on wave crests
    if (!below) {
        vec3 h = normalize(v + l);
        float ndl = max(dot(n, l), 0.0), ndh = max(dot(n, h), 0.0), vdh = max(dot(v, h), 0.0);
        float a = max(rough * rough, 0.0015);
        float spec = d_ggx(ndh, a) * v_smith(ndv, ndl, a) * (0.02 + 0.98 * pow(1.0 - vdh, 5.0)) * ndl;
        color += sun * sh * min(spec, 200.0);
        if (kind > 1.5 && kind < 2.5) {
            float crest = clamp((v_world.y - W.ocean.y) * 0.6, 0.0, 1.0);
            float back = pow(max(dot(-v, l) * 0.5 + 0.5, 0.0), 4.0);
            color += albedo_m * sun * sh * crest * back * 0.6;
        }
    }

    // ---- foam: shoreline (thin water), folding crests (ocean jacobian)
    float foam_amt = B.params.w;
    float shore = 1.0 - smoothstep(0.0, 0.35 + 0.25 * vnoise(v_world.xz * 1.7 + t * 0.2), path);
    float crest_foam = (kind > 1.5 && kind < 2.5) ? smoothstep(0.55, 0.15, jac) : 0.0;
    float foam = clamp((shore * 0.55 + crest_foam) * foam_amt, 0.0, 1.0) * (0.55 + 0.45 * vnoise(v_world.xz * 4.0 - t * 0.3));
    if (!below && foam > 0.0) color = mix(color, (sun * sh * max(l.y, 0.0) + amb) * 0.8, foam);

    // ---- fog (the opaque scene is already fogged; this is the surface's own share)
    float fog = 1.0 - exp(-dist * frame.fog.w);
    color = mix(color, frame.fog.rgb, clamp(fog, 0.0, 1.0));
    if (frame.post_fog.x > 0.5) {
        float vz = max(ss_main_depth(v_world), 0.15);   // the scope PiP: the main view's volume (common.glsl)
        float w = clamp(log(vz / 0.15) / log(frame.post_fog.y / 0.15), 0.0, 1.0);
        vec4 fv = textureLod(froxel, vec3(ss_main_uv(v_world, suv), w), 0.0);
        color = color * fv.a + fv.rgb;
    }

    // ---- soft shoreline: the water fades in over the first centimetres of depth (the snapshot is
    // already fogged)
    float edge = below ? 1.0 : smoothstep(0.0, pc.misc2.y, path);
    color = mix(textureLod(scene_color, suv, 0.0).rgb, color, edge);

    out_color = vec4(max(color, vec3(0.0)), 1.0);
#ifdef WATER_VELOCITY
    vec2 cur = v_cur_clip.xy / v_cur_clip.w, prev = v_prev_clip.xy / v_prev_clip.w;
    out_velocity = (cur - prev) * 0.5;
#endif
}
