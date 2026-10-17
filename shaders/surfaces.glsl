// Surface textures (docs/TEXTURES.md, src/render/surface_tex.c): terrain material blending and
// layered building surfaces for the legacy lit pass (lit.frag defines LIT_SURFACES).
//   surf_albedo  sRGB albedo layers (enum SurfLayer), a: the AO baked from the height (linear, 1 = open)
//   surf_nrh     r, g tangent normal (0.5 flat, g up in the image), b roughness, a height; layers
//                SURF_METAL_SLOT.. : the metal layers' masks (r bare metal, g paint wear)
//   surf_matmap  terrain material map, one layer id / 255 per raster sample (frame.surf_map)
// Outputs besides colour / normal: roughness, metallic (layered prims of metal layers) and the
// material AO, which the lit body multiplies with the SSAO (indirect diffuse + specular occlusion).
#include "surf_bindings.glsl"

#define SURF_LAYERS 20
#define SURF_TERRAIN_LAYERS 14
// Metres covered by one tile of each layer.
const float SURF_TILE[SURF_LAYERS] = float[](2.5, 3.0, 3.0, 3.0, 6.0, 4.0, 2.0, 4.0, 4.0, 4.0,
                                             3.0, 3.0, 4.0, 3.0, 1.1, 2.5, 1.4, 2.0, 2.0, 1.6);
// Terrain base colours (linear): the art direction's palette (mapgen_material_color, mapgen.c). The
// texture adds detail around its own mean, so distant (mipped) terrain keeps exactly these colours.
const vec3 SURF_BASE[SURF_TERRAIN_LAYERS] = vec3[](
    vec3(0.17, 0.25, 0.09), vec3(0.33, 0.31, 0.16), vec3(0.13, 0.12, 0.07), vec3(0.29, 0.22, 0.14), vec3(0.25, 0.24, 0.22),
    vec3(0.58, 0.52, 0.38), vec3(0.38, 0.36, 0.33), vec3(0.10, 0.10, 0.11), vec3(0.34, 0.27, 0.18), vec3(0.40, 0.39, 0.37),
    vec3(0.50, 0.41, 0.20), vec3(0.20, 0.29, 0.09), vec3(0.22, 0.16, 0.10), vec3(0.18, 0.16, 0.12));
// How much of the texture's own colour survives (1) against the base colour's hue (0).
const float SURF_CHROMA[SURF_TERRAIN_LAYERS] = float[](0.8, 0.8, 0.9, 0.7, 0.6, 0.6, 0.9, 0.6, 0.7, 0.6, 0.8, 0.8, 0.7, 0.7);
// How strongly the texture's detail (its ratio to its mean) shows (1 = as photographed).
const float SURF_CONTRAST[SURF_TERRAIN_LAYERS] = float[](0.9, 0.55, 0.8, 0.8, 0.9, 0.8, 0.9, 0.85, 0.8, 0.8, 0.5, 0.8, 0.9, 0.8);
// Strength of the baked AO per layer (1 + s (ao - 1)): strongest on the structured building layers
// (mortar joints, board gaps, corrugation, shingle rows), mild on soft ground.
const float SURF_AO[SURF_LAYERS] = float[](0.8, 0.6, 0.8, 0.8, 1.0, 0.5, 1.0, 0.7, 0.8, 0.8,
                                           0.7, 0.8, 0.9, 0.5, 1.2, 0.9, 1.1, 1.0, 1.0, 1.2);
// Metal layers: metallic of the bare metal (x the mask) and their mask layer in surf_nrh.
#define SURF_LAYER_METAL 17
#define SURF_LAYER_RUST 18
#define SURF_METAL_SLOT 20
const float SURF_METALNESS[2] = float[](0.9, 0.85);

float surf_hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float surf_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(surf_hash(i), surf_hash(i + vec2(1, 0)), f.x), mix(surf_hash(i + vec2(0, 1)), surf_hash(i + vec2(1, 1)), f.x), f.y);
}

// The layer's mean colour (its last mip) and the detail ratio texture / mean.
vec3 surf_mean(float layer) { return max(textureLod(surf_albedo, vec3(0.5, 0.5, layer), 16.0).rgb, vec3(0.004)); }

struct SurfTap { vec3 alb; vec3 nt; float rough; float h; float ao; };

