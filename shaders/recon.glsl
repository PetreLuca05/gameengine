// Reconstruction effect (docs/REVIVE.md "Reconstruction shader"): a body rebuilt inside a
// fabricator over a 0 -> 1 progress. Inside-out: the order of every point is its height above the
// spine base plus its radial distance from the spine, broken up by stable 3D world-space noise; three
// staged fronts reveal a dark skeleton tint, a red muscle tint, then the final material. The fronts
// carry an emissive construction edge, vertical scan lines and a lattice "wire" flash where the
// surface has just materialized. Stable per pixel (no time / per-frame randomness): TAA-safe.
//   v_recon: xyz spine base (world), w progress. MAT_RECON_FX: edge / scan / wire (fidelity).
#ifndef RECON_GLSL
#define RECON_GLSL

const float RECON_SPREAD = 1.75;      // progress -> front position (the three fronts are staggered)
const float RECON_STAGGER = 0.35;
const float RECON_EDGE = 0.06;
const vec3 RECON_EDGE_COLOR = vec3(0.45, 0.95, 1.0);
const vec3 RECON_BONE = vec3(0.13, 0.12, 0.115);
const vec3 RECON_MUSCLE = vec3(0.42, 0.05, 0.04);

float recon_hash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float recon_vnoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(recon_hash(i), recon_hash(i + vec3(1, 0, 0)), f.x), mix(recon_hash(i + vec3(0, 1, 0)), recon_hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(recon_hash(i + vec3(0, 0, 1)), recon_hash(i + vec3(1, 0, 1)), f.x), mix(recon_hash(i + vec3(0, 1, 1)), recon_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// Order of a world point in the build (0 = first).
float recon_order(vec3 wpos, vec3 base) {
    vec3 d = wpos - base;
    float h = clamp(d.y / 1.85, 0.0, 1.0);
    float r = clamp(length(d.xz) / 0.32, 0.0, 1.0);
    float n = recon_vnoise(wpos * 7.0) * 0.6 + recon_vnoise(wpos * 19.0) * 0.4;
    return clamp(mix(0.55 * h + 0.45 * r, n, 0.3), 0.0, 1.0);
}

// False = not built yet (discard). albedo: the layer colour; emissive: edge + scan + wire.
bool recon_eval(vec3 wpos, vec4 rc, bool fx, inout vec3 albedo, out vec3 emissive) {
    emissive = vec3(0.0);
    float p = clamp(rc.w, 0.0, 1.0);
    if (p >= 1.0) return true;
    float t = recon_order(wpos, rc.xyz);
    float fs = p * RECON_SPREAD, fm = fs - RECON_STAGGER, fk = fs - 2.0 * RECON_STAGGER;
    if (fs < t) return false;
    albedo = fk >= t ? albedo : (fm >= t ? RECON_MUSCLE : RECON_BONE);
    if (!fx) return true;
    float e_s = 1.0 - smoothstep(0.0, RECON_EDGE, fs - t);
    float e_m = fm >= t ? 1.0 - smoothstep(0.0, RECON_EDGE, fm - t) : 0.0;
    float e_k = fk >= t ? 1.0 - smoothstep(0.0, RECON_EDGE, fk - t) : 0.0;
    emissive = RECON_EDGE_COLOR * (e_s * 4.5 + e_k * 2.5) + vec3(1.0, 0.35, 0.25) * e_m * 1.2;
    // Vertical scan lines over the freshly built band (fading as it settles).
    float band = 1.0 - smoothstep(0.0, 0.25, fk - t);
    float scan = smoothstep(0.82, 1.0, fract(wpos.y * 46.0));
    emissive += RECON_EDGE_COLOR * scan * band * 1.2;
    // Wire flash: a world-space lattice right behind the leading front.
    vec3 g = abs(fract(wpos * 16.0) - 0.5);
    float wire = smoothstep(0.44, 0.49, max(g.x, max(g.y, g.z)));
    emissive += vec3(1.0) * wire * (1.0 - smoothstep(0.0, 0.1, fs - t)) * 1.6;
    return true;
}

#endif
