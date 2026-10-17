// Shared lighting functions (lit_body.glsl, water/water.frag): time-of-day sky irradiance /
// environment, the cloud shadow, the cascaded sun shadow, the GGX BRDF. Needs common.glsl (frame
// set) included before.
#ifndef LIGHTING_GLSL
#define LIGHTING_GLSL
#ifndef LIGHTING_PI
#define LIGHTING_PI
const float PI = 3.14159265;
#endif

// Time-of-day sky (src/render/sky): irradiance from the SH9 of the environment, the
// environment itself (octahedral, mips by roughness), the cloud shadow on the ground plane.
vec3 sky_irradiance(vec3 d) {
    const float A0 = 3.14159265, A1 = 2.0943951, A2 = 0.785398163;
    vec3 e = sky_sh.sh[0].rgb * 0.282095 * A0;
    e += (sky_sh.sh[1].rgb * d.y + sky_sh.sh[2].rgb * d.z + sky_sh.sh[3].rgb * d.x) * 0.488603 * A1;
    e += (sky_sh.sh[4].rgb * 1.092548 * d.x * d.y + sky_sh.sh[5].rgb * 1.092548 * d.y * d.z + sky_sh.sh[6].rgb * 0.315392 * (3.0 * d.z * d.z - 1.0) +
          sky_sh.sh[7].rgb * 1.092548 * d.x * d.z + sky_sh.sh[8].rgb * 0.546274 * (d.x * d.x - d.y * d.y)) * A2;
    return max(e, vec3(0.0));
}
vec2 sky_env_uv(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = n.y >= 0.0 ? n.xz : (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}
float cloud_shadow_at(vec3 p) {
    if (frame.sky_flags.w < 0.5) return 1.0;
    vec2 uv = (p.xz - frame.cshadow.xy) * frame.cshadow.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    return textureLod(sky_cloud_shadow, uv, 0.0).r;
}

// Sky-driven shadows (src/render/shadow_sky.c, docs/SHADOWS.md). The source's angular radius
// (frame.ssky0.x = tan r) sets the penumbra: half width = blocker distance x tan r (PCSS: blocker
// search, then a filter of that radius, both converted from metres to the tile's texels, so the
// penumbra does not change across cascades). Every tap compares against the receiver's plane at
// the tap (depth gradient from the normal): wide kernels on surfaces sloped to the light do not
// shadow themselves. tests/test_shadow_sky.c runs the same kernel on the CPU.
float g_sh_pen = 0.0, g_sh_block = -1.0;   // r_shadow_debug: penumbra half width, blocker distance (m)
int g_sh_casc = -1;

float sh_ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
vec2 sh_vogel(int i, int n, float rot) {
    float r = sqrt((float(i) + 0.5) / float(n)), t = float(i) * 2.39996323 + rot;
    return r * vec2(cos(t), sin(t));
}

// d(depth NDC) / d(atlas uv) of the plane through the receiver with normal n under the ortho
// matrix M; clamped to `limit` (grazing surfaces).
vec2 sh_receiver_gradient(mat4 M, vec3 n, float limit) {
    vec3 t1 = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(n, t1);
    vec3 a = mat3(M) * t1, b = mat3(M) * t2;
    float det = a.x * b.y - b.x * a.y;
    if (abs(det) < 1e-12) return vec2(0.0);
    vec2 g = vec2(a.z * b.y - b.z * a.y, b.z * a.x - a.z * b.x) / det * 2.0;   // uv = xy * 0.5 + 0.5
    float l = length(g);
    return l > limit ? g * (limit / l) : g;
}

// One filter for every map (docs/SHADOWS.md "The filter", docs/SHADOWS_UNITY_MODEL.md "Shadow Type"):
//   mode 1 Hard   one bilinear hardware-compare fetch: the crisp, aliased Unity "Hard Shadows" edge
//   mode 2 Soft   URP's tent of `tent` texels (Soft Shadow Quality Low / Medium / High = 3 / 5 / 7):
//                 a uniform penumbra in texels whatever the distance; the sky widens it with the
//                 light size (the half width the sky gives at shadow_cfg.z metres, up to 7 texels)
//   mode 3 Ultra  PCSS: blocker search, then a rotated Vogel disc of d x tan r (contact hardening)
// Every fetch compares against the receiver's plane at the fetch (depth gradient g).
//   smap / sdepth   compare / raw views of the map; texel: 1 / map size; [tmin, tmax]: the tile in uv
float sh_filter(sampler2DShadow smap, sampler2D sdepth, float texel, vec2 uv, float z, vec2 g, vec2 tmin, vec2 tmax, float mpt, float span, float tan_r,
                float mode, float tent, float pcss_w, float rot) {
    g_sh_block = -1.0;
    if (mode < 1.5) {
        g_sh_pen = 0.5 * mpt;
        return texture(smap, vec3(clamp(uv, tmin, tmax), z));
    }
    if (mode < 2.5) {
        float n = tent;
        if (frame.shadow_cfg2.x <= 0.0) n = clamp(max(tent, 2.0 * frame.shadow_cfg.z * tan_r / mpt), tent, 7.0);   // the sky's light size (era 2010: fixed)
        g_sh_pen = 0.5 * n * mpt;
        vec2 tc = uv / texel;
        vec4 pu, wu, pv, wv;
        int nu = sh_tent_axis(tc.x, n, pu, wu);
        int nv = sh_tent_axis(tc.y, n, pv, wv);
        float sum = 0.0;
        for (int j = 0; j < nv; j++)
            for (int i = 0; i < nu; i++) {
                vec2 f = vec2(pu[i], pv[j]) * texel;
                sum += wu[i] * wv[j] * texture(smap, vec3(clamp(f, tmin, tmax), z + dot(g, f - uv)));
            }
        return sum;
    }
    float max_tex = max(frame.ssky1.y, frame.shadow_cfg.w), min_tex = frame.shadow_cfg.w;
    float dblock = frame.shadow_cfg.z;
    if (pcss_w > 0.0) {
        const int nb = 16;
        float search = clamp(frame.ssky1.z * tan_r / mpt, 1.0, max_tex * 1.5);
        float bsum = 0.0, bcnt = 0.0;
        for (int i = 0; i < nb; i++) {
            vec2 o = sh_vogel(i, nb, rot) * search * texel;
            float zt = z + dot(g, o);
            float d = textureLod(sdepth, clamp(uv + o, tmin, tmax), 0.0).r;
            if (d < zt) { bsum += zt - d; bcnt += 1.0; }
        }
        // No blocker within the search (wider than any kernel below): fully lit.
        if (bcnt < 0.5) { g_sh_pen = 0.0; return 1.0; }
        g_sh_block = bsum / bcnt * span;
        dblock = mix(dblock, g_sh_block, pcss_w);
    }
    float pen = frame.shadow_cfg2.x > 0.0 ? frame.shadow_cfg2.x : dblock * tan_r;
    float w = clamp(pen / mpt, min_tex, max_tex);
    g_sh_pen = w * mpt;
    const int nf = 16;
    float sum = 0.0;
    for (int i = 0; i < nf; i++) {
        vec2 o = sh_vogel(i, nf, rot) * w * texel;
        sum += texture(smap, vec3(clamp(uv + o, tmin, tmax), z + dot(g, o)));
    }
    return sum / float(nf);
}

// The PCSS disc's rotation: interleaved gradient noise per pixel; a new pattern per frame only while
// TAA / TAA-U resolve it (shadow_cfg.x), otherwise a fixed per-pixel pattern (no flicker under FXAA
// / SMAA / no AA). Hard and tent are deterministic.
float sh_rotation() {
    float f = frame.shadow_cfg.x > 0.5 ? mod(floor(frame.camera_pos.w * 60.0), 64.0) : 0.0;
    return sh_ign(gl_FragCoord.xy + 5.588238 * f) * 6.2831853;
}

// sin of the angle between the normal and the light: the normal offset grows at grazing angles.
float sh_sin_nl(vec3 n, vec3 l) { float c = dot(n, l); return sqrt(clamp(1.0 - c * c, 0.0, 1.0)); }

// Receiver plane slope limit (depth per lateral metre): ~80 deg, ~85 deg at a low sun.
float sh_slope_limit() { return 6.0 + 3.0 * (frame.shadow_cfg2.y - 1.0); }

// One cascade of the 2 x 2 shadow atlas: -1 when p is outside its tile. Bias from the cascade's
// texel size and the sun's Bias / Normal Bias (render.c / shadow_settings.c): normal offset x
// sin(n, l), constant, one texel of slope.
float shadow_cascade(int c, vec3 p, vec3 n) {
    vec3 l = normalize(frame.sun_dir.xyz);
    vec3 offset_p = p + n * (frame.shadow_nbias[c] * sh_sin_nl(n, l));
    vec4 sp = frame.shadow_vps[c] * vec4(offset_p, 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    vec2 tile = vec2(c & 1, c >> 1) * 0.5;
    vec2 local = (uv - tile) * 2.0;
    float texel = frame.params.y;
    float edge = 4.0 * texel;
    if (any(lessThan(local, vec2(edge))) || any(greaterThan(local, vec2(1.0 - edge))) || sp.z > 1.0 || sp.z < 0.0) return -1.0;
    float mpt = frame.shadow_mpt[c], span = frame.shadow_span[c];
    vec2 g = sh_receiver_gradient(frame.shadow_vps[c], n, sh_slope_limit() * mpt / (texel * span));
    float z = sp.z - frame.shadow_bias[c] - texel * length(g);
    g_sh_casc = c;
    vec2 tmin = tile + texel * 0.5, tmax = tile + 0.5 - texel * 0.5;
    return sh_filter(shadow_map, shadow_depth, texel, uv, z, g, tmin, tmax, mpt, span, frame.ssky0.x, frame.shadow_cfg4.x, frame.shadow_cfg4.y, frame.shadow_cfg.y,
                     sh_rotation());
}

// The receiver's selection distance: Stable Fit by the distance from the camera (the cascades are
// spheres), Close Fit by the view depth (the cascades are frustum slices).
float sh_select_distance(vec3 p) {
    vec3 d = p - frame.camera_pos.xyz;
    if (frame.shadow_cfg3.y > 0.5) return length(d);
    return dot(d, -vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2]));
}

