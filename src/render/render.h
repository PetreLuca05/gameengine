// Renderer public API (Vulkan 1.3 backend: dynamic rendering + synchronization2).
// Game code queues draws during the frame; everything is recorded in render_end_frame().
#ifndef DG_RENDER_H
#define DG_RENDER_H

#include "core/base.h"
#include "core/dgmath.h"
#include "render/shadow_settings.h"
#include "anim/anim_mesh.h"
#include "render/post/post_settings.h"
#include "render/shadow_sky.h"

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
    // Surface textures (docs/TEXTURES.md, legacy lit pipeline): MAT_TERRAIN blends the layers of the
    // terrain material map (render_surface_map; vertex alpha = brightness variation), MAT_LAYERED
    // textures each vertex with the layer in its colour's alpha (255 = none), tinted by its rgb.
    MAT_TERRAIN  = 1u << 12,
    MAT_LAYERED  = 1u << 13,
    // glTF PBR material (docs/PBR.md, lit / gbuffer pipelines and vgeo): alphaMode MASK (discard below
    // alpha_cutoff; drawn two-sided), doubleSided (no back-face culling), and which map textures are set.
    MAT_ALPHA_MASK    = 1u << 14,
    MAT_TWO_SIDED     = 1u << 15,
    MAT_MR_TEX        = 1u << 16,  // metallic-roughness: g roughness, b metallic (x the factors), linear
    MAT_NORMAL_TEX    = 1u << 17,  // tangent-space normal (x normal_scale), linear
    MAT_OCCLUSION_TEX = 1u << 18,  // r = ambient occlusion (occlusion_strength), linear
    MAT_EMISSIVE_TEX  = 1u << 19,  // sRGB x emissive_rgb x emissive (replaces the albedo-tinted emissive)
    // Reconstruction (docs/REVIVE.md): skinned draws dissolve in by SkinnedDraw.recon (spine base,
    // progress); MAT_RECON_FX adds the construction edge, scan lines and the wire flash (fidelity).
    MAT_RECON    = 1u << 20,
    MAT_RECON_FX = 1u << 21,
    // Fabricator chamber (docs/REVIVE.md): glass = Fresnel-weighted opacity + rim; caustics =
    // procedural animated caustic light (emissive) on the liquid and the chamber floor.
    MAT_GLASS    = 1u << 22,
    MAT_CAUSTICS = 1u << 23,
    // Unity-style renderer shadow flags (docs/SHADOWS_UNITY_MODEL.md): Cast Shadows Off =
    // MAT_NO_SHADOW, Shadows Only = MAT_SHADOW_ONLY, Two Sided = MAT_SHADOW_TWO_SIDED (no back-face
    // culling in the shadow pass; On culls as the material does); Receive Shadows off = MAT_NO_RECEIVE.
    // Bit 24 is MAT_TERRAIN_MORPH, bits 24..27 the vgeo material's VGM_* flags (vgeo_gpu.h).
    MAT_NO_RECEIVE       = 1u << 28,
    MAT_SHADOW_TWO_SIDED = 1u << 29,
};
#define MAT_PBR_MAPS (MAT_MR_TEX | MAT_NORMAL_TEX | MAT_OCCLUSION_TEX | MAT_EMISSIVE_TEX)
// Terrain chunk geomorph (docs/LOD.md, chunks.c): uv.x = height delta to the next coarser LOD, uv.y = its
// normal (12 + 12 bit x / z), tex_scale = chunk LOD + 8 x LOD distance (m); lit.vert / shadow_morph.vert.
#define MAT_TERRAIN_MORPH (1u << 24)

typedef struct Material {
    vec4 color;
    TextureId texture;
    float roughness;
    float metallic;
    float emissive;
    float tex_scale;
    u32 flags;
    // glTF PBR maps (MAT_*_TEX says which are set; no flag = the scalar factors alone).
    TextureId mr_tex, normal_tex, occlusion_tex, emissive_tex;
    float normal_scale, occlusion_strength, alpha_cutoff;
    u32 emissive_rgb;       // RGBA8 emissive colour (glTF emissiveFactor / its largest channel), with MAT_EMISSIVE_TEX
} Material;

typedef struct DrawItem {
    MeshId mesh;
    mat4 model;
    Material mat;
    u32 object_id;          // stable id of a moving object (0 = static): previous transform -> velocity
    u32 prev_slot;          // renderer internal
    u8 layer;               // 0 .. 7: Unity-style layer (light culling masks, docs/SHADOWS_UNITY_MODEL.md)
} DrawItem;

