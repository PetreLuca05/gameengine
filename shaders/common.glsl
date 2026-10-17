// Shared declarations. Must match FrameData / push structs in src/render/render_types.h.
#define MAX_POINT_LIGHTS 64

#define MAT_GRID       1u
#define MAT_UNLIT      2u
#define MAT_NO_SHADOW  4u
#define MAT_TRIPLANAR  8u
#define MAT_VIEWMODEL  16u
#define MAT_HIGHLIGHT  32u
#define MAT_TEXTURED   64u
#define MAT_CHECKER    128u
#define MAT_GRID_FINE  256u
#define MAT_SHADOW_ONLY 512u
#define MAT_GRID_MOA   1024u
#define MAT_TERRAIN    4096u
#define MAT_LAYERED    8192u
// glTF PBR materials (render.h, shaders/pbr_maps.glsl)
#define MAT_ALPHA_MASK    16384u
#define MAT_TWO_SIDED     32768u
#define MAT_MR_TEX        65536u
#define MAT_NORMAL_TEX    131072u
#define MAT_OCCLUSION_TEX 262144u
#define MAT_EMISSIVE_TEX  524288u
// Reconstruction / fabricator chamber (render.h, docs/REVIVE.md)
#define MAT_RECON      1048576u
#define MAT_RECON_FX   2097152u
#define MAT_GLASS      4194304u
#define MAT_CAUSTICS   8388608u
#define MAT_NO_RECEIVE       16777216u   // Receive Shadows off (docs/SHADOWS_UNITY_MODEL.md)
#define MAT_SHADOW_TWO_SIDED 33554432u

struct PointLight {
    vec4 pos_radius;
    vec4 color_intensity;
};
#define MAX_SPOT_LIGHTS 4
#define LOCAL_SHADOW_TILES 64
struct SpotLight {
    mat4 vp;              // world -> cookie clip space
    vec4 pos_range;
    vec4 dir_cos;         // xyz direction, w cos(outer half angle)
    vec4 color_intensity;
    vec4 params;          // x cos(inner half angle), y shadow tile + 1 (0 = unshadowed), z shadow normal offset per metre
};

