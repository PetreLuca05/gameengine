// Weather surface terms (docs/WEATHER.md), shared by the lit and G-buffer shaders so the shading
// and the screen-space passes (SSR on puddles) agree. Everything comes from a few global constants
// (frame.wx*) plus two textures: the top-down rain occlusion depth (is this point under the sky?)
// and the level's puddle mask (low points). The constants match weather_wet_material()
// (src/world/weather.c).

// Surface overrides set by the terrain shading (shaders/terrain/terrain.glsl) before weather_surface; the
// defaults leave every other surface exactly as before.
float g_wx_porosity = -1.0;   // >= 0: the surface's porosity (the terrain layers')
float g_wx_puddle = 0.0;      // puddle potential beside the level's mask (moist low ground, low borders)
float g_wx_snow = 1.0;        // snow accumulation (per layer, the snow line band)

float wx_hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
vec2 wx_hash2(vec2 p) { return fract(sin(vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)))) * 43758.5453); }

// 1 = the sky reaches p (rain / snow land there), 0 = under a roof / overhang / inside.
float wx_exposed(vec3 p) {
    if (frame.wx1.w < 0.5) return 1.0;
    vec4 c = frame.occl_vp * vec4(p, 1.0);
    vec2 uv = c.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.01))) || any(greaterThan(uv, vec2(0.99)))) return 1.0;
    float dm = texture(wx_occl, uv).r;
    float below = (c.z - dm) * frame.wx4.x;   // metres below the top-most surface
    return 1.0 - smoothstep(0.03, 0.12, below);
}

// Rain ripples (puddles): expanding rings in a jittered grid, new rings every ~0.7 s per cell.
vec3 wx_ripple_normal(vec2 xz, float t, float rain) {
    vec2 g = xz * 4.5;
    vec2 acc = vec2(0.0);
    for (int k = 0; k < 2; k++) {
        vec2 gk = g + (k == 1 ? vec2(0.37, 0.71) : vec2(0.0));
        vec2 cell = floor(gk);
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                vec2 c = cell + vec2(i, j);
                vec2 h = wx_hash2(c + float(k) * 17.0);
                float ph = fract(t * 1.4 + h.x);
                vec2 cp = c + 0.2 + 0.6 * h;
                vec2 d = gk - cp;
                float r = length(d);
                float rad = ph * 0.6;
                float ring = exp(-pow((r - rad) * 18.0, 2.0)) * (1.0 - ph) * (1.0 - ph) * step(h.y, rain * 1.2);
                acc += (r > 1e-4 ? d / r : vec2(0.0)) * ring * sin((r - rad) * 40.0);
            }
    }
    return normalize(vec3(-acc.x * 0.12, 1.0, -acc.y * 0.12));
}

// Wind ripples on puddles (no rain): two moving sine layers.
vec3 wx_wind_normal(vec2 xz, float t) {
    vec2 w = frame.wx2.xy;
    float s = clamp(frame.wx2.z / 10.0, 0.0, 1.0);
    vec2 a = vec2(sin(dot(xz, vec2(3.1, 1.7)) + t * 2.3 + dot(w, xz) * 0.2), sin(dot(xz, vec2(-1.3, 2.9)) + t * 1.7));
    return normalize(vec3(a.x * 0.03 * s, 1.0, a.y * 0.03 * s));
}

// Modifies albedo / roughness / normal for wetness, puddles and snow. Returns the snow amount (for
// the sparkle). porosity from the material (rough dielectrics soak more).
float weather_surface(vec3 wp, inout vec3 n, inout vec3 albedo, inout float rough, float metal, bool viewmodel) {
    float wet = frame.wx0.x, pud = frame.wx0.y, snow = frame.wx0.z;
    if (viewmodel || (wet + pud + snow) < 0.002) return 0.0;
    float exposed = wx_exposed(wp);
    if (exposed <= 0.0) return 0.0;
    float up = n.y;
    float porosity = g_wx_porosity >= 0.0 ? g_wx_porosity : (1.0 - metal) * clamp(rough * 1.15, 0.2, 1.0);
    // Wetness: tops fully, walls partly (run-off).
    float w = wet * exposed * mix(0.45, 1.0, clamp(up * 0.5 + 0.5, 0.0, 1.0));
    float k = 1.0 - w * (0.55 * porosity + 0.08);
    albedo *= k;
    rough = mix(rough, min(rough, 0.18), w * (0.6 + 0.35 * porosity));
    // Puddles: up-facing ground where the mask has low points, filling with the puddle level.
    if ((frame.wx2.w > 0.5 || g_wx_puddle > 0.0) && pud > 0.001 && up > 0.85) {
        vec2 puv = (wp.xz - frame.puddle_xf.xy) * frame.puddle_xf.zw;
        bool in_mask = frame.wx2.w > 0.5 && all(greaterThan(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)));
        if (in_mask || g_wx_puddle > 0.0) {
            float m = max(in_mask ? texture(wx_puddles, puv).r : 0.0, g_wx_puddle);
            float nz = wx_hash(floor(wp.xz * 1.7)) * 0.15 + sin(wp.x * 2.1) * sin(wp.z * 1.7) * 0.08;
            float cover = smoothstep(1.0 - pud * 0.9, 1.0 - pud * 0.9 + 0.12, m + nz * m) * exposed;
            if (cover > 0.0) {
                albedo *= mix(1.0, 0.45, cover);
                rough = mix(rough, frame.wx4.z > 0.5 ? 0.03 : 0.2, cover);
                vec3 rn = frame.wx1.x > 0.02 ? wx_ripple_normal(wp.xz, frame.wx1.z, frame.wx1.x) : wx_wind_normal(wp.xz, frame.wx1.z);
                // Ripples fade out with distance (they would alias into sparkles).
                float near = 1.0 - smoothstep(12.0, 35.0, distance(wp, frame.camera_pos.xyz));
                n = normalize(mix(n, rn, cover * near));
            }
        }
    }
    // Snow cover on upward faces.
    float s = min(snow * exposed * smoothstep(0.25, 0.75, up) * g_wx_snow, 1.0);
    if (s > 0.0) {
        albedo = mix(albedo, vec3(0.86, 0.89, 0.93), s);
        rough = mix(rough, 0.62, s);
    }
    return s;
}
