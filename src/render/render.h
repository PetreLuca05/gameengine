// Renderer public API (Vulkan 1.3 backend: dynamic rendering + synchronization2).
// Game code queues draws during the frame; everything is recorded in render_end_frame().
#ifndef DG_RENDER_H
#define DG_RENDER_H

#include "core/base.h"
#include "core/dgmath.h"
#include "anim/anim_mesh.h"
#include "render/post/post_settings.h"

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
    MAT_GRID_MOA = 1u << 10,  // sighting-in target: 1 MOA grid, spacing in tex_scale (m), bold every 5, red every 10
    MAT_SELF     = 1u << 11,  // with MAT_SHADOW_ONLY: the local player's own body, visible to phone cameras (selfies)
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
    u32 object_id;          // stable id of a moving object (0 = static): previous transform -> velocity
    u32 prev_slot;          // renderer internal
} DrawItem;

#define LIGHT_NO_SHADOW 1u   // PointLight / SpotLight flags: never shadowed (muzzle flashes)
typedef struct PointLight {
    vec3 pos;
    float radius;
    vec3 color;
    float intensity;
    u32 flags;          // LIGHT_*; lights cast shadows by default (docs/LIGHT_SHADOWS.md)
} PointLight;

typedef struct RenderCamera {
    vec3 pos;
    float yaw, pitch, roll;
    float fov_deg;      // vertical FOV
    float znear, zfar;
    float vm_fov_deg;   // arms / held item FOV when r_viewmodel_fov is 0 (the hip FOV); 0 = fov_deg
    float ads_t;        // 0..1 aim progress: the weapon's FOV blends into fov_deg, so once aimed the
                        // gun, sight and world share one projection (identical for any settings)
} RenderCamera;
// The weapon (viewmodel) FOV for a camera: hip value (r_viewmodel_fov / vm_fov_deg / fov_deg)
// blended into the camera FOV with the aim progress.
float render_vm_fov_for(const RenderCamera *c, float r_viewmodel_fov);

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
    // Procedural sky (drawn where nothing else is): off = the fog colour (indoor levels).
    bool sky;
    vec3 sky_zenith, sky_horizon;
    float clouds;       // cloud coverage 0..1
    // Physically based sky + time of day (game_daynight.c, src/render/sky): replaces the gradient
    // sky above and the hemispheric ambient (environment map + SH irradiance) when set. The key
    // light (sun_dir / sun_color / sun_intensity: shadowed) is the sun by day, the moon at night;
    // the other one is the unshadowed fill light.
    bool atmosphere;
    vec3 sun_true_dir, moon_dir;        // towards the bodies
    float sun_illuminance;              // above the atmosphere (cloud loss applied)
    float moon_illuminance;             // moonlight above the atmosphere (phase + cloud loss applied)
    float moon_full_illuminance;        // full moon (disc brightness)
    float sun_size, moon_size;          // angular radius multipliers
    vec3 cel_x, cel_y, cel_z;           // world -> celestial rows (stars, milky way)
    float star_brightness, milky_way, night_floor;
    float cloud_cover, cloud_altitude, cloud_scale, cloud_shadow_strength;
    vec2 cloud_offset;                  // wind drift (m)
    float cloud_density;                // weather: layer opacity 0..1 (0 = default 0.92)
    float cloud_softness;               // weather: edge softness 0..1 (fog-like stratus vs crisp cumulus)
    float cloud_darkness;               // weather: storm clouds darken 0..1
    float ambient_scale;                // environment (SH + specular) multiplier
    float aerial_scale;                 // world distance multiplier of the aerial perspective
    float aerial_max;                   // m covered by the aerial perspective froxels
    float mie_scale;                    // aerosol density (haze / weather)
    vec3 fill_dir, fill_color;          // unshadowed secondary directional light
    float fill_intensity;
    bool moon_key;                      // the key light is the moon
    int sky_debug;                      // sky_debug view (1 transmittance .. 6 aerial perspective)
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
    u32 object_id;          // stable id (0 = none): previous palette -> per-bone velocity
    TextureId texture;      // albedo (with MAT_TEXTURED in flags), 0 = vertex colours
} SkinnedDraw;
MeshId render_skinned_mesh_create(const SkinVertex *verts, u32 vcount, const u32 *indices, u32 icount);
void render_draw_skinned(const SkinnedDraw *d);
void render_draw_viewmodel(const DrawItem *item);
void render_add_light(const PointLight *light);
// Spot lights (flashlights): cone from pos along dir, projected cookie (render_set_spot_cookie),
// no shadows (a light at the eye needs none). Up to 4 per frame.
typedef struct SpotLight { vec3 pos, dir; float range, inner_deg, outer_deg, intensity; vec3 color; u32 flags; } SpotLight;

// Virtualized geometry placements of the loaded level (docs/VGEO.md "Levels"; mirrors LevelVgeo):
// replaces every level vgeo instance (vgeo_test_scene instances are cleared too).
typedef struct RenderVgeoPlacement {
    const char *mesh;            // "assets/vgeo/x.vgeo" or a model name with "vgeo": true
    vec3 pos, color;
    float yaw, scale, scale_jitter, spacing, roughness;
    u32 grid[2], seed;
} RenderVgeoPlacement;
void render_vgeo_set_level(const RenderVgeoPlacement *p, int count);