#define LIGHT_NO_SHADOW 1u   // PointLight / SpotLight flags: never shadowed (muzzle flashes)
typedef struct PointLight {
    vec3 pos;
    float radius;
    vec3 color;
    float intensity;
    u32 flags;          // LIGHT_*; lights cast shadows by default (docs/LIGHT_SHADOWS.md)
    const LightShadowSettings *shadow;   // Unity-style shadow settings (copied); NULL = the engine default (Soft)
} PointLight;

typedef struct RenderCamera {
    vec3 pos;
    float yaw, pitch, roll;
    float fov_deg;      // vertical FOV
    float znear, zfar;
    float vm_fov_deg;   // arms / held item FOV when r_viewmodel_fov is 0 (the hip FOV); 0 = fov_deg
    float ads_t;        // 0..1 aim progress: the weapon's FOV narrows by vm_ads_zoom (fov_deg already has
                        // the mild ADS world zoom; the sight stays on the view axis = screen centre)
    float vm_ads_zoom;  // arms / weapon magnification once aimed (tan ratio; <= 1 = none)
} RenderCamera;
// The weapon (viewmodel) FOV for a camera: hip value (r_viewmodel_fov / vm_fov_deg / fov_deg)
// blended into hip / vm_ads_zoom with the aim progress.
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
    // light (sun_dir / sun_color / sun_intensity: the cascades) is the sun while its disc is up,
    // then the moon; the second light (fill_*) is the moon while the sun owns the cascades,
    // shadowed in atlas tile 3 (shadow_sky.second_shadowed).
    bool atmosphere;
    vec3 sun_true_dir, moon_dir;        // towards the bodies
    float sun_illuminance;              // above the atmosphere (cloud loss applied)
    float moon_illuminance;             // moonlight above the atmosphere (phase + cloud loss applied)
    float moon_full_illuminance;        // full moon (disc brightness)
    float sun_size, moon_size;          // angular radius multipliers
    vec3 cel_x, cel_y, cel_z;           // world -> celestial rows (stars, milky way)
    float star_brightness, milky_way, night_floor;
    float cloud_cover, cloud_altitude, cloud_scale;
    float cloud_shadow_tau;             // optical depth of a full cloud shadow texel (0 = no cloud shadows)
    vec2 cloud_offset;                  // wind drift (m)
    float cloud_density;                // weather: layer opacity 0..1 (0 = default 0.92)
    float cloud_softness;               // weather: edge softness 0..1 (fog-like stratus vs crisp cumulus)
    float cloud_darkness;               // weather: storm clouds darken 0..1
    float ambient_scale;                // environment (SH + specular) multiplier: 1 = physical (daynight.json curve; docs/SHADOWS.md)
    float aerial_scale;                 // world distance multiplier of the aerial perspective
    float aerial_max;                   // m covered by the aerial perspective froxels
    float mie_scale;                    // aerosol density (haze / weather)
    vec3 fill_dir, fill_color;          // second directional light (the moon while the sun owns the cascades)
    float fill_intensity;
    // Sky-driven shadows (shadow_sky.c, docs/SHADOWS.md): source radius, cutoff fade, wide mode,
    // PCF steps, the moon tile. Not valid (levels without the time-of-day sky): a clear sun.
    bool shadow_sky_valid;
    ShadowSkyOut shadow_sky;
    vec3 ground_albedo;                 // the sky probe's ground (snow / wet); 0 = default
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
// Surface textures (docs/TEXTURES.md): the layer arrays (assets/textures/surfaces, loaded on first
// use) and the terrain material map: n x n samples of SurfTexLayer ids (spacing m, min corner origin);
// NULL removes it. The layer ids are the order of STEX_LAYER_NAMES.
enum SurfTexLayer {
    STEX_GRASS, STEX_MEADOW, STEX_FOREST, STEX_DIRT, STEX_ROCK, STEX_SAND, STEX_GRAVEL, STEX_ASPHALT, STEX_DIRT_ROAD, STEX_CONCRETE,
    STEX_FIELD, STEX_CROP, STEX_PLOWED, STEX_MUD, STEX_BRICK, STEX_PLASTER, STEX_WOOD, STEX_METAL, STEX_RUST, STEX_SHINGLES,
    STEX_LAYER_COUNT, STEX_NONE = 255
};
bool render_surfaces_load(void);
void render_surface_map(const u8 *ids, int n, float origin_x, float origin_z, float spacing);