layout(set = 0, binding = 0) uniform FrameData {
    mat4 view_proj;
    mat4 view;
    mat4 proj;
    mat4 vm_proj;
    mat4 shadow_vp;
    // Velocity (TAA / motion blur): unjittered current and previous view-projections (world and
    // viewmodel). Jitter: xy this frame, zw previous frame (NDC units).
    mat4 view_proj_nj;
    mat4 prev_view_proj_nj;
    mat4 vm_view_proj_nj;
    mat4 prev_vm_view_proj_nj;
    vec4 jitter;
    vec4 post_flags;      // x AO, y contact shadows, z SSR, w SSGI (1 = input valid)
    vec4 post_params;     // x SSR intensity, y SSGI intensity, z AO intensity, w specular ambient
    vec4 post_fog;        // x volumetric froxel fog valid, y its max distance (m)
    vec4 camera_pos;      // xyz, w = time
    vec4 sun_dir;         // xyz towards light, w = intensity
    vec4 sun_color;       // rgb, w = shadows enabled
    vec4 sky_ambient;     // rgb, w = ambient intensity
    vec4 ground_ambient;  // rgb, w = light count
    vec4 fog;             // rgb, w = density
    vec4 params;          // x = grid enabled, y = shadow texel, z = pcf radius, w = highlight pulse
    vec4 screen;          // w, h, 1/w, 1/h
    vec4 fill_dir;        // xyz towards the fill (unshadowed) directional light, w intensity
    vec4 fill_color;      // rgb
    vec4 sky_flags;       // x environment + SH valid, y ambient scale, z environment mips, w cloud shadow valid
    vec4 cshadow;         // xy cloud shadow origin (world xz), z 1 / size
    mat4 shadow_vps[4];   // cascaded shadow maps: world -> atlas tile (2 x 2 atlas, up to 4 cascades)
    vec4 shadow_splits;   // cascade selection distances (m from the camera; Close Fit: view depth), cascades 0..3
    vec4 shadow_bias;     // constant depth bias per cascade (NDC, from its texel size and the sun's Bias)
    vec4 spot_info;       // x spot light count, y cookie valid
    SpotLight spots[MAX_SPOT_LIGHTS];
    // Weather (shaders/weather.glsl)
    mat4 occl_vp;
    vec4 wx0;             // wetness, puddles, snow cover, lightning flash
    vec4 wx1;             // rain, snowfall, time, occlusion map valid
    vec4 wx2;             // wind x, z, wind strength, puddle mask valid
    vec4 wx3;             // occlusion centre x, z, half extent, top y
    vec4 wx4;             // occlusion depth range (m), precipitation quality
    vec4 wx5;             // foliage wind: gust factor, gust phase, time (s), wind direction angle (rad)
    vec4 puddle_xf;       // origin x, z, 1 / size x, 1 / size z
    PointLight lights[MAX_POINT_LIGHTS];
    // Local light shadows (src/render/vk_light_shadow.c): world -> atlas tile per cube face / spot.
    mat4 local_vps[LOCAL_SHADOW_TILES];
    vec4 light_shadow[MAX_POINT_LIGHTS];   // x first tile (-1 unshadowed), y normal offset per metre
    vec4 local_shadow_info;                // x 1 / atlas size, y shadowed point lights
    vec4 surf_map;        // terrain material map: origin x, z, 1 / spacing, samples per side (0 = none)
    vec4 surf_info;       // y: surface layers loaded (shaders/surfaces.glsl)
    // Sky-driven shadows (src/render/shadow_sky.c, docs/SHADOWS.md)
    mat4 shadow_vp_moon;  // world -> the moon map (bindings 18 / 19; the moon while the sun owns the cascades)
    vec4 ssky0;           // x tan(key source radius), y cast fade, z cascade 0 weight (0: wide mode), w tap budget (0 PCF 12, 1 PCF 16, 2 PCSS 16 + 16)
    vec4 ssky1;           // x unused, y max filter half width (texels), z blocker search (m), w moon tile fade (0 = off)
    vec4 ssky2;           // x tan(moon source radius), y r_shadow_debug view, z contact shadow length scale, w moon tile fade-out radius (m)
    vec4 shadow_mpt;      // metres per texel: cascades 0..3
    vec4 shadow_span;     // metres of light-space depth per NDC unit: cascades 0..3
    // Cascade fit, bias, filter state, the viewmodel tile (src/render/shadow_csm.c, docs/SHADOWS.md)
    mat4 shadow_vp_vm;    // world -> the viewmodel shadow map (bindings 16 / 17)
    vec4 shadow_nbias;    // normal offset (m) per cascade 0..3
    vec4 shadow_cfg;      // x kernel rotation per frame (TAA on), y PCSS weight, z PCF blocker distance (m), w smallest half width (texels)
    vec4 shadow_cfg2;     // x fixed half width (m, era 2010; 0 = from the sky), y low-sun factor (1 .. 3), z shadow distance (m), w viewmodel FOV scale
    vec4 shadow_vm;       // x metres per texel, y metres per NDC depth, z on (0 = off), w normal offset (m)
    vec4 shadow_vm2;      // x 1 / map size, y constant bias (NDC), z world-cascade skip (m), w PCSS weight of the viewmodel tile
    // Unity-style controls (src/render/shadow_settings.c, docs/SHADOWS_UNITY_MODEL.md)
    vec4 shadow_cfg3;     // x cascade count, y projection (0 Close Fit: select by view depth, 1 Stable Fit), z cascade border, w the sun's Strength
    vec4 shadow_cfg4;     // x the sun's filter (0 none, 1 hard, 2 tent, 3 PCSS), y tent base (texels)
    vec4 shadow_moon;     // moon map: x metres per texel, y metres per NDC depth, z constant bias (NDC), w normal offset (m)
    vec4 shadow_moon2;    // x 1 / map size
    vec4 local_rect[LOCAL_SHADOW_TILES];   // local light atlas squares: xy uv origin, z uv size
    vec4 local_cfg;       // x tent base of Soft local lights (texels), y Hard Only cap
} frame;

layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;
layout(set = 0, binding = 15) uniform sampler2D shadow_depth;          // the same atlas, raw depth (PCSS blocker search)
layout(set = 0, binding = 16) uniform sampler2DShadow shadow_vm_map;  // the viewmodel tile (first-person rig)
layout(set = 0, binding = 17) uniform sampler2D shadow_vm_depth;       // the same, raw depth
layout(set = 0, binding = 18) uniform sampler2DShadow shadow_moon_map; // the moon's map
layout(set = 0, binding = 19) uniform sampler2D shadow_moon_depth;
layout(set = 0, binding = 11) uniform sampler2DShadow local_shadow;   // point / spot light shadow atlas
// Time-of-day sky (src/render/sky): environment (octahedral), SH9 irradiance, cloud shadow.
layout(set = 0, binding = 5) uniform sampler2D sky_env;
layout(std430, set = 0, binding = 6) readonly buffer SkySH { vec4 sh[20]; } sky_sh;
layout(set = 0, binding = 7) uniform sampler2D sky_cloud_shadow;
// Spot light cookie (flashlight pattern), shared by every spot light.
layout(set = 0, binding = 8) uniform sampler2D spot_cookie;
// Weather: the top-down rain occlusion depth around the camera, the level's puddle mask.
layout(set = 0, binding = 9) uniform sampler2D wx_occl;
layout(set = 0, binding = 10) uniform sampler2D wx_puddles;

// Shadow passes (shadow.vert / skin_shadow.vert): flags.w selects the view: 0-3 sun cascades, 4+ a
// local light atlas square, SHADOW_PASS_MOON the moon map, SHADOW_PASS_VM the viewmodel tile,
// SHADOW_PASS_OCCL the rain occlusion map.
#define SHADOW_PASS_MOON 0x10000u
#define SHADOW_PASS_VM 0x20000u
#define SHADOW_PASS_OCCL 0x40000u
mat4 shadow_pass_vp(uint idx) {
    if (idx == SHADOW_PASS_MOON) return frame.shadow_vp_moon;
    if (idx == SHADOW_PASS_VM) return frame.shadow_vp_vm;
    if (idx == SHADOW_PASS_OCCL) return frame.occl_vp;
    if (idx >= 4u) return frame.local_vps[min(idx - 4u, uint(LOCAL_SHADOW_TILES - 1))];
    return frame.shadow_vps[idx];
}

// One axis of a tent filter of base n texels centred at c (texel space; texel i covers [i, i + 1]):
// bilinear fetch k at pos[k] (texel space) with weight w[k]; returns the fetch count (<= 4).
// shadow_settings.c shs_tent_axis is the same code (tests/test_shadow_settings.c).
float sh_tent_cdf(float x, float n) {
    float h = n * 0.5;
    if (x <= -h) return 0.0;
    if (x >= h) return 1.0;
    if (x < 0.0) { float t = x + h; return 0.5 * t * t / (h * h); }
    float t = h - x;
    return 1.0 - 0.5 * t * t / (h * h);
}
int sh_tent_axis(float c, float n, out vec4 pos, out vec4 w) {
    int nf = clamp(int(ceil((n + 1.0) * 0.5 - 1e-4)), 1, 4);
    float i0 = floor(c - n * 0.5);
    pos = vec4(0.0);
    w = vec4(0.0);
    for (int k = 0; k < nf; k++) {
        float a = i0 + 2.0 * float(k);
        float wa = sh_tent_cdf(a + 1.0 - c, n) - sh_tent_cdf(a - c, n);
        float wb = sh_tent_cdf(a + 2.0 - c, n) - sh_tent_cdf(a + 1.0 - c, n);
        w[k] = wa + wb;
        pos[k] = a + 0.5 + (w[k] > 1e-8 ? wb / w[k] : 0.0);
    }
    return nf;
}

// Local light atlas square `tile`: Hard (mode 1) = one bilinear compare fetch, Soft (mode 2) = the
// quality's tent (URP 3 x 3 / 5 x 5 / 7 x 7), clamped inside the square.
float local_shadow_tile(int tile, vec3 p, float mode) {
    vec4 sp = frame.local_vps[tile] * vec4(p, 1.0);
    if (sp.w <= 1e-4) return 1.0;
    vec3 c = sp.xyz / sp.w;
    if (c.z >= 1.0) return 1.0;
    vec2 uv = c.xy * 0.5 + 0.5;
    float texel = frame.local_shadow_info.x;
    vec4 r = frame.local_rect[tile];
    vec2 lo = r.xy + texel * 0.5, hi = r.xy + r.z - texel * 0.5;
    if (mode < 1.5) return texture(local_shadow, vec3(clamp(uv, lo, hi), c.z));
    vec2 tc = uv / texel;
    vec4 pu, wu, pv, wv;
    int nu = sh_tent_axis(tc.x, frame.local_cfg.x, pu, wu);
    int nv = sh_tent_axis(tc.y, frame.local_cfg.x, pv, wv);
    float s = 0.0;
    for (int j = 0; j < nv; j++)
        for (int i = 0; i < nu; i++) s += wu[i] * wv[j] * texture(local_shadow, vec3(clamp(vec2(pu[i], pv[j]) * texel, lo, hi), c.z));
    return s;
}

