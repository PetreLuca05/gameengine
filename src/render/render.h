// Renderer public API (Vulkan 1.3 backend: dynamic rendering + synchronization2).
// Game code queues draws during the frame; everything is recorded in render_end_frame().
#ifndef DG_RENDER_H
#define DG_RENDER_H

#include "core/base.h"
#include "core/dgmath.h"

struct SDL_Window;

#define MAX_POINT_LIGHTS 64

typedef u32 MeshId;     // 0 = invalid
typedef u32 TextureId;  // 0 = invalid (renders as white)

typedef struct Vertex {
    vec3 pos;
    vec3 normal;
    vec2 uv;
    u32 color;  // RGBA8, r in the low byte
} Vertex;

typedef struct LineVertex { vec3 pos; u32 color; } LineVertex;
typedef struct TextVertex { vec3 pos; vec2 uv; u32 color; } TextVertex;
typedef struct UiVertex { vec2 pos; vec2 uv; u32 color; } UiVertex;

typedef struct UiDrawCmd {
    u32 index_offset;
    u32 index_count;
    TextureId texture;
    int clip_x, clip_y, clip_w, clip_h;
} UiDrawCmd;

enum MaterialFlags {
    MAT_GRID      = 1u << 0,
    MAT_UNLIT     = 1u << 1,
    MAT_NO_SHADOW = 1u << 2,  // does not cast shadows
    MAT_TRIPLANAR = 1u << 3,
    MAT_VIEWMODEL = 1u << 4,
    MAT_HIGHLIGHT = 1u << 5,  // interaction focus glow
    MAT_TEXTURED  = 1u << 6,
    MAT_CHECKER   = 1u << 7,
};

typedef struct Material {
    vec4 color;
    TextureId texture;
    float roughness;
    float metallic;
    float emissive;
    float tex_scale;
    u32 flags;
} Material;

typedef struct DrawItem {
    MeshId mesh;
    mat4 model;
    Material mat;
} DrawItem;

typedef struct PointLight {
    vec3 pos;
    float radius;
    vec3 color;
    float intensity;
} PointLight;

typedef struct RenderCamera {
    vec3 pos;
    float yaw, pitch, roll;
    float fov_deg;      // vertical FOV
    float znear, zfar;
} RenderCamera;

typedef struct RenderEnvironment {
    vec3 sun_dir;       // direction towards the sun
    vec3 sun_color;
    float sun_intensity;
    vec3 sky_ambient;
    vec3 ground_ambient;
    float ambient_intensity;
    vec3 fog_color;
    float fog_density;
    vec3 shadow_center; // where the shadow frustum is centered
    float shadow_extent;
} RenderEnvironment;

typedef struct RenderStats {
    u32 draw_calls;
    u32 triangles;
    u32 culled;
    u32 meshes_live;
    u32 textures_live;
    double gpu_mem_mb;
} RenderStats;

bool render_init(struct SDL_Window *window);
void render_shutdown(void);
void render_wait_idle(void);
void render_register_cvars(void);

MeshId render_mesh_create(const Vertex *verts, u32 vcount, const u32 *indices, u32 icount);
void render_mesh_destroy(MeshId id);
AABB render_mesh_bounds(MeshId id);

TextureId render_texture_create(int w, int h, const void *rgba8, bool srgb, bool mips);
// Raw compressed or uncompressed upload (KTX2); levels are tightly packed in data.
TextureId render_texture_create_raw(int w, int h, int levels, u32 vk_format, const void *data, const size_t *level_sizes);
void render_texture_destroy(TextureId id);

// Frame
bool render_begin_frame(void);
void render_set_camera(const RenderCamera *cam);
void render_set_environment(const RenderEnvironment *env);
void render_draw(const DrawItem *item);
void render_draw_viewmodel(const DrawItem *item);
void render_add_light(const PointLight *light);
LineVertex *render_lines_alloc(u32 vertex_count, bool depth_test);
TextVertex *render_text_alloc(u32 vertex_count); // triangle list, font atlas texture
void render_set_text_atlas(TextureId atlas);
void render_ui(const UiVertex *verts, u32 vcount, const u32 *indices, u32 icount, const UiDrawCmd *cmds, u32 ccount);
void render_end_frame(void);

void render_screenshot(const char *path);
bool render_reload_shaders(void);
void render_poll_shader_hot_reload(void);

// Queries
bool render_world_to_screen(vec3 world, vec2 *out_px); // false if behind camera
mat4 render_view_proj(void);
int render_width(void);
int render_height(void);
RenderStats render_stats(void);
const char *render_device_name(void);

DG_INLINE u32 rgba8(float r, float g, float b, float a) {
    u32 R = (u32)(saturate(r) * 255.0f + 0.5f), G = (u32)(saturate(g) * 255.0f + 0.5f);
    u32 B = (u32)(saturate(b) * 255.0f + 0.5f), A = (u32)(saturate(a) * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (A << 24);
}
DG_INLINE u32 rgba8v(vec4 c) { return rgba8(c.x, c.y, c.z, c.w); }

DG_INLINE Material material_color(vec4 color) {
    Material m;
    memset(&m, 0, sizeof(m));
    m.color = color;
    m.roughness = 0.8f;
    m.tex_scale = 1.0f;
    return m;
}

#endif
