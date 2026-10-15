// Renderer public API (Vulkan 1.3 backend: dynamic rendering + synchronization2).
// Game code queues draws during the frame; everything is recorded in render_end_frame().
#ifndef DG_RENDER_H
#define DG_RENDER_H

#include "core/base.h"
#include "core/dgmath.h"
#include "anim/anim_mesh.h"

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
    MAT_GRID_FINE = 1u << 8,  // 10 cm / 50 cm / 1 m measuring grid (recoil wall)
    MAT_SHADOW_ONLY = 1u << 9, // casts shadows but is not drawn (the local player's own body)
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

// Skinned characters: GPU skinning with per-frame bone palettes (SSBO), instanced draws batched
// by mesh. palette[i] = bone_world[i] * inverse_bind[i] (vertices are in bind model space).
typedef struct SkinnedDraw {
    MeshId mesh;            // from render_skinned_mesh_create
    const mat4 *palette;
    u32 bone_count;
    vec4 color;
    float roughness, metallic, emissive;
    u32 flags;              // MaterialFlags (MAT_SHADOW_ONLY, MAT_HIGHLIGHT, ...)
    float fade;             // LOD cross-fade: 0 = visible, (0, 1] fading in, [-1, 0) fading out
    bool cast_shadow;
    vec3 center;            // bounding sphere (world) for culling
    float radius;
} SkinnedDraw;
MeshId render_skinned_mesh_create(const SkinVertex *verts, u32 vcount, const u32 *indices, u32 icount);
void render_draw_skinned(const SkinnedDraw *d);
void render_draw_viewmodel(const DrawItem *item);
void render_add_light(const PointLight *light);
LineVertex *render_lines_alloc(u32 vertex_count, bool depth_test);
TextVertex *render_text_alloc(u32 vertex_count); // triangle list, font atlas texture
void render_set_text_atlas(TextureId atlas);
void render_ui(const UiVertex *verts, u32 vcount, const u32 *indices, u32 icount, const UiDrawCmd *cmds, u32 ccount);
void render_end_frame(void);

// ------------------------------------------------------------------ VFX (src/render/vk_vfx.c)
// GPU particle simulation (compute), sorted / additive / distortion particle passes, decals and
// bloom. The game layer (src/vfx/vfx.c) feeds one RenderVfxFrame per rendered frame.
struct VfxLibrary;
// Must match CpuSprite / Decal in shaders/vfx_common.glsl (std430).
typedef struct RenderVfxSprite { vec4 pos_size; vec4 vel_rot; vec4 color; u32 info[4]; } RenderVfxSprite;
typedef struct RenderVfxDecal { vec4 pos_size; vec4 normal_depth; vec4 tangent_angle; vec4 uv; vec4 nuv; vec4 tint; u32 info[4]; } RenderVfxDecal;
typedef struct RenderVfxFrame {
    const void *spawns;          // VfxGpuSpawn[spawn_count] (src/vfx/vfx_sim.h)
    int spawn_count;
    const RenderVfxSprite *sprites;   // CPU tier: additive first (sprite_add), then alpha back to front
    int sprite_add, sprite_alpha;
    const RenderVfxDecal *decals;
    int decal_count;
    const vec4 *instances;       // [2 * slot]: pos, rot (local-space emitters)
    int instance_count;
    float dt;                    // scaled simulation dt for this frame
    float timescale;
    vec3 wind;
    float exposure_visibility;   // exposure-aware emitters: 1 at night .. ~0.15 in full daylight
    bool lighting, collision;
    bool reset;                  // clear all GPU particles
    int max_sorted;              // alpha particles sorted per frame (quality)
} RenderVfxFrame;
typedef struct RenderVfxStats {
    u32 alive, emitted, dropped, add, alpha, distort, sort_n, pool;
    float ms_compute, ms_decals, ms_particles, ms_bloom;   // GPU time (last completed frame)
    int atlas_pages, atlas_textures;
} RenderVfxStats;
// Packs and uploads every texture of the library (fills its atlas placements) and the emitter
// table. Call after (re)loading the library.
bool render_vfx_set_library(struct VfxLibrary *lib);
void render_vfx_frame(const RenderVfxFrame *f);
// Re-uploads only the emitter table + curve LUTs (editor tweaks; textures unchanged).
void render_vfx_update_emitters(struct VfxLibrary *lib);
void render_vfx_stats(RenderVfxStats *out);
void render_set_exposure_boost(float boost);

void render_screenshot(const char *path);
bool render_reload_shaders(void);
void render_poll_shader_hot_reload(void);

// Queries
bool render_world_to_screen(vec3 world, vec2 *out_px); // false if behind camera
vec3 render_viewmodel_to_world(vec3 viewmodel_point);   // same pixel under the world FOV (muzzle flash / tracers)
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
