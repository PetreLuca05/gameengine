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

// One atlas tile [lo, lo + 0.5]: uv, receiver depth z (NDC, biased), gradient g, metres per texel,
// metres per NDC depth, tan of the source radius.
float sh_tile(vec2 uv, float z, vec2 g, vec2 lo, float mpt, float span, float tan_r, float model, float rot) {
    float texel = frame.params.y;
    vec2 tmin = lo + texel * 0.5, tmax = lo + 0.5 - texel * 0.5;
    if (model < 0.5) {
        // PCF steps (Off / Low, the 2010 era): a square kernel with a fractional outer ring, so a
        // step change of the radius blends in instead of popping.
        float rf = frame.ssky1.x;
        int r = int(ceil(rf - 1e-3));
        float sum = 0.0, wsum = 0.0;
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++) {
                int ring = max(abs(x), abs(y));
                float w = float(ring) <= floor(rf + 1e-3) ? 1.0 : fract(rf);
                vec2 o = vec2(x, y) * texel;
                sum += w * texture(shadow_map, vec3(clamp(uv + o, tmin, tmax), z + dot(g, o)));
                wsum += w;
            }
        g_sh_pen = rf * mpt;
        return sum / max(wsum, 1e-4);
    }
    bool full = model > 1.5;
    int nb = full ? 16 : 8, nf = full ? 16 : 8;
    float max_tex = frame.ssky1.y;
    float search = clamp(frame.ssky1.z * tan_r / mpt, 1.0, max_tex * 1.5);
    float bsum = 0.0, bcnt = 0.0;
    for (int i = 0; i < nb; i++) {
        vec2 o = sh_vogel(i, nb, rot) * search * texel;
        float zt = z + dot(g, o);
        float d = textureLod(shadow_depth, clamp(uv + o, tmin, tmax), 0.0).r;
        if (d < zt) { bsum += zt - d; bcnt += 1.0; }
    }
    if (bcnt < 0.5) { g_sh_block = -1.0; g_sh_pen = 0.0; return 1.0; }
    float dblock = bsum / bcnt * span;
    float w = clamp(dblock * tan_r / mpt, 0.5, max_tex);
    g_sh_block = dblock;
    g_sh_pen = w * mpt;
    float sum = 0.0;
    for (int i = 0; i < nf; i++) {
        vec2 o = sh_vogel(i, nf, rot) * w * texel;
        sum += texture(shadow_map, vec3(clamp(uv + o, tmin, tmax), z + dot(g, o)));
    }
    return sum / float(nf);
}

// The kernel's rotation: interleaved gradient noise per pixel, a new pattern every frame (TAA).
float sh_rotation() {
    float f = mod(floor(frame.camera_pos.w * 60.0), 64.0);
    return sh_ign(gl_FragCoord.xy + 5.588238 * f) * 6.2831853;
}

