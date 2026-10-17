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
// Ground cover (render.h MAT_COVER, MAT_COVER_ID)
#define MAT_COVER      268435456u
#define MAT_COVER_ID(f) (((f) >> 29u) & 7u)

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
    mat4 shadow_vps[3];   // cascaded shadow maps: world -> atlas tile (2 x 2 atlas)
    vec4 shadow_splits;   // x, y, z: cascade radii (m), w: cascade count
    vec4 shadow_bias;     // x, y, z: per-cascade depth bias scale (constant bias in metres)
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
    mat4 shadow_vp_moon;  // world -> atlas tile 3 (the moon while the sun owns the cascades)
    vec4 ssky0;           // x tan(key source radius), y cast fade, z cascade 0 weight (0: wide mode), w softness model (0 PCF steps, 1 PCSS limited, 2 full)
    vec4 ssky1;           // x PCF radius (texels, model 0 / era), y PCSS max filter half width (texels), z blocker search (m), w moon tile fade (0 = off)
    vec4 ssky2;           // x tan(moon source radius), y r_shadow_debug view, z contact shadow length scale, w moon tile fade-out radius (m)
    vec4 shadow_mpt;      // metres per texel: cascades 0..2, w the moon tile
    vec4 shadow_span;     // metres of light-space depth per NDC unit: cascades 0..2, w the moon tile
    vec4 cover[24];       // ground cover table: 3 per MAT_COVER id (grass_asset.h grass_cover_params)
    vec4 cover_info;      // x assets, y grass level, z translucency on, w r_grass_debug
    vec4 ground_color[16];   // per terrain material: mean albedo (linear), w grass top coverage
    vec4 ground_top[16];     // per terrain material: grass tops' colour (linear)
    vec4 ground_macro;    // x, y 1 / scales (m), z hue swing (rad), w value swing
    vec4 ground_macro2;   // x saturation swing, y strength (0 off)
} frame;

layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;
layout(set = 0, binding = 15) uniform sampler2D shadow_depth;          // the same atlas, raw depth (PCSS blocker search)
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

// Shadow passes (shadow.vert / skin_shadow.vert): flags.w selects the view: 0-2 sun cascades, 3 the
// rain occlusion map, 4+ a local light atlas tile, SHADOW_PASS_MOON the moon tile.
#define SHADOW_PASS_MOON 0x10000u
mat4 shadow_pass_vp(uint idx) {
    if (idx == SHADOW_PASS_MOON) return frame.shadow_vp_moon;
    if (idx >= 4u) return frame.local_vps[min(idx - 4u, uint(LOCAL_SHADOW_TILES - 1))];
    return idx == 3u ? frame.occl_vp : frame.shadow_vps[min(idx, 2u)];
}

// Local light shadow atlas: 2 x 2 hardware-PCF taps at p (world) in tile `tile`.
float local_shadow_tile(int tile, vec3 p) {
    vec4 sp = frame.local_vps[tile] * vec4(p, 1.0);
    if (sp.w <= 1e-4) return 1.0;
    vec3 c = sp.xyz / sp.w;
    if (c.z >= 1.0) return 1.0;
    vec2 uv = c.xy * 0.5 + 0.5;
    float t = frame.local_shadow_info.x * 0.75;
    float s = texture(local_shadow, vec3(uv + vec2(-t, -t), c.z));
    s += texture(local_shadow, vec3(uv + vec2(t, -t), c.z));
    s += texture(local_shadow, vec3(uv + vec2(-t, t), c.z));
    s += texture(local_shadow, vec3(uv + vec2(t, t), c.z));
    return s * 0.25;
}

// Point light i: the cube face of the (normal-offset) point's direction from the light.
float point_light_shadow(int i, vec3 p, vec3 n) {
    float tile = frame.light_shadow[i].x;
    if (tile < 0.0) return 1.0;
    vec3 lp = frame.lights[i].pos_radius.xyz;
    vec3 op = p + n * (length(p - lp) * frame.light_shadow[i].y);
    vec3 d = op - lp, a = abs(d);
    int f = (a.x >= a.y && a.x >= a.z) ? (d.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5));
    return local_shadow_tile(int(tile) + f, op);
}

float spot_light_shadow(int i, vec3 p, vec3 n) {
    float tile = frame.spots[i].params.y - 1.0;
    if (tile < 0.0) return 1.0;
    vec3 op = p + n * (length(p - frame.spots[i].pos_range.xyz) * frame.spots[i].params.z);
    return local_shadow_tile(int(tile), op);
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
