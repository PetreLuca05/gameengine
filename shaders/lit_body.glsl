// Shared lit shading (lit.frag, skin.frag). The includer defines MAT_COLOR, MAT_PARAMS, MAT_FLAGS;
// PBR_MAPS adds the glTF material maps (shaders/pbr_maps.glsl), LIT_ALPHA_MASK the alphaMode MASK test.
#include "weather.glsl"
// LIT_CUSTOM_INPUTS: the includer provides v_world, v_normal, v_uv, v_color and LIT_FRONT_FACING
// itself (the vgeo lit resolve decodes them per pixel, shaders/vg_lit.frag).
#ifndef LIT_CUSTOM_INPUTS
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_color;
#define LIT_FRONT_FACING gl_FrontFacing
#endif
// The material texture (the vgeo lit resolve samples a bindless array instead).
#ifndef LIT_TEX
#define LIT_TEX(uv) texture(tex, (uv))
#endif

layout(set = 1, binding = 0) uniform sampler2D tex;
#ifdef LIT_SURFACES
#include "surfaces.glsl"
#endif
#ifdef PBR_MAPS
#include "pbr_maps.glsl"
#endif
// Screen-space inputs from the post stack (push descriptor, render resolution): ambient occlusion
// (indirect only), sun contact shadows, reflections (rgb + confidence), diffuse GI.
layout(set = 2, binding = 0) uniform sampler2D post_ao;
layout(set = 2, binding = 1) uniform sampler2D post_contact;
layout(set = 2, binding = 2) uniform sampler2D post_ssr;
layout(set = 2, binding = 3) uniform sampler2D post_ssgi;
layout(set = 2, binding = 4) uniform sampler3D post_froxel;   // integrated volumetric fog (blended draws)

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265;

// Anti-aliased grid line intensity for coordinate c with spacing s and width w (in pixels).
float grid_line(float c, float s, float w) {
    float x = c / s;
    float d = abs(fract(x - 0.5) - 0.5) / max(fwidth(x), 1e-5);
    return 1.0 - clamp(d / w, 0.0, 1.0);
}

vec3 apply_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    vec2 axis_tint_sel;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.25, 0.8), grid_line(c.y, 0.25, 0.8));
    float major = max(grid_line(c.x, 1.0, 1.3), grid_line(c.y, 1.0, 1.3));
    float big   = max(grid_line(c.x, 5.0, 2.0), grid_line(c.y, 5.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.72, minor * 0.6);
    col = mix(col, col * 0.35, major);
    col = mix(col, vec3(0.9, 0.55, 0.1) * 0.5, big * 0.6);
    // Highlight the world origin axes on floors: X red, Z blue.
    if (an.y > 0.9) {
        float ax = grid_line(p.z, 1000.0, 2.5) * step(abs(p.z), 0.5);
        float az = grid_line(p.x, 1000.0, 2.5) * step(abs(p.x), 0.5);
        col = mix(col, vec3(0.8, 0.1, 0.1), ax);
        col = mix(col, vec3(0.1, 0.2, 0.9), az);
    }
    return col;
}

// Measuring grid for the recoil wall: 10 cm minor, 50 cm major, 1 m bold lines.
vec3 apply_fine_grid(vec3 base, vec3 p, vec3 n) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    float minor = max(grid_line(c.x, 0.1, 0.9), grid_line(c.y, 0.1, 0.9));
    float half_m = max(grid_line(c.x, 0.5, 1.4), grid_line(c.y, 0.5, 1.4));
    float meter = max(grid_line(c.x, 1.0, 2.0), grid_line(c.y, 1.0, 2.0));
    vec3 col = base;
    col = mix(col, col * 0.62, minor * 0.7);
    col = mix(col, vec3(0.1, 0.3, 0.6), half_m * 0.8);
    col = mix(col, vec3(0.75, 0.15, 0.1), meter);
    return col;
}

