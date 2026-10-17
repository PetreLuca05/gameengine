// Terrain shading (docs/TERRAIN_SHADING.md): terrain_shade replaces the older surface layer blend
// (surfaces.glsl surf_terrain) for MAT_TERRAIN draws while the terrain set is loaded (frame.surf_info.z).
// Per pixel, the level (TB.g.tier.x: r_terrain_quality, bounded by Engine Fidelity) decides what runs:
//   all     material weights from the blend map, top 2 layers, height blending, simple-noise borders,
//           cells tiling (random rotation / mirror), macro texture, the shared macro field, paint map
//   1 Low   + per-layer HSV jitter by world noise
//   2 Med   + hex bombing, multi-scale, slope / altitude tint, crumbs, multi-octave borders, micro roughness,
//           triplanar on steep ground (the dominant layer)
//   3 High  + top 3 layers, curvature / moisture tint, sub-layer variants, parallax, scatter, transition sets, rims
//   4 Mod   + detail normals, directional / slope / curvature-aware borders, aspect tint
// The weather pass (weather.glsl) then sees each layer's porosity, the terrain's puddle potential and
// snow accumulation (g_wx_*).
#include "terrain/terrain_params.glsl"
#include "terrain/terrain_noise.glsl"
#include "terrain/terrain_macro.glsl"
#include "terrain/terrain_tile.glsl"
#include "terrain/terrain_weights.glsl"

vec3 ter_id_color(int id) { return vec3(tn_hash(vec2(float(id), 3.0)), tn_hash(vec2(float(id), 11.0)), tn_hash(vec2(float(id), 29.0))) * 0.8 + 0.1; }

// World-space normal from a tangent normal on the up-facing projection (image right = +x, image up = -z),
// whiteout-blended onto the geometric normal ng.
vec3 ter_world_normal(vec2 nt01, float strength, vec3 ng) {
    vec2 t = (nt01 * 2.0 - 1.0) * strength;
    float z = sqrt(max(1.0 - dot(t, t), 0.0));
    return normalize(vec3(t.x + ng.x, z * ng.y, -t.y + ng.z));
}

// Variant mask of a layer at the point: its source, through lo .. hi.
float ter_variant_mask(TerLayer L, vec4 var, vec2 xz, vec3 attr, vec4 paint, vec3 ng) {
    int src = int(var.y);
    float v;
    float nz = tn_fbm(xz * L.varn.x, 3);
    if (src == TMS_MOISTURE) v = attr.y;
    else if (src == TMS_CURVATURE) v = clamp(1.0 - attr.x, 0.0, 1.0);   // ridges (convex) high
    else if (src == TMS_SLOPE) v = 1.0 - ng.y;
    else if (src == TMS_PAINT) v = paint.a;
    else if (src == TMS_DRY) v = 1.0 - attr.y;
    else v = nz;
    if (src != TMS_NOISE && src != TMS_PAINT) v = mix(v, v + (nz - 0.5), L.varn.y * 0.5);
    return smoothstep(var.z, var.w, v);
}

// One layer's tap with its variant mixed in (High+, the top two layers: the first variant whose mask is
// on here; bounds the worst case at one extra bombed tap per layer).
TerTap ter_layer_tap(TerLayer L, vec2 xz, vec3 dpx, vec3 dpy, int level, float dist, vec3 attr, vec4 paint, vec3 ng, bool variants, out float vmask) {
    vec2 uv = xz * L.tex.z, gx = dpx.xz * L.tex.z, gy = dpy.xz * L.tex.z;
    TerTap t = ter_sample(L, L.tex.x, uv, gx, gy, level, dist, xz);
    vmask = 0.0;
    if (level >= 3 && variants) {
        for (int k = 0; k < 2; k++) {
            vec4 var = k == 0 ? L.var0 : L.var1;
            if (var.x < 0.0) continue;
            float m = ter_variant_mask(L, var, xz, attr, paint, ng);
            if (m <= 0.001) continue;
            TerTap v = ter_sample(L, var.x, uv * 0.97 + 0.31, gx * 0.97, gy * 0.97, min(level, 2), dist, xz);
            t.alb = mix(t.alb, v.alb, m);
            t.nrh = mix(t.nrh, v.nrh, m);
            vmask = max(vmask, m * (k == 0 ? 0.5 : 1.0));
            break;
        }
    }
    return t;
}