// One planar tap with explicit gradients, anti-tiled: a second tap of the same layer at another
// scale and rotation takes over in noise patches (no repeat visible across a field).
SurfTap surf_tap(float layer, vec2 uv, vec2 gx, vec2 gy, float anti) {
    vec4 a = textureGrad(surf_albedo, vec3(uv, layer), gx, gy);
    vec4 b = textureGrad(surf_nrh, vec3(uv, layer), gx, gy);
    if (anti > 0.001) {
        const mat2 ROT = mat2(0.6, 0.8, -0.8, 0.6) * 0.47;
        vec2 uv2 = ROT * uv + vec2(0.37, 0.71);
        vec4 a2 = textureGrad(surf_albedo, vec3(uv2, layer), ROT * gx, ROT * gy);
        vec4 b2 = textureGrad(surf_nrh, vec3(uv2, layer), ROT * gx, ROT * gy);
        a = mix(a, a2, anti);
        b = mix(b, b2, anti);
    }
    SurfTap t;
    t.alb = a.rgb;
    t.nt = vec3(b.rg * 2.0 - 1.0, 0.0);
    t.nt.z = sqrt(max(1.0 - dot(t.nt.xy, t.nt.xy), 0.0));
    t.rough = b.b;
    t.h = b.a;
    t.ao = a.a;
    return t;
}

// The layer's AO at its strength.
float surf_ao_of(int layer, float ao) { return clamp(1.0 + SURF_AO[layer] * (ao - 1.0), 0.0, 1.0); }

// Terrain layer at p: planar from above, triplanar on steep ground (weight tri: 1 near, fading to 0 at
// the distance LOD, docs/LOD.md). World-space normal out.
SurfTap surf_terrain_layer(int layer, vec3 p, vec3 ng, vec3 dpx, vec3 dpy, float anti, float tri, out vec3 nw) {
    float s = 1.0 / SURF_TILE[layer];
    float l = float(layer);
    SurfTap top = surf_tap(l, p.xz * s, dpx.xz * s, dpy.xz * s, anti);
    // whiteout blend: image right = +x, image up = -z
    vec3 ntop = vec3(top.nt.x + ng.x, top.nt.z * ng.y, -top.nt.y + ng.z);
    if (abs(ng.y) > 0.8 || tri <= 0.0) { nw = normalize(ntop); return top; }
    vec3 w = pow(abs(ng), vec3(4.0));
    w /= w.x + w.y + w.z;
    SurfTap sx = surf_tap(l, vec2(p.z, -p.y) * s, vec2(dpx.z, -dpx.y) * s, vec2(dpy.z, -dpy.y) * s, anti);
    SurfTap sz = surf_tap(l, vec2(p.x, -p.y) * s, vec2(dpx.x, -dpx.y) * s, vec2(dpy.x, -dpy.y) * s, anti);
    vec3 nx = vec3(sx.nt.z * ng.x, sx.nt.y + ng.y, sx.nt.x + ng.z);
    vec3 nz = vec3(sz.nt.x + ng.x, sz.nt.y + ng.y, sz.nt.z * ng.z);
    nw = normalize(nx * w.x + ntop * w.y + nz * w.z);
    SurfTap t;
    t.alb = sx.alb * w.x + top.alb * w.y + sz.alb * w.z;
    t.rough = sx.rough * w.x + top.rough * w.y + sz.rough * w.z;
    t.h = sx.h * w.x + top.h * w.y + sz.h * w.z;
    t.ao = sx.ao * w.x + top.ao * w.y + sz.ao * w.z;
    if (tri < 1.0) {   // the distance band: back to the planar tap
        nw = normalize(mix(ntop, nw, tri));
        t.alb = mix(top.alb, t.alb, tri);
        t.rough = mix(top.rough, t.rough, tri);
        t.h = mix(top.h, t.h, tri);
        t.ao = mix(top.ao, t.ao, tri);
    }
    t.nt = vec3(0, 0, 1);
    return t;
}