// The world cascades without the sky fade or the Strength: the cascade by the selection distance,
// the last Cascade Border of each one blends into the next, the Shadow Distance (shadow_cfg2.z) fades
// out over the last border. Wide mode (ssky0.z < 1, Ultra): cascade 0 fades into cascade 1.
float shadow_world(vec3 p, vec3 n) {
    int count = int(frame.shadow_cfg3.x);
    float d = sh_select_distance(p);
    float D = frame.shadow_cfg2.z, border = frame.shadow_cfg3.z;
    float fade = 1.0 - smoothstep(D * (1.0 - border), D, d);
    if (fade <= 0.0) return 1.0;
    float w0 = count > 1 ? frame.ssky0.z : 1.0;
    int first = w0 <= 0.0 ? 1 : 0;
    float s = 1.0;
    for (int c = first; c < count; c++) {
        float split = frame.shadow_splits[c];
        if (d > split && c < count - 1) continue;
        float sc = shadow_cascade(c, p, n);
        if (sc < 0.0) {
            if (c < count - 1) continue;
            sc = 1.0;
        }
        float blend = c < count - 1 ? smoothstep(split * (1.0 - border), split, d) : 0.0;
        if (c == 0 && w0 < 1.0) blend = max(blend, 1.0 - w0);
        if (blend > 0.0) {
            float s2 = shadow_cascade(c + 1, p, n);
            if (s2 >= 0.0) sc = mix(sc, s2, blend);
        }
        s = sc;
        break;
    }
    return mix(1.0, s, fade);
}