// Frame
bool render_begin_frame(void);
void render_set_camera(const RenderCamera *cam);
// Classic renderer (r_vgeo 0), last frame's main view: instances per LOD 0..3 and impostors (n[4]), and
// those in a cross-fade band (drawn twice, docs/LOD.md).
void render_classic_lod_counts(u32 n[5], u32 *fading);
// The scope picture-in-picture view of the last frame (docs/LOD.md "Scopes"): objective position, axis,
// tan of the half FOV, target size (px). false: not scoped.
bool render_scope_view(vec3 *eye, vec3 *fwd, float *tan_half, float *size_px);
void render_set_camera_override(bool (*fn)(RenderCamera *cam));   // replaces the camera while fn returns true (bench_fidelity flythrough)
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
    vec4 recon;             // MAT_RECON: spine base (world xyz) + progress 0..1 (w)
    // PBR maps (docs/PBR.md, docs/CHARACTERS.md "PBR maps"; MAT_*_TEX in flags says which are set):
    // skin atlases' normal / ORM (occlusion = the ORM's r) / emissive, sampled as the static path does.
    // roughness / metallic above are factors on the map. Draws of one mesh share the same maps.
    TextureId mr_tex, normal_tex, occlusion_tex, emissive_tex;
    float normal_scale;     // with MAT_NORMAL_TEX (0 is taken as 1)
    u8 layer;               // 0 .. 7 (light culling masks)
} SkinnedDraw;
MeshId render_skinned_mesh_create(const SkinVertex *verts, u32 vcount, const u32 *indices, u32 icount);
void render_draw_skinned(const SkinnedDraw *d);
void render_draw_viewmodel(const DrawItem *item);
void render_add_light(const PointLight *light);
// Spot lights (flashlights): cone from pos along dir, projected cookie (render_set_spot_cookie),
// no shadows (a light at the eye needs none). Up to 4 per frame.
typedef struct SpotLight { vec3 pos, dir; float range, inner_deg, outer_deg, intensity; vec3 color; u32 flags; const LightShadowSettings *shadow; } SpotLight;
// The directional light's (sun / moon) Unity-style shadow settings: type, resolution, strength,
// bias, normal bias, culling mask (docs/SHADOWS_UNITY_MODEL.md). Persist until changed.
void render_set_sun_shadow(const LightShadowSettings *s);
void render_get_sun_shadow(LightShadowSettings *out);

// Virtualized geometry placements of the loaded level (docs/VGEO.md "Levels"; mirrors LevelVgeo):
// replaces every level vgeo instance (vgeo_test_scene instances are cleared too).
typedef struct RenderVgeoPlacement {
    const char *mesh;            // "assets/vgeo/x.vgeo" or a model name with "vgeo": true
    vec3 pos, color;
    float yaw, scale, scale_jitter, spacing, roughness;
    u32 grid[2], seed;
    u32 shadow_flags;            // Unity-style renderer flags of every instance (VGI_NO_CAST / SHADOWS_ONLY / NO_RECEIVE, layer << 6)
} RenderVgeoPlacement;
void render_vgeo_set_level(const RenderVgeoPlacement *p, int count);