// The colour terms of a layer on its albedo (Low+ HSV jitter, Medium+ slope / altitude, High+ curvature /
// moisture, Modern aspect; the macro texture, the shared macro field and the paint tint everywhere).
vec3 ter_layer_color(TerLayer L, vec3 c, vec3 p, vec3 ng, vec3 attr, vec4 paint, vec3 field, int level) {
    if (L.tex.y >= 0.0) {
        vec4 m = texture(terrain_macro, vec3(p.xz * L.tex.w, L.tex.y));
        c *= mix(vec3(1.0), m.rgb * 2.0, TB.g.macro.x);
    }
    if (level >= 1) {
        vec2 q = p.xz * L.hsv.w;
        vec3 r = vec3(tn_noise(q), tn_noise(q + 17.1), tn_noise(q - 31.7)) * 2.0 - 1.0;
        c = tn_hue(c, r.x * L.hsv.x);
        c = tn_sat(c, 1.0 + r.y * L.hsv.y);
        c *= 1.0 + r.z * L.hsv.z;
    }
    if (level >= 2) {
        float s = smoothstep(L.ranges.x, L.ranges.y, ng.y);
        c *= mix(vec3(1.0), L.slope.rgb, s * L.slope.w * TB.g.macro.z);
        float a = smoothstep(L.ranges.z, L.ranges.w, p.y);
        c *= mix(vec3(1.0), L.alt.rgb, a * L.alt.w);
    }
    if (level >= 3) {
        float cv = (attr.x - 0.5) * 2.0 * TB.g.macro.w;
        c *= (1.0 - max(cv, 0.0) * L.curv.x) * (1.0 + max(-cv, 0.0) * L.curv.y);
        c *= mix(vec3(1.0), L.moist.rgb, attr.y * L.moist.w) * (1.0 - attr.y * L.curv.z * 0.5);
    }
    if (level >= 4) {
        float north = clamp(-ng.z * 2.0, 0.0, 1.0) * (1.0 - ng.y) * 2.0;
        c *= mix(vec3(1.0), L.aspect.rgb, clamp(north, 0.0, 1.0) * L.aspect.w);
    }
    c *= paint.rgb * 2.0;
    return ground_macro_apply(c, field, L.varn.z);
}

// Scatter: sprites (pebbles, leaves, twigs, grass bits) placed per cell of size cs (m) with probability
// density x cs^2 (mask: allowed atlas cells); composited over the albedo / normal / roughness.
void ter_scatter(vec3 p, vec3 dpx, vec3 dpy, float density, float cs, uint mask, float size, inout vec3 alb, inout vec3 nw, inout float rough, inout float ao, out float hit) {
    hit = 0.0;
    if (density <= 0.0 || mask == 0u) return;
    vec2 cols = max(TB.g.scatter.xy, vec2(1.0));
    int cells = int(cols.x * cols.y);
    vec2 g = p.xz / cs;
    vec2 base = floor(g - 0.5);
    for (int k = 0; k < 4; k++) {
        vec2 c = base + vec2(k & 1, k >> 1);
        vec2 h = tn_hash2(c * 1.37 + 5.1);
        if (h.x >= density * cs * cs) continue;
        // a random allowed sprite
        int pick = int(tn_hash(c + 91.0) * float(bitCount(mask)));
        int sprite = 0;
        for (int b = 0, seen = 0; b < cells; b++) {
            if ((mask & (1u << uint(b))) == 0u) continue;
            if (seen++ == pick) { sprite = b; break; }
        }
        vec2 centre = (c + 0.2 + 0.6 * tn_hash2(c + 3.3)) * cs;
        float sc = size * (0.6 + 0.8 * h.y);
        float a = tn_hash(c - 7.7) * 6.2831853;
        vec2 l = ter_rot(p.xz - centre, a) / sc + 0.5;
        if (any(lessThan(l, vec2(0.0))) || any(greaterThan(l, vec2(1.0)))) continue;
        vec2 cell = vec2(float(sprite % int(cols.x)), float(sprite / int(cols.x)));
        vec2 uv = (cell + clamp(l, 0.02, 0.98)) / cols;
        vec2 gx = ter_rot(dpx.xz, a) / sc / cols, gy = ter_rot(dpy.xz, a) / sc / cols;
        vec4 s = textureGrad(terrain_sc_alb, uv, gx, gy);
        if (s.a < 0.35) continue;
        vec4 sn = textureGrad(terrain_sc_nrh, uv, gx, gy);
        float k2 = smoothstep(0.35, 0.6, s.a);
        alb = mix(alb, s.rgb, k2);
        vec2 nt = ter_rot(sn.xy * 2.0 - 1.0, -a);
        nw = normalize(mix(nw, normalize(vec3(nt.x, 1.0, -nt.y)), k2 * 0.8));
        rough = mix(rough, sn.z, k2);
        ao = mix(ao, 1.0, k2 * 0.5);
        hit = max(hit, k2);
    }
}