// One cascade of the 2 x 2 shadow atlas: -1 when p is outside its tile.
float shadow_cascade(int c, vec3 p, vec3 n) {
    float k = c == 0 ? 1.0 : (c == 1 ? 2.5 : 7.0);   // texel size relative to cascade 0
    vec3 offset_p = p + n * 0.03 * k;
    vec4 sp = frame.shadow_vps[c] * vec4(offset_p, 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    vec2 tile = vec2(c & 1, c >> 1) * 0.5;
    vec2 local = (uv - tile) * 2.0;
    float edge = 4.0 * frame.params.y;
    if (any(lessThan(local, vec2(edge))) || any(greaterThan(local, vec2(1.0 - edge))) || sp.z > 1.0 || sp.z < 0.0) return -1.0;
    float mpt = frame.shadow_mpt[c], span = frame.shadow_span[c];
    // receiver plane: slopes up to ~80 deg; constant bias + one texel of slope
    vec2 g = sh_receiver_gradient(frame.shadow_vps[c], n, 6.0 * mpt / (frame.params.y * span));
    float z = sp.z - 0.0012 * k * frame.shadow_bias[c] - frame.params.y * length(g);
    g_sh_casc = c;
    return sh_tile(uv, z, g, tile, mpt, span, frame.ssky0.x, frame.ssky0.w, sh_rotation());
}

float shadow_factor(vec3 p, vec3 n) {
    if (frame.sun_color.w < 0.5 || frame.ssky0.y <= 0.0) return 1.0;   // off, or under the sky cutoff
    int count = int(frame.shadow_splits.w);
    float s = 1.0;
    if (count <= 1) {
        s = shadow_cascade(0, p, n);
        float fade_end = frame.shadow_bias.w;
        if (s >= 0.0 && fade_end > 0.0) s = mix(s, 1.0, smoothstep(fade_end * 0.85, fade_end, length(p - frame.camera_pos.xyz)));
        s = s < 0.0 ? 1.0 : s;
    } else {
        // Cascade by distance from the camera; the last 15 % of a cascade blends into the next one.
        // Engine Fidelity: 1 .. 3 cascades (w); shadow_bias.w > 0 fades the last one out (shorter reach).
        // Wide mode (ssky0.z < 1): cascade 0 fades into cascade 1 (the wide blur), then is not drawn.
        float d = length(p - frame.camera_pos.xyz);
        float w0 = frame.ssky0.z;
        int first = w0 <= 0.0 ? 1 : 0;
        for (int c = first; c < count; c++) {
            float radius = frame.shadow_splits[c];
            if (d > radius && c < count - 1) continue;
            float sc = shadow_cascade(c, p, n);
            if (sc < 0.0) continue;
            float blend = c < count - 1 ? smoothstep(radius * 0.85, radius, d) : 0.0;
            if (c == 0 && w0 < 1.0) blend = max(blend, 1.0 - w0);
            if (blend > 0.0) {
                float s2 = shadow_cascade(c + 1, p, n);
                if (s2 >= 0.0) sc = mix(sc, s2, blend);
            }
            float fade_end = frame.shadow_bias.w;
            if (fade_end > 0.0) sc = mix(sc, 1.0, smoothstep(fade_end * 0.85, fade_end, d));
            s = sc;
            break;
        }
    }
    return mix(1.0, s, frame.ssky0.y);
}

// The moon tile (atlas tile 3): the moon while the sun still owns the cascades.
float moon_shadow(vec3 p, vec3 n) {
    if (frame.ssky1.w <= 0.0 || frame.sun_color.w < 0.5) return 1.0;
    vec4 sp = frame.shadow_vp_moon * vec4(p + n * 0.075, 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    vec2 local = (uv - 0.5) * 2.0;
    float edge = 4.0 * frame.params.y;
    if (any(lessThan(local, vec2(edge))) || any(greaterThan(local, vec2(1.0 - edge))) || sp.z > 1.0 || sp.z < 0.0) return 1.0;
    float mpt = frame.shadow_mpt.w, span = frame.shadow_span.w;
    vec2 g = sh_receiver_gradient(frame.shadow_vp_moon, n, 6.0 * mpt / (frame.params.y * span));
    float z = sp.z - 0.0012 * 2.5 * frame.shadow_bias.y - frame.params.y * length(g);
    float s = sh_tile(uv, z, g, vec2(0.5), mpt, span, frame.ssky2.x, min(frame.ssky0.w, 1.0), sh_rotation());
    float r = frame.ssky2.w;
    s = mix(s, 1.0, smoothstep(r * 0.85, r, length(p - frame.camera_pos.xyz)));
    return mix(1.0, s, frame.ssky1.w);
}

// r_shadow_debug 2..5: penumbra width, blocker distance, cascade / mode, cloud shadow.
vec3 shadow_debug_color(int view, vec3 p) {
    vec3 heat[4] = vec3[4](vec3(0.05, 0.05, 0.4), vec3(0.1, 0.8, 0.3), vec3(1.0, 0.85, 0.1), vec3(1.0, 0.1, 0.05));
    if (view == 2 || view == 3) {
        float v = view == 2 ? g_sh_pen / 0.5 : (g_sh_block < 0.0 ? -1.0 : g_sh_block / 30.0);   // 0 .. 0.5 m, 0 .. 30 m
        if (v < 0.0) return vec3(0.15);
        float t = clamp(v, 0.0, 1.0) * 3.0;
        int i = min(int(t), 2);
        return mix(heat[i], heat[i + 1], t - float(i));
    }
    if (view == 4) {
        if (frame.ssky0.y <= 0.0) return vec3(0.4);   // cast shadows off (sky only)
        vec3 c = g_sh_casc == 0 ? vec3(1.0, 0.25, 0.2) : g_sh_casc == 1 ? vec3(0.25, 1.0, 0.3) : g_sh_casc == 2 ? vec3(0.3, 0.4, 1.0) : vec3(0.6);
        if (frame.ssky0.z < 1.0) c = mix(c, vec3(1.0, 1.0, 0.2), 0.35);   // wide mode
        return c * (0.35 + 0.65 * frame.ssky0.y);
    }
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

// Specular scale of the surface being shaded (ground cover: its "specular"; 1 everywhere else).
float g_spec_scale = 1.0;
vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float rough, float metal) {
    vec3 h = normalize(v + l);
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, v), 1e-4);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(v, h), 0.0);
    float a = max(rough * rough, 0.02);
    vec3 f0 = mix(vec3(0.04), albedo, metal);
    vec3 F = f_schlick(vdh, f0);
    vec3 spec = d_ggx(ndh, a) * v_smith(ndv, ndl, a) * F * g_spec_scale;
    vec3 kd = (1.0 - F) * (1.0 - metal);
    return (kd * albedo / PI + spec) * ndl;
}

#endif