// Foliage on vgeo (docs/VGEO.md "Foliage support"): placement chunks (.vgp, 32 m: 4 x 4 cells of 32 B
// instances) loaded / freed with world chunks, over foliage meshes resolved by name (the procedural
// grass "test_tuft", the imported trees / bushes / flowers (docs/VGEO.md "Tree models"), or models with "vgeo": true and a "foliage" block).
typedef struct RenderCapsule { vec3 a, b; float radius; } RenderCapsule;
typedef struct RenderFoliageHit { vec3 pos; float scale, dist; u32 instance; char type[48]; } RenderFoliageHit;
int render_foliage_test_types(void);            // registers the tufts, imported bushes and flowers (once, load time); returns the type count
// The open-world map kinds (the imported tree species of docs/VGEO.md "Tree models", crop_wheat; plus
// the tufts and the imported bushes / flowers), once, load time; returns the type count.
int render_foliage_map_types(void);
// Scatter roles of the tree types (the order of vgeo_tree.h VgTreeRole).
enum { RENDER_TREE_CONIFER, RENDER_TREE_BROADLEAF, RENDER_TREE_DEAD, RENDER_TREE_BUSH, RENDER_TREE_FLOWER, RENDER_TREE_ROLES };
// The registered types of a role (names, pick weights; registration order: the same on every machine
// with the same assets). Returns the count (<= max).
int render_foliage_role_types(int role, char (*names)[48], float *weights, int max);
// A placement chunk (.vgp, malloc'd; thread-safe) from the caller's instances: kind = index into names
// (registered type or vgeo model names). Instances outside chunk (cx, cz) are dropped. NULL when empty.
typedef struct RenderFoliageInst { vec3 pos; float yaw, scale; u16 kind; } RenderFoliageInst;
void *render_foliage_make_chunk(int cx, int cz, const char (*names)[48], int name_count, const RenderFoliageInst *inst, int n, size_t *size);
// A .vgp chunk of test foliage for chunk (cx, cz): jittered grid at `density` per m2 (90 % tufts, the
// rest imported bushes and daisies), on height(user, x, z) (NULL: 0), inside [x0, x1) x [z0, z1). Thread-safe (job threads):
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
    vec3 wind;                            // m/s (world) at the camera, gusts included (wind_at)
    float gust;                           // gust factor (WeatherWind.gust): FrameData wx5
    float time;                           // s (animation; the foliage wind's time, slow motion included)
    // Wind field (src/world/wind.h, docs/FOLIAGE_WIND.md): this frame's render tile and the field outside it.
    const u16 *wind_texels;               // 64 x 64 RGBA16F (instant x, z, settled bend x, z), row = world texel z mod 64; NULL = none
    int wind_ox, wind_oz;                 // window origin (texels)
    float wind_texel;                     // m per texel
    vec4 wind_global;                     // outside the tile: instant x, z, settled bend x, z (m/s)
    int wind_view;                        // foliage debug view: 0 none, 1 wind channels as colours, 2 bend amount
    int wind_solo;                        // 0 every layer, 1 main bending, 2 detail bending, 3 edge flutter
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

// Water surfaces (src/render/vk_water.c, docs/WATER.md "Rendering"): the static meshes of the
// level's lakes / rivers / pools (world space, flat at their level), the ocean grid around the
// camera (displaced by the swell), the per-body look, the swell's components, the ripple sim and its
// sources. Tiers (r_water): 0 flat + sky reflection, 1 + SSR + refraction, 2 + ripple sim + caustics,
// 3 + every detail.
#define RENDER_WATER_BODIES 64
#define RENDER_WATER_WAVES 32
#define RENDER_WATER_RIPPLES 64
typedef struct RenderWaterVertex { float pos[3]; u32 body; } RenderWaterVertex;
typedef struct RenderWaterBody {
    vec4 absorb;      // rgb 1/m, w turbidity
    vec4 scatter;     // rgb 1/m, w roughness
    vec4 params;      // x level, y kind (0 puddle, 1 lake, 2 ocean, 3 river), z ripple / wave scale, w foam
    vec4 flow;        // xy flow (m/s), z wind coupling, w unused
} RenderWaterBody;
typedef struct RenderWaterRipple { vec4 pos_radius_strength; vec4 vel; } RenderWaterRipple;   // x, z, radius (m), push (m); vx, vz
typedef struct RenderWater {
    bool enabled;
    int body_count;
    RenderWaterBody body[RENDER_WATER_BODIES];
    int wave_count;
    vec4 waves[RENDER_WATER_WAVES * 2];   // WaveActive: (dx, dz, k, omega), (amp, qa, phase, 0)
    float wave_time;                      // s (waves_time of the render tick)
    vec2 wind_dir;                        // unit heading
    float wind;                           // m/s
    float rain;                           // 0..1 on open water
    int ocean_body;                       // -1: no ocean grid
    float ocean_level;
    vec3 ripple_center;                   // the ripple sim window follows this point (the camera)
    int ripple_count;
    RenderWaterRipple ripples[RENDER_WATER_RIPPLES];
    int tier;                             // r_water
} RenderWater;
void render_water_mesh(const RenderWaterVertex *v, u32 vcount, const u32 *idx, u32 icount);   // NULL / 0: none
void render_set_water(const RenderWater *w);   // every frame (enabled = false: no water pass)
// Tests (water_test waves): the GPU swell height (shaders/water/water_waves.glsl) at n points of the
// current render water's waves; xzt = (x, z, wave time) per point. Blocking (waits for the GPU).
bool render_water_gpu_heights(const float *xzt, int n, float *out);

