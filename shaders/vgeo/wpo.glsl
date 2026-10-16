// World-position-offset snippets of vgeo foliage materials (docs/VGEO.md "Foliage support"). Pure
// functions, included by vg_decode.glsl (vg_vertex_world: every raster path, the shadows and the
// resolve displace the same way); CPU mirror: vgeo_wpo_eval / vgeo_wpo_max (foliage_support.c).
//
// Inputs: p the undisplaced world position, ip the instance position (pivot), col the vertex colour
// (wind weights: R bend / trunk, G branch, B leaf flutter), phase the instance's wind phase (0..1),
// wind (direction x, z, strength m/s, gust), t time (s), prm the material's wpo_params. Every snippet
// clamps the wind strength to prm.w (the wind it was declared for), so its displacement never
// exceeds vgeo_wpo_max(snippet, prm) whatever the weather does: that bound inflates every culling
// sphere of the mesh.

#define WPO_TAU 6.2831853

// Grass: the tuft bends along the wind by its R weight (0 root, 1 tip). prm: x bend at full wind (m,
// weight 1), y blade height (m, the drop of a bent tip), z sway frequency (Hz), w full wind (m/s).
// |offset| <= sqrt(2) prm.x.
vec3 wpo_grass(vec4 prm, vec3 p, vec3 ip, vec4 col, float phase, vec4 wind, float t) {
    float s = clamp(wind.z / max(prm.w, 1e-3), 0.0, 1.0);
    float osc = 0.65 + 0.35 * sin(WPO_TAU * (prm.z * t + phase) + ip.x * 0.37 + ip.z * 0.21);
    float b = prm.x * clamp(col.r, 0.0, 1.0) * s * osc;
    vec3 d = vec3(wind.x, 0.0, wind.y) * b;
    d.y = -min(0.5 * b * b / max(prm.y, 0.05), b);
    return d;
}

// Tree / bush: trunk sway (R), branch bounce (G), leaf flutter (B), per-instance phase. prm: x trunk
// sway, y branch amplitude, z flutter amplitude (m, weight 1, full wind), w full wind (m/s).
// |offset| <= prm.x + prm.y + 1.5 prm.z.
vec3 wpo_tree(vec4 prm, vec3 p, vec3 ip, vec4 col, float phase, vec4 wind, float t) {
    float s = clamp(wind.z / max(prm.w, 1e-3), 0.0, 1.0);
    float ph = WPO_TAU * phase;
    vec3 dir = vec3(wind.x, 0.0, wind.y), perp = vec3(-wind.y, 0.0, wind.x);
    float sway = prm.x * clamp(col.r, 0.0, 1.0) * s * (0.6 + 0.4 * sin(t * 0.9 + ph));
    float branch = prm.y * clamp(col.g, 0.0, 1.0) * s * sin(t * 2.3 + ph * 1.7 + p.x * 0.5 + p.y * 0.3 + p.z * 0.4);
    vec3 fl = vec3(sin(t * 7.1 + p.x * 3.1 + ph), 0.5 * sin(t * 8.3 + p.y * 2.7 + ph), sin(t * 6.7 + p.z * 3.3 + ph));
    float flutter = prm.z * clamp(col.b, 0.0, 1.0) * (0.25 + 0.75 * s);
    return dir * sway + (dir * 0.6 + perp * 0.8) * branch + fl * flutter;
}

// Displacement buffer push (WPO_DISPLACE, combinable): disp = the buffer's horizontal push at p (m,
// |disp| <= VG_DISPLACE_MAX), weighted by R; the bent part drops by half the push.
// |offset| <= 1.118 VG_DISPLACE_MAX.
vec3 wpo_displace(vec2 disp, vec4 col) {
    float l = length(disp);
    if (l > VG_DISPLACE_MAX) disp *= VG_DISPLACE_MAX / l;
    float w = clamp(col.r, 0.0, 1.0);
    return vec3(disp.x, -0.5 * min(l, VG_DISPLACE_MAX), disp.y) * w;
}

vec3 wpo_offset(uint snippet, vec4 prm, vec3 p, vec3 ip, vec4 col, float phase, vec4 wind, float t) {
    if (snippet == WPO_GRASS) return wpo_grass(prm, p, ip, col, phase, wind, t);
    if (snippet == WPO_TREE) return wpo_tree(prm, p, ip, col, phase, wind, t);
    return vec3(0.0);
}
