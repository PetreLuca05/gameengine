// Ground cover shading (MAT_COVER, docs/FOLIAGE_GRASS.md; grass_asset.h): the lit body calls cover_base on
// the textured albedo, then cover_light adds the translucency. Inputs from the includer:
//   LIT_COVER_IN      vec4: x height along the card (0 root .. 1 tip), y card hash, z clump hash
//   LIT_COVER_PIVOT   vec3: the clump's world position (its root)
//   COVER_TEX(uv)     the asset's nta map: normal xy, b thickness, a AO
// Parameters: frame.cover[3 id + 0..2] (grass_cover_params), frame.cover_info.
// Everything is gated on MAT_COVER: other materials (trees included) never run it.
#include "terrain/terrain_ground.glsl"

struct CoverState { bool on; float t, thick, ao, sss, wrap, spec; };
CoverState g_cv = CoverState(false, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);

float cv_lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Variation, macro, ground matching and root darkening of the albedo; fills g_cv.
void cover_base(uint flags, vec4 cin, vec3 pivot, vec2 uv, inout vec3 albedo) {
    uint id = MAT_COVER_ID(flags);
    vec4 p0 = frame.cover[id * 3u], p1 = frame.cover[id * 3u + 1u], p2 = frame.cover[id * 3u + 2u];
    float t = clamp(cin.x, 0.0, 1.0);
    // per clump + card: three decorrelated values in -1..1
    vec3 r = fract(vec3(cin.z * 13.731 + cin.y * 0.371, cin.z * 7.937 + cin.y * 0.713, cin.z * 5.177 + cin.y * 0.529) * vec3(1.0, 1.37, 2.11)) * 2.0 - 1.0;
    vec3 a = tn_hue(albedo, r.x * p1.x);
    a = tn_sat(a, 1.0 + r.y * p1.y);
    a *= 1.0 + r.z * p1.z;
    vec3 top;
    vec3 ground = terrain_ground_color(pivot.xz, top);
    vec3 f = ground_macro_field(pivot.xz);
    a = ground_macro_apply(a, f, p1.w);
    // lower blade toward the ground: its hue at the blade's brightness, half its own brightness
    float gm = p0.z * (1.0 - smoothstep(0.0, max(p0.w, 1e-3), t));
    vec3 gh = ground * (cv_lum(a) / max(cv_lum(ground), 1e-4));
    a = mix(a, mix(gh, ground, 0.5), gm);
    a *= mix(p0.x, 1.0, smoothstep(0.0, max(p0.y, 1e-3), t));
    vec4 nta = COVER_TEX(uv);
    g_cv.on = true;
    g_cv.t = t;
    g_cv.thick = nta.b;
    g_cv.ao = mix(p2.w, 1.0, smoothstep(0.0, 0.4, t)) * mix(1.0, nta.a, 0.7);
    g_cv.sss = p2.x * frame.cover_info.z;
    g_cv.wrap = p2.y;
    g_cv.spec = p2.z;
    int dbg = int(frame.cover_info.w);
    if (dbg == 1) a = vec3(t, 1.0 - t, 0.0) * 0.25;                     // root -> tip
    else if (dbg == 2) a = ground;                                       // the ground colour it matches
    else if (dbg == 3) a = vec3(0.5) + r * 0.25;                         // per clump / card variation
    else if (dbg == 4) a = vec3(g_cv.thick) * 0.4;                       // translucency thickness
    else if (dbg == 5) a = vec3(0.5) + f * 0.4;                          // macro field
    albedo = a;
}

// Light through the blades from light direction l (unit, towards the light) for shading normal n
// (facing the viewer), view v: wrapped back lighting + forward scattering toward the viewer, x thickness,
// tinted by the albedo (saturated: light passing through a leaf).
vec3 cover_translucency(vec3 n, vec3 v, vec3 l, vec3 albedo) {
    if (!g_cv.on || g_cv.sss <= 0.0) return vec3(0.0);
    float back = clamp((-dot(n, l) + g_cv.wrap) / (1.0 + g_cv.wrap), 0.0, 1.0);
    float fwd = pow(clamp(dot(-v, l), 0.0, 1.0), 4.0);
    vec3 tint = albedo * mix(vec3(1.0), albedo / max(cv_lum(albedo), 0.01), 0.5);
    return tint * (0.5 * back + fwd) * g_cv.thick * g_cv.sss;
}