// Parallax occlusion on the dominant layer (High+, near): the uv offset (in its tiles), 8..16 steps of a
// linear search through its height, refined; faded out over the last third of the distance.
vec2 ter_parallax(TerLayer L, vec2 uv, vec2 gx, vec2 gy, vec3 v, float fade) {
    float depth = L.detail.x * L.tex.z;   // metres -> tiles
    if (depth <= 0.0 || fade <= 0.0 || v.y <= 0.05) return vec2(0.0);
    vec2 dir = vec2(-v.x, v.z) / v.y * depth * fade;   // image right = +x, image up = -z
    int steps = int(mix(8.0, 16.0, clamp(1.0 - v.y, 0.0, 1.0)));
    float dh = 1.0 / float(steps), h = 1.0;
    vec2 off = vec2(0.0), prev = vec2(0.0);
    float th = textureGrad(terrain_nrh, vec3(uv, L.tex.x), gx, gy).a, ph = th;
    for (int i = 0; i < steps && th < h; i++) {
        prev = off;
        ph = th;
        h -= dh;
        off += dir * dh;
        th = textureGrad(terrain_nrh, vec3(uv + off, L.tex.x), gx, gy).a;
    }
    float a = th - h, b = ph - (h + dh);
    float t = a / min(a - b, -1e-4);
    return mix(off, prev, clamp(t, 0.0, 1.0));
}

