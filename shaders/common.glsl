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

struct PointLight {
    vec4 pos_radius;
    vec4 color_intensity;
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
    PointLight lights[MAX_POINT_LIGHTS];
} frame;

layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;
// Time-of-day sky (src/render/sky): environment (octahedral), SH9 irradiance, cloud shadow.
layout(set = 0, binding = 5) uniform sampler2D sky_env;
layout(std430, set = 0, binding = 6) readonly buffer SkySH { vec4 sh[20]; } sky_sh;
layout(set = 0, binding = 7) uniform sampler2D sky_cloud_shadow;
// Previous-frame model matrices of moving draws (velocity), indexed by push flags.y - 1.
layout(std430, set = 0, binding = 4) readonly buffer DrawPrev { mat4 prev_models[]; } draw_prev;

vec3 srgb_to_linear(vec3 c) { return pow(c, vec3(2.2)); }

// Skinned characters (set 0, bindings 2 / 3): bone palettes and per-instance material data.
struct SkinInstance {
    vec4 color;
    vec4 params;    // roughness, metallic, emissive, lod fade (-1..1, 0 = fully visible)
    uvec4 info;     // x = first bone, y = material flags
};
layout(std430, set = 0, binding = 2) readonly buffer SkinBones { mat4 bones[]; } skin_bones;
layout(std430, set = 0, binding = 3) readonly buffer SkinInstances { SkinInstance inst[]; } skin_inst;

mat4 skin_matrix(uint first, uvec4 j, vec4 w) {
    return skin_bones.bones[first + j.x] * w.x + skin_bones.bones[first + j.y] * w.y +
           skin_bones.bones[first + j.z] * w.z + skin_bones.bones[first + j.w] * w.w;
}