// Sighting-in target: 1 MOA squares at its distance (spacing s in metres), 5 MOA bold, 10 MOA red
// (level.c snaps the target centre onto a 10 MOA crossing; the aim square is its own box).
vec3 apply_moa_grid(vec3 base, vec3 p, vec3 n, float s) {
    vec3 an = abs(n);
    vec2 c;
    if (an.y >= an.x && an.y >= an.z) c = p.xz;
    else if (an.x >= an.z) c = p.zy;
    else c = p.xy;
    s = max(s, 1e-4);
    // Each level fades out once its squares get smaller than a few pixels (readable through a
    // scope, a clean board from far away).
    float px = max(length(fwidth(c)), 1e-6);
    float f1 = clamp((s / px - 4.0) / 4.0, 0.0, 1.0), f5 = clamp((s * 5.0 / px - 4.0) / 4.0, 0.0, 1.0), f10 = clamp((s * 10.0 / px - 4.0) / 4.0, 0.0, 1.0);
    float minor = max(grid_line(c.x, s, 0.8), grid_line(c.y, s, 0.8)) * f1;
    float five = max(grid_line(c.x, s * 5.0, 1.2), grid_line(c.y, s * 5.0, 1.2)) * f5;
    float ten = max(grid_line(c.x, s * 10.0, 1.6), grid_line(c.y, s * 10.0, 1.6)) * f10;
    vec3 col = base;
    col = mix(col, col * 0.55, minor * 0.8);
    col = mix(col, vec3(0.08, 0.1, 0.12), five * 0.85);
    col = mix(col, vec3(0.8, 0.08, 0.05), ten);
    return col;
}

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