void terrain_shade(vec3 p, inout vec3 n, inout vec4 base, float variation, out float rough_out, out float ao_out) {
    int level = int(TB.g.tier.x), dbg = int(TB.g.tier.y);
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec3 ng = n;
    vec3 v = normalize(frame.camera_pos.xyz - p);
    float dist = length(frame.camera_pos.xyz - p);
    float nmap = max(frame.surf_map.w, 1.0);
    vec2 mg = (p.xz - frame.surf_map.xy) * frame.surf_map.z;
    vec2 muv = (mg + 0.5) / nmap;
    vec4 attr4 = texture(terrain_attr, muv);
    vec3 attr = attr4.xyz;
    vec4 paint = texture(terrain_paint, muv);
    vec3 field = ground_macro_field(p.xz);
    // --- weights: top 2 (top 3 on High+)
    TerW W = ter_gather(p.xz);
    int nl = min(W.n, level >= 3 ? 3 : 2);
    int ids[4] = int[](W.id[0], W.id[1], W.id[2], W.id[3]);
    float t[4] = float[](W.w[0], W.w[1], W.w[2], W.w[3]);
    float wsum = 0.0;
    for (int i = 0; i < nl; i++) wsum += t[i];
    for (int i = 0; i < 4; i++) t[i] = i < nl ? t[i] / max(wsum, 1e-6) : 0.0;
    // --- the top pair's border: signed distance (m), the pair's shift, its band
    float spacing = 1.0 / max(frame.surf_map.z, 1e-6);
    TerPair q = TB.pair[0];
    float ta = 1.0, dpair = 0.0, band = 0.0;
    bool swapped = false, has_pair = nl >= 2 && t[1] > 1e-4;
    if (has_pair) {
        uint pi = TB.pair_of[ids[0] * TER_SLOTS + ids[1]];
        swapped = (pi & 0x80000000u) != 0u;
        q = TB.pair[min(int(pi & 0x7FFFFFFFu), TER_MAX_PAIRS - 1)];
        float d = (t[0] - t[1]) / (t[0] + t[1]) * (TB.g.tier.w + 0.5 * spacing);
        dpair = swapped ? -d : d;                       // > 0 on the pair's first material
        vec2 gs = vec2(dFdx(dpair), dFdy(dpair));       // the border's direction (screen -> world xz)
        mat2 J = mat2(dpx.x, dpy.x, dpx.z, dpy.z);      // rows: d(x, z) / d(screen x, y)
        float det = determinant(J);
        vec2 gw = abs(det) > 1e-10 ? inverse(J) * gs : vec2(0.0);
        vec2 border_dir = dot(gw, gw) > 1e-12 ? normalize(gw) : vec2(0.0);
        vec2 slope_dir = length(ng.xz) > 1e-4 ? normalize(ng.xz) * clamp((1.0 - ng.y) * 4.0, 0.0, 1.0) : vec2(0.0);
        vec2 wind = dot(frame.wx2.xy, frame.wx2.xy) > 1e-6 ? normalize(frame.wx2.xy) : vec2(1.0, 0.0);   // the weather's wind (xz)
        dpair += ter_border_shift(q, p.xz, attr, slope_dir, border_dir, wind, level);
        ta = ter_band(q, dpair);
        band = 4.0 * ta * (1.0 - ta);
        float tt = t[0] + t[1];
        float top = swapped ? 1.0 - ta : ta;
        t[0] = tt * top;
        t[1] = tt * (1.0 - top);
    }
    // rock exposure on steep ground: layers with "expose" take weight with the slope
    for (int i = 0; i < nl; i++) {
        TerLayer L = TB.layer[ids[i]];
        if (L.snow.w > 0.0 && level >= 2) t[i] += L.snow.w * smoothstep(L.snow.z, L.snow.z - 0.25, ng.y) * 0.5;
    }
    // --- parallax on the dominant layer (High+): one offset for every tap
    vec2 xz = p.xz;
    if (level >= 3 && dist < TB.g.dist.y) {
        TerLayer L0 = TB.layer[ids[0]];
        vec2 off = ter_parallax(L0, p.xz * L0.tex.z, dpx.xz * L0.tex.z, dpy.xz * L0.tex.z, v, 1.0 - smoothstep(TB.g.dist.y * 0.66, TB.g.dist.y, dist));
        xz += off / max(L0.tex.z, 1e-4);
    }
    // --- taps, colour terms, height blend
    TerTap taps[4];
    float H[4], vm[4];
    vec3 cols[4];
    float sharp = 1.0;
    for (int i = 0; i < 4; i++) {
        taps[i] = TerTap(vec4(0.0), vec4(0.5, 0.5, 0.9, 0.0));
        H[i] = 0.0;
        vm[i] = 0.0;
        cols[i] = vec3(0.0);
        if (i >= nl) continue;
        TerLayer L = TB.layer[ids[i]];
        if (L.tex.x < 0.0) {   // untextured slot: its mean colour, flat
            taps[i] = TerTap(vec4(L.mean.rgb, 1.0), vec4(0.5, 0.5, L.mean.w, 0.5));
        } else {
            taps[i] = ter_layer_tap(L, xz, dpx, dpy, level, dist, attr, paint, ng, i < 2, vm[i]);
        }
        cols[i] = ter_layer_color(L, taps[i].alb.rgb, p, ng, attr, paint, field, level);
        H[i] = taps[i].nrh.a * L.blend.x + L.blend.z;
        sharp = min(sharp, max(L.blend.y, 0.02));
    }
    float wf[4];
    if (has_pair && (uint(q.b.w) & TPM_HEIGHT) == 0u) {
        for (int i = 0; i < 4; i++) wf[i] = t[i];
    } else {
        ter_height_blend(nl, t, H, sharp, wf);
    }
    vec3 alb = vec3(0.0), nw = vec3(0.0);
    float rough = 0.0, ao = 0.0, hgt = 0.0, porosity = 0.0, snow_acc = 0.0;
    for (int i = 0; i < nl; i++) {
        TerLayer L = TB.layer[ids[i]];
        alb += cols[i] * wf[i];
        nw += ter_world_normal(taps[i].nrh.xy, L.detail.w, ng) * wf[i];
        rough += mix(taps[i].nrh.z, L.mean.w, 0.3) * wf[i];
        ao += clamp(1.0 + L.curv.w * (taps[i].alb.a - 1.0), 0.0, 1.0) * wf[i];
        hgt += taps[i].nrh.a * wf[i];
        porosity += L.blend.w * wf[i];
        snow_acc += L.snow.x * wf[i];
    }
    // --- transition texture set and rim inside the band (High+)
    uint pm = uint(q.b.w);
    float setw = 0.0;
    if (has_pair && level >= 3) {
        if ((pm & TPM_SET) != 0u && q.b.x >= 0.0) {
            setw = band * q.b.y * (0.7 + 0.3 * tn_noise(p.xz * 1.3));
            if (setw > 0.002) {
                TerLayer L0 = TB.layer[ids[0]];
                vec2 uv = xz * q.b.z;
                TerTap s = ter_tap_cells(q.b.x, uv, dpx.xz * q.b.z, dpy.xz * q.b.z, 2.0, ter_mean(q.b.x));
                vec3 sc = ter_layer_color(L0, s.alb.rgb, p, ng, attr, paint, field, min(level, 2));
                alb = mix(alb, sc, setw);
                nw = normalize(mix(nw, ter_world_normal(s.nrh.xy, 1.0, ng), setw));
                rough = mix(rough, s.nrh.z, setw);
            }
        }
        if ((pm & TPM_RIM) != 0u && q.d.y >= 0.0 && q.d.z > 0.0) {
            float x = -(dpair) - 0.5 * q.a.x;   // metres into the pair's second material past the band
            float rim = exp(-pow((x - 0.5 * q.d.z) / (0.5 * q.d.z), 2.0)) * smoothstep(-0.2, 0.0, x) * (0.6 + 0.4 * tn_noise(p.xz * 2.1));
            if (rim > 0.002) {
                vec2 uv = xz * 0.4;
                TerTap s = ter_tap_cells(q.d.y, uv, dpx.xz * 0.4, dpy.xz * 0.4, 2.0, ter_mean(q.d.y));
                alb = mix(alb, s.alb.rgb * ground_macro_apply(vec3(1.0), field, 1.0), rim);
                rough = mix(rough, s.nrh.z, rim);
            }
        }
        if (q.d.w > 0.0 && nl >= 2) {   // tint_by_weight: the minor material stains the major one in the band
            vec3 ma = TB.layer[ids[0]].mean.rgb, mb = TB.layer[ids[1]].mean.rgb;
            alb *= mix(vec3(1.0), mb / max(ma, vec3(0.01)), band * q.d.w);
        }
    }
    // --- scatter (High+, near): the dominant layer's set, and the pair's border scatter in the band
    float sc_hit = 0.0;
    if (level >= 3 && dist < TB.g.dist.z) {
        float fade = 1.0 - smoothstep(TB.g.dist.z * 0.7, TB.g.dist.z, dist);
        TerLayer L0 = TB.layer[ids[0]];
        float h1;
        ter_scatter(p, dpx, dpy, L0.scatter.x * fade * (1.0 - band), max(L0.scatter.y, 0.2), uint(L0.scatter.z), L0.scatter.w, alb, nw, rough, ao, h1);
        sc_hit = h1;
        if (has_pair && (pm & TPM_SCATTER) != 0u) {
            float h2;
            ter_scatter(p + vec3(13.7, 0.0, -7.1), dpx, dpy, q.e.y * band * fade, 0.35, uint(q.e.x), 0.09, alb, nw, rough, ao, h2);
            sc_hit = max(sc_hit, h2);
        }
    }
    // --- triplanar on steep ground (Medium+, the dominant layer's side projections)
    if (level >= 2 && ng.y < 0.7 && dist < 120.0) {
        TerLayer L0 = TB.layer[ids[0]];
        if (L0.tex.x >= 0.0) {
            float s = L0.tex.z;
            vec3 wt = pow(abs(ng), vec3(4.0));
            wt /= wt.x + wt.y + wt.z;
            TerTap sx = ter_tap(L0.tex.x, vec2(p.z, -p.y) * s, vec2(dpx.z, -dpx.y) * s, vec2(dpy.z, -dpy.y) * s);
            TerTap sz = ter_tap(L0.tex.x, vec2(p.x, -p.y) * s, vec2(dpx.x, -dpx.y) * s, vec2(dpy.x, -dpy.y) * s);
            vec3 side = ter_layer_color(L0, sx.alb.rgb * wt.x + sz.alb.rgb * wt.z, p, ng, attr, paint, field, level) / max(wt.x + wt.z, 1e-4);
            float k = (1.0 - wt.y) * smoothstep(0.7, 0.45, ng.y) * wf[0];
            alb = mix(alb, side, k);
            vec3 nx = vec3(ng.x, (sx.nrh.y * 2.0 - 1.0) + ng.y, (sx.nrh.x * 2.0 - 1.0) + ng.z);
            vec3 nz2 = vec3((sz.nrh.x * 2.0 - 1.0) + ng.x, (sz.nrh.y * 2.0 - 1.0) + ng.y, ng.z);
            nw = normalize(mix(nw, normalize(nx * wt.x + nz2 * wt.z + nw * wt.y), k));
        }
    }
    // --- detail normals (Modern, near) and micro roughness (Medium+)
    if (level >= 4 && dist < TB.g.dist.w) {
        TerLayer L0 = TB.layer[ids[0]];
        float f = (1.0 - smoothstep(TB.g.dist.w * 0.5, TB.g.dist.w, dist)) * L0.detail.y;
        if (f > 0.0 && L0.tex.x >= 0.0) {
            vec2 duv = ter_rot(xz * L0.tex.z * 4.7, 0.9);
            vec4 dn = textureGrad(terrain_nrh, vec3(duv, L0.tex.x), ter_rot(dpx.xz, 0.9) * L0.tex.z * 4.7, ter_rot(dpy.xz, 0.9) * L0.tex.z * 4.7);
            vec2 d = (dn.xy * 2.0 - 1.0) * f;
            nw = normalize(nw + vec3(d.x, 0.0, -d.y));
        }
    }
    if (level >= 2) rough = clamp(rough + (tn_noise(p.xz * 9.0) - 0.5) * TB.layer[ids[0]].detail.z, 0.04, 1.0);
    // --- the grass term beyond the grass fade: the ground takes the grass tops' colour where grass grows
    float gcov = 0.0;
    vec3 gtop = vec3(0.0);
    for (int i = 0; i < nl; i++) {
        gcov += frame.ground_color[ids[i]].w * wf[i];
        gtop += frame.ground_top[ids[i]].rgb * wf[i];
    }
    float gfade = smoothstep(TB.g.grass.x, TB.g.grass.y, dist) * TB.g.grass.z * gcov;
    if (gfade > 0.0) {
        vec3 tc = ground_macro_apply(gtop, field, 1.0);
        alb = mix(alb, tc, gfade);
        rough = mix(rough, 0.85, gfade);
        nw = normalize(mix(nw, ng, gfade * 0.5));
    }
    // --- weather hooks (weather.glsl): porosity, puddle potential (moist low ground, the pair's bias), snow
    g_wx_porosity = porosity;
    g_wx_puddle = clamp(attr.y * 0.6 - 0.25 + (has_pair ? q.e.z * band * 0.5 : 0.0) - hgt * 0.15, 0.0, 1.0);
    float line = TB.g.snow.x + (tn_noise(p.xz / max(TB.g.snow.z, 1.0)) - 0.5) * 2.0 * TB.g.snow.z;
    g_wx_snow = snow_acc * mix(0.6, 1.0, smoothstep(line - 0.5 * TB.g.snow.y, line + 0.5 * TB.g.snow.y, p.y)) + (has_pair ? q.e.w * band : 0.0);
    // --- debug views (r_terrain_debug)
    if (dbg != 0) {
        vec3 c = alb;
        if (dbg == 1) { c = vec3(0.0); for (int i = 0; i < nl; i++) c += ter_id_color(ids[i]) * wf[i]; }
        else if (dbg == 2) c = ter_id_color(g_ter_cell) * 0.6;
        else if (dbg == 3) c = vec3(0.5) + field * 0.4;
        else if (dbg == 4) c = vec3(attr.x);
        else if (dbg == 5) c = vec3(0.1, 0.2, 0.6) * attr.y + vec3(0.4, 0.3, 0.2) * (1.0 - attr.y);
        else if (dbg == 6) c = vec3(vm[0], vm[1], 0.0) * 0.8 + 0.1;
        else if (dbg == 7) c = mix(vec3(0.15), vec3(1.0, 0.25, 0.1), band) + vec3(0.0, 0.0, setw);
        else if (dbg == 8) {   // tiling frequency: tile repeats per 100 px of screen (green few .. red many)
            float rep = length(vec2(length(dpx.xz), length(dpy.xz))) * TB.layer[ids[0]].tex.z * 100.0;
            c = mix(vec3(0.1, 0.7, 0.2), vec3(0.9, 0.15, 0.1), clamp(rep / 4.0, 0.0, 1.0));
        }
        else if (dbg == 9) c = mix(vec3(0.2), vec3(1.0, 0.9, 0.2), sc_hit);
        else if (dbg == 10) c = mix(vec3(0.15), vec3(0.3, 0.8, 0.2), gfade);
        else if (dbg == 11) c = vec3(clamp(0.5 + dpair * 0.25, 0.0, 1.0), 0.2, clamp(0.5 - dpair * 0.25, 0.0, 1.0));
        alb = c;
    }
    base.rgb = alb * mix(1.0, variation, 0.3);
    n = normalize(nw);
    rough_out = clamp(rough, 0.04, 1.0);
    ao_out = clamp(ao, 0.0, 1.0);
}
