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

// One cascade of the 2 x 2 shadow atlas: -1 when p is outside its tile.
float shadow_cascade(int c, vec3 p, vec3 n) {
    float k = c == 0 ? 1.0 : (c == 1 ? 2.5 : 7.0);   // texel size relative to cascade 0
    vec3 offset_p = p + n * 0.03 * k;
    vec4 sp = frame.shadow_vps[c] * vec4(offset_p, 1.0);
    sp.xyz /= sp.w;
    vec2 uv = sp.xy * 0.5 + 0.5;
    vec2 tile = vec2(c & 1, c >> 1) * 0.5;
    vec2 local = (uv - tile) * 2.0;
    float edge = 2.0 * frame.params.y * (frame.params.z + 1.0);
    if (any(lessThan(local, vec2(edge))) || any(greaterThan(local, vec2(1.0 - edge))) || sp.z > 1.0 || sp.z < 0.0) return -1.0;
    float texel = frame.params.y;
    int r = int(frame.params.z);
    float sum = 0.0;
    float count = 0.0;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            sum += texture(shadow_map, vec3(uv + vec2(x, y) * texel, sp.z - 0.0012 * k * frame.shadow_bias[c]));
            count += 1.0;
        }
    return sum / count;
}

float shadow_factor(vec3 p, vec3 n) {
    if (frame.sun_color.w < 0.5) return 1.0;
    int count = int(frame.shadow_splits.w);
    if (count <= 1) {
        float s = shadow_cascade(0, p, n);
        float fade_end = frame.shadow_bias.w;
        if (s >= 0.0 && fade_end > 0.0) s = mix(s, 1.0, smoothstep(fade_end * 0.85, fade_end, length(p - frame.camera_pos.xyz)));
        return s < 0.0 ? 1.0 : s;
    }
    // Cascade by distance from the camera; the last 15 % of a cascade blends into the next one.
    // Engine Fidelity: 1 .. 3 cascades (w); shadow_bias.w > 0 fades the last one out (shorter reach).
    float d = length(p - frame.camera_pos.xyz);
    for (int c = 0; c < count; c++) {
        float radius = frame.shadow_splits[c];
        if (d > radius && c < count - 1) continue;
        float s = shadow_cascade(c, p, n);
        if (s < 0.0) continue;
        float blend = c < count - 1 ? smoothstep(radius * 0.85, radius, d) : 0.0;
        if (blend > 0.0) {
            float s2 = shadow_cascade(c + 1, p, n);
            if (s2 >= 0.0) s = mix(s, s2, blend);
        }
        float fade_end = frame.shadow_bias.w;
        if (fade_end > 0.0) s = mix(s, 1.0, smoothstep(fade_end * 0.85, fade_end, d));
        return s;
    }
    return 1.0;
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