// Foliage on vgeo (docs/VGEO.md "Foliage support"): placement chunks (.vgp, 32 m: 4 x 4 cells of 32 B
// instances) loaded / freed with world chunks, over foliage meshes resolved by name (the procedural
// test types "test_tuft" / "test_bush", or models with "vgeo": true and a "foliage" block).
typedef struct RenderCapsule { vec3 a, b; float radius; } RenderCapsule;
typedef struct RenderFoliageHit { vec3 pos; float scale, dist; u32 instance; char type[48]; } RenderFoliageHit;
int render_foliage_test_types(void);            // registers the procedural test types (once, load time); returns their count
// The open-world map kinds (tree_pine, tree_broadleaf, tree_dead, crop_wheat; plus the test types),
// once, load time; returns their count.
int render_foliage_map_types(void);
// A placement chunk (.vgp, malloc'd; thread-safe) from the caller's instances: kind = index into names
// (registered type or vgeo model names). Instances outside chunk (cx, cz) are dropped. NULL when empty.
typedef struct RenderFoliageInst { vec3 pos; float yaw, scale; u16 kind; } RenderFoliageInst;
void *render_foliage_make_chunk(int cx, int cz, const char (*names)[48], int name_count, const RenderFoliageInst *inst, int n, size_t *size);
// A .vgp chunk of test foliage for chunk (cx, cz): jittered grid at `density` per m2 (90 % tufts, 10 %
// bushes), on height(user, x, z) (NULL: 0), inside [x0, x1) x [z0, z1). Thread-safe (job threads):
// returns a malloc'd file (size in *size), NULL when the chunk gets nothing.
void *render_foliage_test_chunk(int cx, int cz, float density, float x0, float z0, float x1, float z1, float (*height)(void *user, float x, float z), void *user,
                                size_t *size);
int render_foliage_chunk_load(const void *vgp, size_t size);   // handle, -1 on failure (unknown types are dropped)
void render_foliage_chunk_unload(int handle);
// Trunk capsules of a loaded chunk (types with "colliders": "capsule"): world segment + radius.
int render_foliage_chunk_capsules(int handle, RenderCapsule *out, int max);
// Placement instances within radius (horizontal) of pos, nearest first (footsteps, crushing, burning).
int render_foliage_query(vec3 pos, float radius, RenderFoliageHit *out, int max);
// Flattens grass: a push into the displacement buffer this frame (strength: m/s of displacement).
void render_foliage_push(vec3 pos, float radius, float strength);
void render_foliage_clear(void);                // unloads every chunk
void render_add_spot(const SpotLight *light);
void render_set_spot_cookie(TextureId cookie);

// Weather (src/game/game_weather.c, docs/WEATHER.md): surface terms (wetness, puddles, snow on
// every lit material), precipitation (rain / snow streaks and splashes around the camera, occluded
// by a top-down depth map), lens droplets.
#define RENDER_LENS_DROPS 192
typedef struct RenderLensDrop { vec4 pos_r;  /* x, y (0..1 screen), radius (fraction of height), stretch */ vec4 info; /* alpha, age, trail, unused */ } RenderLensDrop;
typedef struct RenderWeather {
    float wetness, puddles, snow_cover;   // 0..1 surface state
    float rain, snowfall;                 // precipitation intensities 0..1
    float lightning;                      // flash 0..1 this frame
    vec3 wind;                            // m/s (world), gusts included
    float gust;                           // gust factor (WeatherWind.gust): foliage WPO, FrameData wx5
    float time;                           // s (animation)
    int precip_quality;                   // 0 off, 1 low, 2 medium, 3 high
    bool occlusion_update;                // re-render the top-down occlusion map this frame
    vec3 occlusion_center;                // map centre (snapped)
    float occlusion_extent;               // half size (m)
    TextureId puddle_mask;                // R8 low-point mask over the level (0 = none)
    vec4 puddle_xform;                    // origin x, z, 1 / size x, 1 / size z
    const RenderLensDrop *drops;          // lens droplets this frame (screen space)
    int drop_count;
    float lens_strength;                  // 0 = no lens pass
    bool puddle_reflections;              // mirror-smooth puddles (SSR); off = glossy only
} RenderWeather;
void render_set_weather(const RenderWeather *w);   // every frame (zeroed = no weather)

LineVertex *render_lines_alloc(u32 vertex_count, bool depth_test);
TextVertex *render_text_alloc(u32 vertex_count); // triangle list, font atlas texture
void render_set_text_atlas(TextureId atlas);
void render_ui(const UiVertex *verts, u32 vcount, const u32 *indices, u32 icount, const UiDrawCmd *cmds, u32 ccount);