// Point light i: the cube face of the (normal-offset) point's direction from the light; the light's
// Strength blends the result towards lit.
float point_light_shadow(int i, vec3 p, vec3 n) {
    vec4 ls = frame.light_shadow[i];
    if (ls.x < 0.0) return 1.0;
    vec3 lp = frame.lights[i].pos_radius.xyz;
    vec3 op = p + n * (length(p - lp) * ls.y);
    vec3 d = op - lp, a = abs(d);
    int f = (a.x >= a.y && a.x >= a.z) ? (d.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5));
    float s = local_shadow_tile(int(ls.x) + f, op, ls.z);
    return 1.0 - ls.w * (1.0 - s);
}

// Spot light i (params.w = filter mode x 2 + Strength).
float spot_light_shadow(int i, vec3 p, vec3 n) {
    float tile = frame.spots[i].params.y - 1.0;
    if (tile < 0.0) return 1.0;
    vec3 op = p + n * (length(p - frame.spots[i].pos_range.xyz) * frame.spots[i].params.z);
    float pw = frame.spots[i].params.w;
    float mode = floor(pw * 0.5), strength = pw - 2.0 * mode;
    float s = local_shadow_tile(int(tile), op, mode);
    return 1.0 - strength * (1.0 - s);
}

// Spot light i at world position p: radiance scale (cone x cookie x distance falloff) and the
// direction towards the light.
vec3 spot_light(int i, vec3 p, out vec3 ldir) {
    vec3 dl = frame.spots[i].pos_range.xyz - p;
    float dist = length(dl);
    ldir = dl / max(dist, 1e-4);
    float range = frame.spots[i].pos_range.w;
    if (dist > range) return vec3(0.0);
    float cd = dot(-ldir, frame.spots[i].dir_cos.xyz);
    float cone = smoothstep(frame.spots[i].dir_cos.w, frame.spots[i].params.x, cd);
    if (cone <= 0.0) return vec3(0.0);
    vec3 cookie = vec3(1.0);
    if (frame.spot_info.y > 0.5) {
        vec4 c = frame.spots[i].vp * vec4(p, 1.0);
        if (c.w > 1e-4) cookie = texture(spot_cookie, c.xy / c.w * 0.5 + 0.5).rgb;
    }
    float f = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
    f = f * f / (dist * dist + 1.0);
    return frame.spots[i].color_intensity.rgb * frame.spots[i].color_intensity.w * cone * f * cookie;
}
// Previous-frame model matrices of moving draws (velocity), indexed by push flags.y - 1.
layout(std430, set = 0, binding = 4) readonly buffer DrawPrev { mat4 prev_models[]; } draw_prev;

vec3 srgb_to_linear(vec3 c) { return pow(c, vec3(2.2)); }

// Skinned characters (set 0, bindings 2 / 3): bone palettes and per-instance material data.
struct SkinInstance {
    vec4 color;
    vec4 params;    // roughness, metallic, emissive, lod fade (-1..1, 0 = fully visible)
    uvec4 info;     // x = first bone, y = material flags
    vec4 recon;     // MAT_RECON: spine base xyz, progress w (docs/REVIVE.md)
};
layout(std430, set = 0, binding = 2) readonly buffer SkinBones { mat4 bones[]; } skin_bones;
layout(std430, set = 0, binding = 3) readonly buffer SkinInstances { SkinInstance inst[]; } skin_inst;

mat4 skin_matrix(uint first, uvec4 j, vec4 w) {
    return skin_bones.bones[first + j.x] * w.x + skin_bones.bones[first + j.y] * w.y +
           skin_bones.bones[first + j.z] * w.z + skin_bones.bones[first + j.w] * w.w;
}

// Cotangent frame of a surface's uv without a tangent attribute (surface textures, glTF normal maps):
// T along +u, B along +v from the screen derivatives of position and uv, N the shading normal.
mat3 cotangent_frame(vec3 N, vec3 dpx, vec3 dpy, vec2 dux, vec2 duy) {
    vec3 dp2perp = cross(dpy, N), dp1perp = cross(N, dpx);
    vec3 T = dp2perp * dux.x + dp1perp * duy.x;
    vec3 B = dp2perp * dux.y + dp1perp * duy.y;
    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    return mat3(T * invmax, B * invmax, N);
}