void main() {
#ifdef LIT_PRE_MAIN
    LIT_PRE_MAIN
#endif
    uint flags = MAT_FLAGS;
    vec3 n = normalize(v_normal);
    if (!LIT_FRONT_FACING) n = -n;
    vec4 base = MAT_COLOR * v_color;

    if ((flags & MAT_TEXTURED) != 0u) {
        vec4 t;
        if ((flags & MAT_TRIPLANAR) != 0u) {
            float s = MAT_PARAMS.w;
            vec3 w = abs(n);
            w /= (w.x + w.y + w.z);
            t = LIT_TEX(v_world.zy * s) * w.x + LIT_TEX(v_world.xz * s) * w.y + LIT_TEX(v_world.xy * s) * w.z;
        } else {
            t = LIT_TEX(v_uv * max(MAT_PARAMS.w, 1e-3));
        }
        base *= t;
    }
#ifdef LIT_ALPHA_MASK
    if (base.a < pbr_alpha_cutoff()) discard;
    base.a = 1.0;
#endif
    vec2 map_uv = v_uv * max(MAT_PARAMS.w, 1e-3);   // the albedo's uv (glTF maps share it)
#ifdef PBR_MAPS
    n = pbr_normal(flags, map_uv, v_world, n);
#endif
    float surf_rough = -1.0;   // >= 0: the surface textures' roughness replaces the material's
    float surf_metal = -1.0;   // >= 0: the metal layers' metallic replaces the material's
    float surf_ao = 1.0;       // the surface layers' baked AO (x SSAO)
#ifdef LIT_SURFACES
    if (frame.surf_info.y > 0.5) {
        if ((flags & MAT_TERRAIN) != 0u && frame.surf_map.w > 0.5) surf_terrain(v_world, n, base, 0.82 + 0.36 * v_color.a, surf_rough, surf_ao);
        else if ((flags & MAT_LAYERED) != 0u) surf_layered(v_world, v_uv, v_color, n, base, surf_rough, surf_metal, surf_ao);
    }
    if ((flags & (MAT_TERRAIN | MAT_LAYERED)) != 0u) base.a = MAT_COLOR.a;   // the vertex alpha is a layer / variation
#endif
    if ((flags & MAT_CHECKER) != 0u) {
        vec2 c = floor(v_world.xz * 0.25);
        float k = mod(c.x + c.y, 2.0);
        base.rgb *= mix(0.8, 1.0, k);
    }
    if ((flags & MAT_GRID) != 0u && frame.params.x > 0.5) base.rgb = apply_grid(base.rgb, v_world, n);
    if ((flags & MAT_GRID_FINE) != 0u) base.rgb = apply_fine_grid(base.rgb, v_world, n);
    if ((flags & MAT_GRID_MOA) != 0u) base.rgb = apply_moa_grid(base.rgb, v_world, n, MAT_PARAMS.w);
#ifdef LIT_BASE_HOOK
    LIT_BASE_HOOK
#endif

    vec3 color;
    if ((flags & MAT_UNLIT) != 0u) {
        color = base.rgb;
    } else {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
        float rough = clamp(surf_rough >= 0.0 ? surf_rough : MAT_PARAMS.x, 0.04, 1.0);
        float metal = clamp(surf_metal >= 0.0 ? surf_metal : MAT_PARAMS.y, 0.0, 1.0);
#ifdef PBR_MAPS
        pbr_metal_rough(flags, map_uv, rough, metal);
        rough = max(rough, 0.04);
#endif
        vec3 albedo = base.rgb;
        // Screen-space inputs (never on the viewmodel: it is excluded from the screen-space passes).
        bool vm = (flags & MAT_VIEWMODEL) != 0u;
        // Weather: wet / puddles / snow (shaders/weather.glsl; the G-buffer applies the same terms).
        float wx_snow = weather_surface(v_world, n, albedo, rough, metal, vm);
        vec2 suv = gl_FragCoord.xy * frame.screen.zw;
        float ao = (!vm && frame.post_flags.x > 0.5) ? mix(1.0, texture(post_ao, suv).r, frame.post_params.z) : 1.0;
#ifdef PBR_MAPS
        ao *= pbr_occlusion(flags, map_uv);   // material AO x SSAO (indirect diffuse + specular occlusion)
#endif
        ao *= surf_ao;                        // the surface layers' baked AO, the same way
        float contact = (!vm && frame.post_flags.y > 0.5) ? texture(post_contact, suv).r : 1.0;
        contact = mix(1.0, contact, frame.ssky0.y);   // contact shadows leave with the cast shadows (sky cutoff)
        // hemispheric ambient (indirect diffuse): AO only darkens indirect light
        float hemi = n.y * 0.5 + 0.5;
        vec3 ambient = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, hemi) * frame.sky_ambient.w;
        bool sky_env_on = frame.sky_flags.x > 0.5;
        if (sky_env_on) ambient = sky_irradiance(n) / PI * frame.sky_flags.y;   // time-of-day sky (Lambert: E / pi)
        if (!vm && frame.post_flags.w > 0.5) ambient += texture(post_ssgi, suv).rgb * frame.post_params.y;
        color = ambient * albedo * (1.0 - metal) * ao;
        // indirect specular: sky / ground by the reflection vector, replaced by SSR where it hit
        vec3 rv = reflect(-v, n);
        float ndv0 = max(dot(n, v), 1e-4);
        vec3 f0 = mix(vec3(0.04), albedo, metal);
        vec3 fr = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - ndv0, 5.0);
        vec3 env = mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, rv.y * 0.5 + 0.5) * frame.sky_ambient.w;
        if (sky_env_on) env = textureLod(sky_env, sky_env_uv(rv), rough * frame.sky_flags.z).rgb * frame.sky_flags.y;
        vec3 spec_ind = env;
        if (!vm && frame.post_flags.z > 0.5) {
            vec4 ssr = texture(post_ssr, suv);
            spec_ind = mix(env, ssr.rgb, clamp(ssr.a * frame.post_params.x, 0.0, 1.0));
        }
        float spec_occ = clamp(pow(ndv0 + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
        color += spec_ind * fr * frame.post_params.w * spec_occ;
        // sun
        vec3 l = normalize(frame.sun_dir.xyz);
        float sh = shadow_factor(v_world, n) * contact * cloud_shadow_at(v_world);
        color += brdf(n, v, l, albedo, rough, metal) * frame.sun_color.rgb * frame.sun_dir.w * sh;
        // Snow sparkle: tiny glints facing the light.
        if (wx_snow > 0.05) {
            float g = step(0.9965, wx_hash(floor(v_world.xz * 90.0) + floor(dot(v, vec3(13.0, 7.0, 3.0)) * 8.0)));
            color += vec3(g * wx_snow * 2.5) * frame.sun_color.rgb * frame.sun_dir.w * sh * max(dot(n, l), 0.0);
        }
        // Second light: the moon while the sun owns the cascades, shadowed in atlas tile 3.
        if (frame.fill_dir.w > 0.0) color += brdf(n, v, normalize(frame.fill_dir.xyz), albedo, rough, metal) * frame.fill_color.rgb * frame.fill_dir.w * moon_shadow(v_world, n);
        // spot lights (flashlights, cookie-projected)
        for (int i = 0; i < int(frame.spot_info.x); i++) {
            vec3 sl;
            vec3 sc = spot_light(i, v_world, sl);
            if (sc.r + sc.g + sc.b > 0.0) color += brdf(n, v, sl, albedo, rough, metal) * sc * spot_light_shadow(i, v_world, n);
        }
        // point lights
        int count = int(frame.ground_ambient.w);
        for (int i = 0; i < count; i++) {
            vec3 lp = frame.lights[i].pos_radius.xyz;
            float radius = frame.lights[i].pos_radius.w;
            vec3 dl = lp - v_world;
            float dist = length(dl);
            if (dist > radius) continue;
            vec3 ldir = dl / max(dist, 1e-4);
            float falloff = clamp(1.0 - pow(dist / radius, 4.0), 0.0, 1.0);
            falloff = falloff * falloff / (dist * dist + 1.0);
            float lsh = dot(n, ldir) > 0.0 ? point_light_shadow(i, v_world, n) : 1.0;
            color += brdf(n, v, ldir, albedo, rough, metal) * frame.lights[i].color_intensity.rgb * frame.lights[i].color_intensity.w * falloff * lsh;
        }
    }
#ifdef PBR_MAPS
    if ((flags & MAT_EMISSIVE_TEX) != 0u) color += pbr_emissive(map_uv) * MAT_PARAMS.z;   // glTF: not tinted by the albedo
    else
#endif
    color += base.rgb * MAT_PARAMS.z; // emissive
#ifdef LIT_EMISSIVE_HOOK
    color += LIT_EMISSIVE_HOOK;
#endif

    if ((flags & MAT_HIGHLIGHT) != 0u) {
        vec3 v = normalize(frame.camera_pos.xyz - v_world);
        float rim = pow(1.0 - max(dot(n, v), 0.0), 2.0);
        float pulse = 0.5 + 0.5 * sin(frame.camera_pos.w * 6.0);
        color += vec3(1.0, 0.8, 0.3) * (0.15 + rim * 0.8) * (0.6 + 0.4 * pulse);
    }

    // exponential fog
    float fd = length(frame.camera_pos.xyz - v_world);
    float fog = 1.0 - exp(-fd * frame.fog.w);
    color = mix(color, frame.fog.rgb, clamp(fog, 0.0, 1.0));
#ifdef LIT_FROXEL_FOG
    // Blended surfaces are drawn after the volumetric fog pass: fog their own contribution with
    // the froxel volume (in-scattering + transmittance up to this depth).
    if (LIT_FROXEL_FOG && frame.post_fog.x > 0.5) {
        vec2 fuv = gl_FragCoord.xy * frame.screen.zw;
        float vz = max(dot(v_world - frame.camera_pos.xyz, -vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2])), 0.15);
        float w = clamp(log(vz / 0.15) / log(frame.post_fog.y / 0.15), 0.0, 1.0);
        vec4 fv = textureLod(post_froxel, vec3(fuv, w), 0.0);
        color = color * fv.a + fv.rgb;
    }
#endif

    if ((flags & MAT_CAUSTICS) != 0u) color += base.rgb * lit_caustic(v_world, frame.camera_pos.w) * 2.5;
    if ((flags & MAT_GLASS) != 0u) {
        // Glass: more opaque and brighter at grazing angles (Fresnel), clear face-on.
        vec3 vg = normalize(frame.camera_pos.xyz - v_world);
        float fr = pow(1.0 - abs(dot(n, vg)), 4.0);
        base.a = clamp(base.a + fr * 0.55, 0.0, 0.85);
        color += vec3(0.6, 0.85, 1.0) * fr * 0.6;
    }
    int sh_view = int(frame.ssky2.y);
    if (sh_view >= 2 && (flags & MAT_UNLIT) == 0u) color = shadow_debug_color(sh_view, v_world) * 0.6;
    out_color = vec4(color, base.a);
}