// Terrain: up to four material-map samples around p (jittered: no grid-aligned borders), weights
// sharpened by each layer's height (stones poke through dirt instead of a smooth cross-fade).
void surf_terrain(vec3 p, inout vec3 n, inout vec4 base, float variation, out float rough, out float ao) {
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    // distance LOD: the anti-tiling tap and the side projections cost the most where the mips have
    // already flattened the detail (beyond ~100 m one planar tap per layer)
    float dist = length(p - frame.camera_pos.xyz);
    // docs/LOD.md: the ranges x frame.lod_fade.w (1 before), the triplanar end a band instead of a cut
    float ds = frame.lod_fade.w;
    float anti = smoothstep(0.35, 0.65, surf_noise(p.xz * 0.045)) * (1.0 - smoothstep(70.0 * ds, 110.0 * ds, dist));
    float tri = frame.lod_fade.z > 0.5 ? 1.0 - smoothstep(130.0 * ds, 160.0 * ds, dist) : (dist < 160.0 ? 1.0 : 0.0);
    vec2 jit = vec2(surf_noise(p.xz * 0.9), surf_noise(p.xz * 0.9 + 17.3)) - 0.5;
    vec2 g = (p.xz - frame.surf_map.xy) * frame.surf_map.z + jit * 0.9;   // raster samples at integer g
    vec2 gf = floor(g), fr = g - gf;
    int nmap = int(frame.surf_map.w);
    int ids[4];
    float wts[4];
    for (int k = 0; k < 4; k++) {
        ivec2 c = clamp(ivec2(gf) + ivec2(k & 1, k >> 1), ivec2(0), ivec2(nmap - 1));
        ids[k] = min(int(texelFetch(surf_matmap, c, 0).r * 255.0 + 0.5), SURF_TERRAIN_LAYERS - 1);
        wts[k] = ((k & 1) != 0 ? fr.x : 1.0 - fr.x) * ((k >> 1) != 0 ? fr.y : 1.0 - fr.y);
    }
    // merge equal ids
    for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++)
            if (ids[b] == ids[a] && wts[b] > 0.0) { wts[a] += wts[b]; wts[b] = 0.0; }
    vec3 ng = n;
    SurfTap taps[4];
    vec3 nws[4];
    float hmax = -1e9;
    for (int k = 0; k < 4; k++) {
        if (wts[k] <= 0.0) continue;
        taps[k] = surf_terrain_layer(ids[k], p, ng, dpx, dpy, anti, tri, nws[k]);
        hmax = max(hmax, taps[k].h + wts[k]);
    }
    vec3 alb = vec3(0.0), nn = vec3(0.0);
    float r = 0.0, o = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; k++) {
        if (wts[k] <= 0.0) continue;
        float w = max(taps[k].h + wts[k] - (hmax - 0.25), 0.0);
        int id = ids[k];
        vec3 m = surf_mean(float(id));
        vec3 ratio = taps[k].alb / m;
        float lum = dot(ratio, vec3(0.2126, 0.7152, 0.0722));
        ratio = mix(vec3(1.0), mix(vec3(lum), ratio, SURF_CHROMA[id]), SURF_CONTRAST[id]);
        alb += SURF_BASE[id] * ratio * w;
        nn += nws[k] * w;
        r += taps[k].rough * w;
        o += surf_ao_of(id, taps[k].ao) * w;
        wsum += w;
    }
    alb /= wsum;
    base.rgb = alb * variation;
    n = normalize(nn);
    rough = r / wsum;
    ao = o / wsum;
}

// Layered prim: uv in metres (u along the face, v up the face), the layer in the vertex alpha,
// the vertex colour as the paint / material colour (the texture modulates around its mean).
// Metal layers (corrugated sheet, rusted steel) set the metallic from their mask: bare metal where
// the vertex colour is unpainted (grey, low saturation), only the paint wear on painted prims
// (saturated colours: containers, cranes, hulls), none on near-black ones (tyres, soot).
// metal < 0: the material's.
void surf_layered(vec3 p, vec2 uv_m, vec4 vcol, inout vec3 n, inout vec4 base, out float rough, out float metal, out float ao) {
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec2 dux = dFdx(uv_m), duy = dFdy(uv_m);
    rough = -1.0;
    metal = -1.0;
    ao = 1.0;
    int layer = int(vcol.a * 255.0 + 0.5);
    if (layer >= SURF_LAYERS) return;
    float s = 1.0 / SURF_TILE[layer];
    vec2 uv = vec2(uv_m.x, -uv_m.y) * s;   // image up = +v
    vec2 gx = vec2(dux.x, -dux.y) * s, gy = vec2(duy.x, -duy.y) * s;
    SurfTap t = surf_tap(float(layer), uv, gx, gy, 0.0);
    vec3 ratio = clamp(t.alb / surf_mean(float(layer)), vec3(0.0), vec3(3.0));
    base.rgb *= ratio;
    mat3 tbn = cotangent_frame(n, dpx, dpy, vec2(dux.x, -dux.y), vec2(duy.x, -duy.y));
    vec3 pn = tbn * vec3(t.nt.x, -t.nt.y, t.nt.z);   // image up (+g) = -v of the flipped uv
    if (dot(pn, pn) > 1e-8) n = normalize(pn);
    rough = t.rough;
    ao = surf_ao_of(layer, t.ao);
    if (layer == SURF_LAYER_METAL || layer == SURF_LAYER_RUST) {
        int k = layer - SURF_LAYER_METAL;
        vec4 m = textureGrad(surf_nrh, vec3(uv, float(SURF_METAL_SLOT + k)), gx, gy);
        if (m.a > 0.5) {   // a = 0: no mask file (surface_tex.c), the material's metallic stays
            float hi = max(vcol.r, max(vcol.g, vcol.b)), lo = min(vcol.r, min(vcol.g, vcol.b));
            float paint = smoothstep(0.12, 0.3, (hi - lo) / max(hi, 1e-3));
            metal = SURF_METALNESS[k] * mix(m.r, m.g, paint) * smoothstep(0.04, 0.1, hi);   // near black: rubber / soot
            rough *= 1.0 - 0.3 * metal;   // bare steel is smoother than its paint / patina
        }
    }
}