// ------------------------------------------------------------------ UI extras (src/render/render_ui.c)
// UI vertex uv ranges select the fragment mode (shaders/ui.frag): [0, 1] textured, +2 SDF text,
// +4 soft SDF (shadow / glow), uv.x < -0.5 the blurred scene behind the UI (screen space).
TextureId render_ui_backdrop(void);   // requests the blurred scene this frame; draw it with this texture
// Offscreen item views (inventory preview, icons): parts are unit meshes scaled by 2 * half.
typedef struct RenderUi3dPart {
    vec3 center; quat rot; vec3 half;
    vec4 color;               // linear albedo
    u32 shape;                // 0 box, 1 cylinder (Y axis)
    float emissive, metallic, highlight;
} RenderUi3dPart;
typedef struct RenderUi3dView {
    TextureId target;         // render_ui3d_target_create
    mat4 view, proj;
    vec4 clear;               // display-encoded RGBA (a = 0: transparent)
    const RenderUi3dPart *parts;
    int part_count;
    TextureId copy_to;        // optional: blit (scaled) into this texture's rect (icon atlas)
    int copy_x, copy_y, copy_w, copy_h;
    const char *png_path;     // optional: written after the frame, png_size square (downscaled)
    int png_size;
} RenderUi3dView;
TextureId render_ui3d_target_create(int w, int h);
void render_ui3d_target_destroy(TextureId t);
void render_ui3d(const RenderUi3dView *v);   // this frame, before the UI
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
    const RenderVfxDecal *decals;     // static first, then decal_dynamic skinned ones at the end
    int decal_count, decal_dynamic;
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
// Post stack (src/render/post): camera state the effects need, diagnostics, HDR capture.
void render_set_camera_fx(bool ads, float ads_focus_dist);   // depth of field ADS mode / focus target

// Weapon sights (game_sights.c -> post_sights.c + the scope picture-in-picture pass). Positions
// and axes are in viewmodel world space (the space the held item is drawn in), the PiP camera in
// true world space. One sight per frame: the local player's mounted optic.
typedef enum RenderSightKind { RSIGHT_NONE, RSIGHT_REDDOT, RSIGHT_SCOPE_PIP, RSIGHT_SCOPE_FULL } RenderSightKind;
typedef struct RenderSight {
    int kind;                     // RenderSightKind
    float ads_t;                  // 0 hip .. 1 aimed
    bool complete;                // ADS finished this frame (full-screen scope / ADS DoF switch)
    // Lens (red dot window / scope ocular) and the optic axis (fwd = towards the target).
    vec3 lens_pos, fwd, up, right;
    vec3 axis_fwd, axis_up, axis_right;   // the optic's own axes (eye box: reacts to the gun's residual sway); zero = fwd / up / right
    float lens_radius;
    vec3 objective_pos;           // scopes: objective lens (world, for the PiP camera)
    float objective_radius;
    // Reticle
    vec3 dot_point;               // red dot: where the collimated dot converges (parallax-free distance)
    float reticle_moa, reticle_outer_moa;
    int reticle_shape;            // ReticleShape
    vec3 reticle_color;
    float brightness;             // x sight_brightness
    bool ffp, illuminated;
    // Glass
    vec3 tint;
    float dust, fresnel, chromatic;
    // Scopes
    float zoom;                   // magnification (relative to the base FOV)
    float base_fov;               // cl_fov: the zoom reference
    float sight_fov;              // full-screen scope FOV
    float eye_box, eye_box_depth, eye_relief;
    float pip_scale;              // PiP resolution / render height
    // ADS depth of field (complete only): fixed far focus, near-field (weapon) blur.
    bool ads_dof;
    float dof_focus, dof_fstop, dof_near_blur;
    bool hide_viewmodel;          // full-screen scope: no weapon / arms
    int debug;                    // sight_debug view
} RenderSight;
void render_set_sight(const RenderSight *s);   // every frame (kind NONE = no sight)
const RenderSight *render_sight(void);         // what was set this frame
float render_scope_pip_fov(void);              // last PiP camera FOV (deg), 0 = none
// Time-of-day sky diagnostics (src/render/sky): SH irradiance in use (CPU copy, a frame late), the
// luminance of its DC term and of the latest refilter, refilter cycles completed.
vec3 render_sky_irradiance(vec3 dir);
float render_sky_luminance(float *latest);
u32 render_sky_refilters(void);
void render_camera_cut(void);                                 // teleport / respawn: reset temporal history
bool render_capture_hdr(const char *exr_path);                // next frame: linear HDR EXR
bool render_load_lut(const char *cube_path);                  // grading LUT (.cube); NULL / "" = neutral
int render_pass_count(void);                                  // frame graph passes (last completed frame)
const char *render_pass_name(int i);
float render_pass_ms(int i);
u64 render_post_memory(void);
float render_exposure_ev(void);
bool render_hdr_supported(void);
bool render_hdr_active(void);
void render_render_size(u32 *w, u32 *h);                      // internal 3D resolution
bool render_reload_shaders(void);
void render_poll_shader_hot_reload(void);

// Queries
bool render_world_to_screen(vec3 world, vec2 *out_px); // false if behind camera
vec3 render_viewmodel_to_world(vec3 viewmodel_point);   // same pixel under the world FOV (muzzle flash / tracers)
float render_vm_fov(void);   // the viewmodel (arms + held item) projection FOV this frame
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