// The sun's shadow term: Strength (shadow_cfg3.w) and the sky fade both blend it towards lit
// (Unity: Strength blends with the light, never with the ambient).
float shadow_factor(vec3 p, vec3 n) {
    if (frame.sun_color.w < 0.5 || frame.ssky0.y <= 0.0) return 1.0;   // off, or under the sky cutoff
    return mix(1.0, shadow_world(p, n), frame.ssky0.y * frame.shadow_cfg3.w);
}

// The first-person rig (docs/SHADOWS.md "The viewmodel tile"). Its own tile (bindings 16 / 17,
// shadow_vp_vm): arms, held item and attachments as casters at their drawn transforms, around
// 1 mm texels, with the directional light's filter (Hard / tent / PCSS) and Strength. The world
// cascades are sampled where the rig appears in the world (r_viewmodel_fov scales view x / y by
// shadow_cfg2.w), ignoring casters within shadow_vm2.z of the receiver along the light: the owner's
// body copy (MAT_SHADOW_ONLY, a third-person pose that does not match the first-person arms)
// never shadows the hands.
int g_sh_vm = -1;   // r_shadow_debug 7: -1 not the viewmodel, 0 outside its tile, 1 inside
float shadow_vm_tile(vec3 p, vec3 n) {
    g_sh_vm = 0;
    if (frame.shadow_vm.z <= 0.0) return 1.0;
    vec3 l = normalize(frame.sun_dir.xyz);
    vec4 sp = frame.shadow_vp_vm * vec4(p + n * (frame.shadow_vm.w * sh_sin_nl(n, l)), 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    float texel = frame.shadow_vm2.x;
    float edge = 2.0 * texel;
    if (any(lessThan(uv, vec2(edge))) || any(greaterThan(uv, vec2(1.0 - edge))) || sp.z > 1.0 || sp.z < 0.0) return 1.0;
    g_sh_vm = 1;
    float mpt = frame.shadow_vm.x, span = frame.shadow_vm.y;
    vec2 g = sh_receiver_gradient(frame.shadow_vp_vm, n, sh_slope_limit() * mpt / (texel * span));
    float z = sp.z - frame.shadow_vm2.y - texel * length(g);
    return sh_filter(shadow_vm_map, shadow_vm_depth, texel, uv, z, g, vec2(texel * 0.5), vec2(1.0 - texel * 0.5), mpt, span, frame.ssky0.x, frame.shadow_cfg4.x,
                     frame.shadow_cfg4.y, frame.shadow_vm2.w, sh_rotation());
}

// Where a viewmodel point appears in the world (the viewmodel is drawn with its own FOV).
vec3 sh_vm_apparent(vec3 p) {
    vec3 v = (frame.view * vec4(p, 1.0)).xyz;
    v.xy *= frame.shadow_cfg2.w;
    return frame.camera_pos.xyz + transpose(mat3(frame.view)) * v;
}

float shadow_factor_vm(vec3 p, vec3 n) {
    if (frame.sun_color.w < 0.5 || frame.ssky0.y <= 0.0) return 1.0;
    vec3 pw = sh_vm_apparent(p) + normalize(frame.sun_dir.xyz) * frame.shadow_vm2.z;
    float s = shadow_world(pw, n) * shadow_vm_tile(p, n);
    return mix(1.0, s, frame.ssky0.y * frame.shadow_cfg3.w);
}

// The moon map (bindings 18 / 19): the moon while the sun still owns the cascades; the sun's type
// (Ultra: the widest tent) and Strength.
float moon_shadow(vec3 p, vec3 n) {
    if (frame.ssky1.w <= 0.0 || frame.sun_color.w < 0.5) return 1.0;
    vec3 l = normalize(frame.fill_dir.xyz);
    vec4 sp = frame.shadow_vp_moon * vec4(p + n * (frame.shadow_moon.w * sh_sin_nl(n, l)), 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    float texel = frame.shadow_moon2.x;
    float edge = 4.0 * texel;
    if (any(lessThan(uv, vec2(edge))) || any(greaterThan(uv, vec2(1.0 - edge))) || sp.z > 1.0 || sp.z < 0.0) return 1.0;
    float mpt = frame.shadow_moon.x, span = frame.shadow_moon.y;
    vec2 g = sh_receiver_gradient(frame.shadow_vp_moon, n, sh_slope_limit() * mpt / (texel * span));
    float z = sp.z - frame.shadow_moon.z - texel * length(g);
    float mode = min(frame.shadow_cfg4.x, 2.0), tent = frame.shadow_cfg4.x > 2.5 ? 7.0 : frame.shadow_cfg4.y;
    float s = sh_filter(shadow_moon_map, shadow_moon_depth, texel, uv, z, g, vec2(texel * 0.5), vec2(1.0 - texel * 0.5), mpt, span, frame.ssky2.x, mode, tent, 0.0,
                        sh_rotation());
    float r = frame.ssky2.w;
    s = mix(s, 1.0, smoothstep(r * 0.85, r, length(p - frame.camera_pos.xyz)));
    return mix(1.0, s, frame.ssky1.w * frame.shadow_cfg3.w);
}

// r_shadow_debug 2..7, 9: penumbra width, blocker distance, cascade / mode, cloud shadow, texel size,
// viewmodel tile coverage, the sun's shadow term with Strength (grey).
vec3 shadow_debug_color(int view, vec3 p, vec3 n) {
    vec3 heat[4] = vec3[4](vec3(0.05, 0.05, 0.4), vec3(0.1, 0.8, 0.3), vec3(1.0, 0.85, 0.1), vec3(1.0, 0.1, 0.05));
    if (view == 2 || view == 3 || view == 6) {
        float v = view == 2 ? g_sh_pen / 0.5 : view == 3 ? (g_sh_block < 0.0 ? -1.0 : g_sh_block / 30.0) : (g_sh_casc < 0 ? -1.0 : frame.shadow_mpt[g_sh_casc] / 0.1);   // 0 .. 0.5 m, 0 .. 30 m, 0 .. 10 cm
        if (g_sh_vm == 1 && view == 6) v = frame.shadow_vm.x / 0.1;
        if (v < 0.0) return vec3(0.15);
        float t = clamp(v, 0.0, 1.0) * 3.0;
        int i = min(int(t), 2);
        return mix(heat[i], heat[i + 1], t - float(i));
    }
    if (view == 4) {
        if (frame.ssky0.y <= 0.0) return vec3(0.4);   // cast shadows off (sky only)
        vec3 c = g_sh_casc == 0 ? vec3(1.0, 0.25, 0.2) : g_sh_casc == 1 ? vec3(0.25, 1.0, 0.3) : g_sh_casc == 2 ? vec3(0.3, 0.4, 1.0) : g_sh_casc == 3 ? vec3(1.0, 0.85, 0.2) : vec3(0.6);
        if (g_sh_vm == 1) c = vec3(1.0, 0.3, 1.0);    // the viewmodel tile
        if (frame.ssky0.z < 1.0) c = mix(c, vec3(1.0, 1.0, 0.2), 0.35);   // wide mode
        // the shadow distance fade (last border) darkens towards its end
        float d = sh_select_distance(p), D = frame.shadow_cfg2.z;
        return c * (0.35 + 0.65 * frame.ssky0.y) * (1.0 - 0.7 * smoothstep(D * (1.0 - frame.shadow_cfg3.z), D, d));
    }
    if (view == 7) return g_sh_vm < 0 ? vec3(0.2) : g_sh_vm == 1 ? vec3(0.2, 1.0, 0.3) : vec3(1.0, 0.15, 0.1);
    if (view == 9) return vec3(shadow_factor(p, n));
    return vec3(cloud_shadow_at(p));
}

float d_ggx(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

vec3 f_schlick(float vdh, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0); }

float v_smith(float ndv, float ndl, float a) {
    float k = a * 0.5;
    return 1.0 / ((ndv * (1.0 - k) + k) * (ndl * (1.0 - k) + k) * 4.0);
}

// Animated caustic light (fabricator chamber, docs/REVIVE.md): a few warped sine layers; depends
// only on position and time (TAA-safe).
float lit_caustic(vec3 p, float t) {
    vec2 q = p.xz * 5.0 + vec2(0.0, p.y * 2.0);
    float c = 0.0;
    for (int i = 0; i < 3; i++) {
        q += vec2(sin(q.y * 1.3 + t * 0.9 + float(i)), cos(q.x * 1.1 - t * 0.7 + float(i) * 1.7));
        c += 1.0 / (1.0 + 8.0 * abs(sin(q.x) + sin(q.y)));
    }
    return c / 3.0;
}

vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float rough, float metal) {
    vec3 h = normalize(v + l);
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, v), 1e-4);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(v, h), 0.0);
    float a = max(rough * rough, 0.02);
    vec3 f0 = mix(vec3(0.04), albedo, metal);
    vec3 F = f_schlick(vdh, f0);
    vec3 spec = d_ggx(ndh, a) * v_smith(ndv, ndl, a) * F;
    vec3 kd = (1.0 - F) * (1.0 - metal);
    return (kd * albedo / PI + spec) * ndl;
}

#endif
