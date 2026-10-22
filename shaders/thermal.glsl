// Thermal scope PiP (docs/NIGHT_OPTICS.md "Thermal render pass"): when frame.thermal.x is set, the lit
// shaders return 8..14 um radiance instead of light. A surface emits by its temperature and emissivity
// and reflects the rest (the cold sky above, the ground below):
//   L = e B(T) + (1 - e) B(T_reflected)
// T comes from its thermal class (src/world/thermal.h: the class's temperature now from the weather and
// the day's history, + facing the sun while sunlit, - facing the clear night sky), then the heat volumes
// (game_thermal.c): bodies (characters: face / hands / torso, zombies colder, corpses cooling), surface
// hot spots (engines, exhausts, brakes, tyres, barrels, fires, lamps) and ground patches (a car parked
// there, footprints). The class is read from the material: every flag bit is taken, so it is inferred
// (glass / windows, foliage cards / ground cover, terrain by its colour, metal by metallic / roughness,
// lamps by emission, wood by its colour), and a skinned draw is a body. CPU mirror: world/thermal.c.
#ifndef THERMAL_GLSL
#define THERMAL_GLSL

#define THC_CONCRETE 0
#define THC_SOIL 2
#define THC_GRASS 3
#define THC_FOLIAGE 4
#define THC_WOOD 6
#define THC_METAL_PAINTED 7
#define THC_METAL_BARE 8
#define THC_GLASS 9
#define THC_WATER 10
#define THC_SKIN 12
#define THC_HOT 15

float th_lut(int i) { return frame.thermal_lut[i >> 2][i & 3]; }

float th_radiance(float t) {
    float x = clamp((t + 60.0) / 6.5, 0.0, 254.999);
    int i = int(x);
    return mix(th_lut(i), th_lut(i + 1), x - float(i));
}

// The apparent sky temperature along a direction (cos_z = its height above the horizon).
float th_sky(float cos_z) {
    return cos_z <= 0.0 ? frame.thermal.y : frame.thermal.y - frame.thermal_sky.x * pow(cos_z, frame.thermal_sky.y);
}

int th_class(uint flags, uint ao_b, vec3 albedo, float metal, float rough, float emissive, bool body) {
    if (body) return THC_SKIN;
    if ((flags & (MAT_GLASS | MAT_WINDOWS)) != 0u) return THC_GLASS;
    if ((flags & MAT_COVER) != 0u) return THC_GRASS;
    if ((ao_b & MAT_AO_BYTE_THIN) != 0u || (flags & MAT_ALPHA_MASK) != 0u) return THC_FOLIAGE;
    if (emissive > 0.3 || ((flags & MAT_UNLIT) != 0u && max(albedo.r, max(albedo.g, albedo.b)) > 0.6)) return THC_HOT;
    if ((flags & MAT_TERRAIN) != 0u) return albedo.g > albedo.r * 1.05 && albedo.g > albedo.b ? THC_GRASS : THC_SOIL;
    if (metal > 0.5) return rough < 0.35 ? THC_METAL_BARE : THC_METAL_PAINTED;
    if (albedo.r > albedo.g * 1.15 && albedo.g > albedo.b * 1.15 && albedo.r < 0.7) return THC_WOOD;
    return THC_CONCRETE;
}

// Heat volumes: bodies set a character's surface (the weighted volumes), hot spots pull any surface
// towards their temperature, ground patches only up-facing surfaces; worn gear on a body is warmed through.
float th_volumes(vec3 wp, vec3 n, float t, bool body) {
    int count = int(frame.thermal_sky.w);
    float bw = 0.0, bt = 0.0;
    for (int i = 0; i < count && i < THERMAL_VOLUMES; i++) {
        vec4 a = frame.thermal_vol[i * 2], b = frame.thermal_vol[i * 2 + 1];
        float d = length(wp - a.xyz) / max(a.w, 1e-3);
        if (d >= 1.0) continue;
        float w = 1.0 - smoothstep(b.z, 1.0, d);
        int kind = int(b.y + 0.5);
        if (kind == 1) {
            if (body) { bw += w * w; bt += w * w * b.x; }
            else t = mix(t, b.x, w * 0.85);   // worn gear / a placeholder body: warmed through
        } else if (kind == 2) {
            if (!body && n.y > 0.6) t = mix(t, b.x, w);
        } else {
            t = mix(t, b.x, w);
        }
    }
    if (body) t = bw > 1e-3 ? bt / bw : frame.thermal.y;
    return t;
}

// rgba: radiance, temperature (C), emissivity, 1 (opaque: glass does not transmit LWIR).
vec4 thermal_shade(uint flags, uint ao_b, vec3 wp, vec3 n, vec3 albedo, float metal, float rough, float emissive, bool body, float sunlit) {
    int c = th_class(flags, ao_b, albedo, metal, rough, emissive, body);
    vec4 k = frame.thermal_cls[c];
    vec3 sd = frame.sun_dir.xyz;
    vec3 l = dot(sd, sd) > 1e-8 ? normalize(sd) : vec3(0.0, 1.0, 0.0);   // no sun at night: zero vector
    float t = c == THC_HOT ? frame.thermal.w : k.x + k.z * sunlit * max(dot(n, l), 0.0) - k.w * max(n.y, 0.0);
    t = th_volumes(wp, n, t, body);
    float e = k.y;
    vec3 v = normalize(frame.camera_pos.xyz - wp);
    vec3 rv = reflect(-v, n);
    float t_refl = rv.y > 0.0 ? th_sky(rv.y) : frame.thermal.z;
    float rad = e * th_radiance(t) + (1.0 - e) * th_radiance(t_refl);
    // fog / haze: LWIR passes thin haze better than light, dense fog still swallows the scene
    float fd = length(frame.camera_pos.xyz - wp);
    float tr = exp(-fd * frame.fog.w * 0.6);
    rad = mix(th_radiance(frame.thermal.y), rad, tr);
    return vec4(rad, t, e, 1.0);
}
#endif
