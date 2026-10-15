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
    vec4 camera_pos;      // xyz, w = time
    vec4 sun_dir;         // xyz towards light, w = intensity
    vec4 sun_color;       // rgb, w = shadows enabled
    vec4 sky_ambient;     // rgb, w = ambient intensity
    vec4 ground_ambient;  // rgb, w = light count
    vec4 fog;             // rgb, w = density
    vec4 params;          // x = grid enabled, y = shadow texel, z = pcf radius, w = highlight pulse
    vec4 screen;          // w, h, 1/w, 1/h
    PointLight lights[MAX_POINT_LIGHTS];
} frame;

layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;

vec3 srgb_to_linear(vec3 c) { return pow(c, vec3(2.2)); }