// Per-foliage-type wind parameters (docs/FOLIAGE_WIND.md; the vgeo materials' VgWindParams, same order).
typedef struct RenderFoliageWind {
    float trunk_stiffness, branch_stiffness, leaf_stiffness, full_wind;
    float main_freq, detail_freq, flutter_freq, max_bend_deg, settle_time, response, height, pad;
} RenderFoliageWind;
// The registered foliage types (names; the wind panel's type picker).
int render_foliage_type_count(void);
const char *render_foliage_type_name(int i);
bool render_foliage_wind_get(const char *type, RenderFoliageWind *out);
bool render_foliage_wind_set(const char *type, const RenderFoliageWind *w);   // live tuning: every material of the type, bounds re-derived

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
typedef enum RenderSightKind { RSIGHT_NONE, RSIGHT_REDDOT, RSIGHT_SCOPE_PIP } RenderSightKind;
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
    float zoom;                   // magnification inside the lens (relative to the base FOV)
    float base_fov;               // the world camera's base FOV (sv_fov): the zoom reference
    float sight_fov;              // the base FOV at the sight's magnification (ads_debug)
    float eye_box, eye_box_depth, eye_relief;
    float pip_scale;              // PiP resolution / render height
    // ADS depth of field (complete only): fixed far focus, near-field (weapon) blur.
    bool ads_dof;
    float dof_focus, dof_fstop, dof_near_blur;
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
// Looks (docs/LOOKS.md): rebake every look's LUT before the next frame (look_tune, Engine Fidelity
// changes); the Settings preview strip of this frame (tiles side by side, LookId order; 0 until the
// first strip is graded).
void render_looks_invalidate(void);
TextureId render_look_preview(int *tiles, float *tile_aspect);
int render_pass_count(void);                                  // frame graph passes (last completed frame)
const char *render_pass_name(int i);
float render_pass_ms(int i);
u64 render_post_memory(void);
float render_exposure_ev(void);
// Engine Fidelity (docs/ENGINE_FIDELITY.md) diagnostics.
float render_direct_exposure_ev(void);                        // Engine Fidelity Off: the fixed (metered) exposure as EV100
float render_stretch_factor(void);                            // r_stretch: horizontal stretch of the 3D view (1 = none)
float render_gpu_frame_ms(void);                              // whole command buffer, GPU timestamps (last completed frame)
u64 render_gpu_memory_bytes(void);                            // every VMA allocation (images, buffers, pools)
void render_graph_trim(void);                                 // drop pooled images the last graph didn't use (level switch)
typedef struct RenderGraphReport {
    int passes, post_passes, dead_passes, unused_res, undefined_reads, pool_images, stale_images;
    u64 pool_bytes, stale_bytes;
    const char *names[96], *post[32], *dead[12], *unused[12], *undefined[12];
} RenderGraphReport;
void render_graph_report(RenderGraphReport *out);             // the last executed frame graph
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
// Sun shadow state of the last built frame (docs/SHADOWS.md: shadow_info, r_shadow_debug 1 / 8,
// shadow_switch_stress).
typedef struct RenderShadowInfo {
    int cascades;
    unsigned tile, vm_tile;                 // texels per atlas tile / viewmodel map
    float split[4], radius[4], texel_m[4];  // selection distance, tile half extent, texel size (m)
    float bias_m[4], nbias_m[4];            // constant / normal offset bias (m)
    vec3 center[5];                         // tile centres (4: the moon map)
    float moon_radius, distance, low_sun;
    vec3 light_dir;                         // towards the key light
    bool active[4], moon_active, vm_active, taa;
    // Unity-style controls this frame (docs/SHADOWS_UNITY_MODEL.md)
    ShadowsMode mode;
    ShadowRes resolution;
    ShadowProjection projection;
    SoftQuality soft_quality;
    float border, near_offset, shadowmask_distance;
    int shadowmask;
    LightShadowSettings sun;
    ShadowType sun_type;
    int filter_mode;                        // SHF_*
    float filter_tent;
    unsigned local_lights, local_tiles, local_atlas;
    vec3 vm_center;
    float vm_radius, vm_texel_m, vm_pcss;
    float pcss_w, max_texels, fade;         // filter state as uploaded (ssky / shadow_cfg)
    unsigned unrendered;                    // frames that sampled an unrendered tile (must stay 0)
} RenderShadowInfo;
void render_shadow_info(RenderShadowInfo *out);
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
    m.normal_scale = m.occlusion_strength = 1.0f;
    m.alpha_cutoff = 0.5f;
    m.emissive_rgb = 0xFFFFFFFFu;
    return m;
}

#endif
