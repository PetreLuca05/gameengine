// Private renderer state shared by the render/*.c files.
#ifndef DG_VK_INTERNAL_H
#define DG_VK_INTERNAL_H

#include "volk.h"
#include "vk_mem_alloc.h"
#include "render.h"
#include "render/shadow_csm.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/cvar.h"

#define FRAMES_IN_FLIGHT 2
#define MAX_SWAP_IMAGES 8
#define MAX_MESHES 16384
#define MAX_TEXTURES 1024
#define MAX_MAT_SETS 256
#define MAX_DRAWS 16384
#define MAX_VM_DRAWS 512
#define MAX_DEFERRED 4096
#define MAX_UPLOADS 4096
#define DYN_BUFFER_SIZE (16u << 20)
#define MAX_SKIN_BONES 196608   // hordes: ~1000 characters x 65 bones + shadow draws
#define MAX_SKIN_DRAWS 4096
#define OBJ_SLOTS 16384                // power of two: object_id hash for previous transforms

#define R (g_engine.render)

// G-buffer formats (prepass).
#define GB_NORMAL_FORMAT VK_FORMAT_R16G16_SNORM
#define GB_MATERIAL_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define GB_VELOCITY_FORMAT VK_FORMAT_R16G16_SFLOAT

void vkr_check_failed(VkResult r, const char *file, int line, const char *expr);   // logs the device fault (lost device), then fatal
#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) vkr_check_failed(_r, __FILE__, __LINE__, #x); } while (0)

typedef struct GpuBuffer {
    VkBuffer buffer;
    VmaAllocation alloc;
    void *mapped;
    VkDeviceSize size;
} GpuBuffer;

typedef struct GpuMesh {
    GpuBuffer vb, ib;
    u32 vertex_count, index_count;
    AABB bounds;
    bool used;
    bool skinned;
    SkinVertex *cpu_verts;  // skinned meshes keep a CPU copy (r_cpu_skinning fallback)
    u32 *cpu_indices;
} GpuMesh;

typedef struct GpuTexture {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    VkDescriptorSet set;
    u32 w, h, mips;
    VkFormat format;
    bool used;
    bool ready;       // upload recorded
    bool wrapped;     // the image belongs to someone else (render_ui.c targets): only the set is ours
} GpuTexture;

typedef struct RenderTarget {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    VkFormat format;
    u32 w, h;
    VkImageLayout layout;
} RenderTarget;

typedef enum DeferredKind { DEFER_BUFFER, DEFER_IMAGE, DEFER_DESCRIPTOR } DeferredKind;
typedef struct Deferred {
    DeferredKind kind;
    VkBuffer buffer;
    VkImage image;
    VkImageView view;
    VmaAllocation alloc;
    VkDescriptorSet set;
} Deferred;

typedef enum UploadKind { UPLOAD_BUFFER, UPLOAD_TEXTURE, UPLOAD_IMAGE } UploadKind;
typedef struct Upload {
    UploadKind kind;
    GpuBuffer staging;
    VkBuffer dst;
    VkDeviceSize size;
    u32 texture;          // index into textures
    int levels_in_data;   // >1 for raw uploads with explicit mips
    size_t level_offsets[16];
    bool gen_mips;
    // UPLOAD_IMAGE (texture arrays outside the texture table, surface_tex.c): every mip of every layer
    // in the staging buffer, level_offsets[mip] = its layer 0 (the layers follow, tightly packed).
    VkImage image;
    u32 w, h, layers;
} Upload;

typedef struct FrameResources {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore image_available;
    GpuBuffer ubo;
    VkDescriptorSet frame_set;
    GpuBuffer ubo_pip;    // scope picture-in-picture camera (same layout as ubo)
    VkDescriptorSet frame_set_pip;
    GpuBuffer ubo_phone[2];   // phone camera views (src/render/phone_view.c)
    VkDescriptorSet frame_set_phone[2];
    GpuBuffer dyn;        // host-visible: lines, text, ui
    GpuBuffer bones;      // host-visible SSBO: skinning palettes
    GpuBuffer instances;  // host-visible SSBO: skinned instance data
    GpuBuffer prev_models; // host-visible SSBO: previous model matrices of moving draws (velocity)
    VkDeviceSize dyn_used;
    Deferred deferred[MAX_DEFERRED];
    int deferred_count;
} FrameResources;

// Every frame descriptor set of a frame slot (main, scope PiP, phone views): bindings shared by
// all of them (shadow map, sky, weather, cookie) are written to each.
#define FRAME_SET_COUNT 4
DG_INLINE void frame_sets_of(const FrameResources *f, VkDescriptorSet *out) {
    out[0] = f->frame_set;
    out[1] = f->frame_set_pip;
    out[2] = f->frame_set_phone[0];
    out[3] = f->frame_set_phone[1];
}

// Must match shaders/common.glsl (std140).
typedef struct GpuPointLight { vec4 pos_radius; vec4 color_intensity; } GpuPointLight;
// Spot lights (flashlights): cookie projection, position + range, direction + cos(outer), colour + intensity, x cos(inner).
typedef struct GpuSpotLight { mat4 vp; vec4 pos_range; vec4 dir_cos; vec4 color_intensity; vec4 params; } GpuSpotLight;
#define MAX_SPOT_LIGHTS 4
#define LOCAL_SHADOW_TILES 64     // local light shadow atlas: 8 x 8 tiles (6 per point light, 1 per spot)
#define SHADOW_PASS_MOON 0x10000u // shadow pass view index of the moon tile (shaders/common.glsl)
#define SHADOW_PASS_VM 0x20000u   // shadow pass view index of the viewmodel tile (shaders/common.glsl)
#define SHADOW_PASS_OCCL 0x40000u // shadow pass view index of the rain occlusion map (0 .. 3 are the sun cascades)
typedef struct FrameData {
    mat4 view_proj, view, proj, vm_proj, shadow_vp;
    mat4 view_proj_nj, prev_view_proj_nj, vm_view_proj_nj, prev_vm_view_proj_nj;
    vec4 jitter, post_flags, post_params, post_fog;   // post_fog: x froxel fog on, y max distance (m)
    vec4 camera_pos, sun_dir, sun_color, sky_ambient, ground_ambient, fog, params, screen;
    vec4 fill_dir;        // xyz towards the fill (unshadowed) directional light, w intensity
    vec4 fill_color;      // rgb
    vec4 sky_flags;       // x environment + SH valid, y ambient scale, z environment mips, w cloud shadow valid
    vec4 cshadow;         // xy cloud shadow origin (world xz), z 1 / size
    mat4 shadow_vps[4];   // cascaded shadow maps: world -> atlas tile (2 x 2 atlas, up to 4 cascades)
    vec4 shadow_splits;   // cascade selection distances (m from the camera; Close Fit: view depth), cascades 0..3
    vec4 shadow_bias;     // constant depth bias per cascade (NDC, from its texel size and the sun's Bias)
    vec4 spot_info;       // x spot light count, y cookie valid
    GpuSpotLight spots[MAX_SPOT_LIGHTS];
    // Weather (shaders/weather.glsl)
    mat4 occl_vp;         // world -> rain occlusion map clip (top-down ortho)
    vec4 wx0;             // wetness, puddles, snow cover, lightning flash
    vec4 wx1;             // rain, snowfall, time, occlusion map valid
    vec4 wx2;             // wind x, z, wind strength, puddle mask valid
    vec4 wx3;             // occlusion centre x, z, half extent, top y
    vec4 wx4;             // occlusion depth range (m), precipitation quality, unused, unused
    vec4 wx5;             // foliage wind: gust factor, gust phase, time (s), wind direction angle (rad, atan2(x, -z))
    vec4 puddle_xf;       // origin x, z, 1 / size x, 1 / size z
    GpuPointLight lights[MAX_POINT_LIGHTS];
    // Local light shadows (vk_light_shadow.c): world -> atlas tile per cube face / spot.
    mat4 local_vps[LOCAL_SHADOW_TILES];
    vec4 light_shadow[MAX_POINT_LIGHTS];   // x first tile (-1 unshadowed), y normal offset per metre
    vec4 local_shadow_info;                // x 1 / atlas size, y shadowed point lights
    // Surface textures (surface_tex.c, docs/TEXTURES.md): terrain material map origin x, z,
    // 1 / spacing, samples per side (0 = no map); y of surf_info: layers loaded (0 = untextured).
    vec4 surf_map;
    vec4 surf_info;
    // Sky-driven shadows (shadow_sky.c, docs/SHADOWS.md).
    mat4 shadow_vp_moon;  // world -> the moon map (bindings 18 / 19; the moon while the sun owns the cascades)
    vec4 ssky0;           // x tan(key source radius), y cast fade, z cascade 0 weight (0: wide mode), w tap budget (0 PCF 12, 1 PCF 16, 2 PCSS 16 + 16)
    vec4 ssky1;           // x unused, y max filter half width (texels), z blocker search (m), w moon tile fade (0 = off)
    vec4 ssky2;           // x tan(moon source radius), y r_shadow_debug view, z contact shadow length scale, w moon tile fade-out radius (m)
    vec4 shadow_mpt;      // metres per texel: cascades 0..3
    vec4 shadow_span;     // metres of light-space depth per NDC unit: cascades 0..3
    // LOD cross-fades (docs/LOD.md, shaders/lod_fade.glsl): x dither frame (0..63, 0 = static), y debug bits
    // (1 r_lod_fade_debug, 2 r_lod_debug 2 level colours), z 1 = the new LOD fades (0 = legacy), w terrain
    // surface detail range scale; lod_terrain: x geomorph band (octaves), y chunk diagonal (m).
    vec4 lod_fade;
    vec4 lod_terrain;
    // Cascade fit, bias, filter state, the viewmodel tile (shadow_csm.c, docs/SHADOWS.md).
    mat4 shadow_vp_vm;    // world -> the viewmodel shadow map (bindings 16 / 17)
    vec4 shadow_nbias;    // normal offset (m) per cascade 0..3
    vec4 shadow_cfg;      // x kernel rotation per frame (TAA on), y PCSS weight, z PCF blocker distance (m), w smallest half width (texels)
    vec4 shadow_cfg2;     // x fixed half width (m, era 2010; 0 = from the sky), y low-sun factor (1 .. 3), z shadow distance (m), w viewmodel FOV scale
    vec4 shadow_vm;       // x metres per texel, y metres per NDC depth, z on (0 = off), w normal offset (m)
    vec4 shadow_vm2;      // x 1 / map size, y constant bias (NDC), z world-cascade skip (m), w PCSS weight of the viewmodel tile
    // A second camera of the same frame (the scope picture-in-picture, docs/ATTACHMENTS.md): its
    // screen-space inputs (AO, contact shadows, SSR, SSGI) and the froxel fog are the main view's,
    // looked up through the main view's projection (shaders/common.glsl ss_main_uv). x: 1 = on.
    mat4 ss_view_proj;    // world -> the main view's jittered clip (what the screen-space passes saw)
    mat4 ss_view;         // world -> the main view (froxel depth)
    vec4 ss_info;
    // Unity-style controls (shadow_settings.c, docs/SHADOWS_UNITY_MODEL.md)
    vec4 shadow_cfg3;     // x cascade count, y projection (0 Close Fit: select by view depth, 1 Stable Fit), z cascade border, w the sun's Strength
    vec4 shadow_cfg4;     // x the sun's filter (0 none, 1 hard, 2 tent, 3 PCSS), y tent base (texels)
    vec4 shadow_moon;     // moon map: x metres per texel, y metres per NDC depth, z constant bias (NDC), w normal offset (m)
    vec4 shadow_moon2;    // x 1 / map size
    vec4 local_rect[LOCAL_SHADOW_TILES];   // local light atlas tiles: xy uv origin, z uv size (variable sizes)
    vec4 local_cfg;       // x tent base of Soft local lights (texels), y Hard Only cap (1)
    // Ground cover (MAT_COVER, docs/FOLIAGE_GRASS.md): 3 vec4 per cover id (grass_cover_params), x of
    // cover_info the asset count, y the grass fidelity level, z translucency on, w r_grass_debug view.
    vec4 cover[24];
    vec4 cover_info;
    // Ground colour (shaders/terrain/terrain_ground.glsl): per terrain material (raster id) the mean
    // albedo (linear rgb) and w the grass tops' coverage; ground_top: the grass tops' colour per material.
    vec4 ground_color[16];
    vec4 ground_top[16];
    // Shared macro variation (grass + terrain, terrain_macro.glsl): x, y 1 / scales (m), z hue swing
    // (rad), w value swing; ground_macro2: x saturation swing, y strength (0 off).
    vec4 ground_macro;
    vec4 ground_macro2;
} FrameData;

typedef struct PushLit {
    mat4 model;
    vec4 color;
    vec4 params;
    u32 flags[4];
    u32 pbr[4];     // glTF maps (shaders/pbr_maps.glsl): float bits of normal_scale, occlusion_strength, emissive_rgb, float bits of alpha_cutoff
} PushLit;
_Static_assert(sizeof(PushLit) == 128, "PushLit: the guaranteed push constant budget");

typedef enum PipelineId {
    PIPE_LIT, PIPE_LIT_WIRE, PIPE_LIT_BLEND, PIPE_SHADOW, PIPE_LINES, PIPE_LINES_OVERLAY,
    PIPE_TEXT, PIPE_PREPASS, PIPE_UI, PIPE_SKIN, PIPE_SKIN_WIRE, PIPE_SKIN_SHADOW, PIPE_SKIN_SHADOW_FADE, PIPE_SHADOW_MORPH, PIPE_PREPASS_SKIN, PIPE_UI3D, PIPE_PRECIP,
    // glTF material variants of PIPE_LIT / PIPE_PREPASS (docs/PBR.md): doubleSided (no culling), alphaMode MASK (discard, no culling)
    PIPE_LIT_2S, PIPE_LIT_MASK, PIPE_PREPASS_2S, PIPE_PREPASS_MASK,
    PIPE_SHADOW_MASK,   // shadow passes: alpha-masked casters (glTF MASK: foliage cards, fences)
    PIPE_SHADOW_2S,     // shadow passes: Cast Shadows Two Sided / two-sided materials (PIPE_SHADOW culls back faces)
    PIPE_SHADOW_MORPH_2S,   // the same for terrain geomorph casters (PIPE_SHADOW_MORPH culls back faces)
    PIPE_COUNT
} PipelineId;

// Must match SkinInstance in shaders/common.glsl (std430).
typedef struct GpuSkinInstance { vec4 color; vec4 params; u32 info[4]; vec4 recon; } GpuSkinInstance;

typedef struct LineBatch { VkDeviceSize offset; u32 count; } LineBatch;

typedef struct Renderer {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceProperties gpu_props;
    VkDevice device;
    VkQueue queue;
    u32 queue_family;
    VmaAllocator vma;
    bool validation;
    bool has_wireframe;
    bool has_push_descriptor, has_colorspace_ext;
    bool has_device_fault;         // VK_EXT_device_fault: fault addresses / vendor info on a lost device
    bool has_unused_attachments;   // VK_EXT_dynamic_rendering_unused_attachments: G-buffer without velocity
    bool hdr_supported, swap_hdr;
    bool vg_scalar, vg_int64, vg_indirect_count, vg_descriptor_indexing, vg_mesh_shader;   // vgeo device features
    int hdr_output_applied;
    float max_anisotropy;

    // swapchain
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    u32 swap_count;
    VkImage swap_images[MAX_SWAP_IMAGES];
    VkImageView swap_views[MAX_SWAP_IMAGES];
    VkImageLayout swap_layouts[MAX_SWAP_IMAGES];
    VkSemaphore render_done[MAX_SWAP_IMAGES];
    bool swap_dirty;
    bool vsync_applied;

    // targets
    RenderTarget hdr, depth, shadow, vm_depth;
    u32 shadow_size;
    RenderTarget vm_shadow;   // the viewmodel shadow tile (first-person rig, docs/SHADOWS.md)
    u32 vm_shadow_size;
    RenderTarget moon_shadow; // the moon's map (bindings 18 / 19)
    u32 moon_shadow_size;

    // descriptors
    VkDescriptorSetLayout frame_layout;   // set 0
    VkDescriptorSetLayout tex_layout;     // set 1
    VkDescriptorSetLayout lit_post_layout; // set 2 of layout_lit (push descriptor: AO, contact, SSR, SSGI)
    VkDescriptorSetLayout mat_layout;     // set 3 of layout_lit: glTF PBR maps (metallic-roughness, normal, occlusion, emissive)
    VkDescriptorPool pool;
    VkPipelineLayout layout_lit;          // set0 + set1 + push lit
    VkPipelineLayout layout_ui;           // set1 + push (vertex + fragment: screen scale, 1 / size)
    VkPipelineLayout layout_ui3d;         // push only (render_ui.c item views)
    VkPipeline pipes[PIPE_COUNT];
    VkSampler sampler_linear;   // material textures (aniso)
    VkSampler sampler_clamp;    // post / ui
    VkSampler sampler_shadow;   // compare
    VkSampler sampler_depth;    // nearest, no compare (PCSS blocker search)

    FrameResources frames[FRAMES_IN_FLIGHT];
    u32 frame_slot;
    u32 image_index;
    bool frame_active;

    GpuMesh meshes[MAX_MESHES];
    u32 mesh_free_hint;
    GpuTexture textures[MAX_TEXTURES];
    TextureId white_tex;
    TextureId flat_normal_tex;            // (0.5, 0.5, 1): a normal map that is not ready yet
    TextureId text_atlas;
    // glTF PBR map sets (set 3, vk_resources.c vkr_material_set): one per combination of the four
    // map textures; [0] = all white (materials without maps).
    struct MatSet { TextureId tex[4]; VkDescriptorSet set; } mat_sets[MAX_MAT_SETS];
    u32 mat_set_count;

    Upload uploads[MAX_UPLOADS];
    int upload_count;

    // per-frame queued work
    DrawItem *draws;
    u32 draw_count;
    DrawItem *vm_draws;
    u32 vm_draw_count;
    SkinnedDraw *skin_draws;      // palette pointers rewritten to offsets into skin_bones
    u32 skin_draw_count;
    mat4 *skin_bones;
    u32 skin_bone_count;
    GpuPointLight lights[MAX_POINT_LIGHTS];
    u32 light_count;
    GpuSpotLight spots[MAX_SPOT_LIGHTS];
    u32 spot_count;
    u32 light_flags[MAX_POINT_LIGHTS], spot_flags[MAX_SPOT_LIGHTS];   // LIGHT_* (render.h)
    // Unity-style per-light shadow settings (docs/SHADOWS_UNITY_MODEL.md); has_*: given explicitly
    // (otherwise the engine defaults: points 0.05 m near plane, spots 0.6 m).
    LightShadowSettings light_set[MAX_POINT_LIGHTS], spot_set[MAX_SPOT_LIGHTS];
    bool light_set_given[MAX_POINT_LIGHTS], spot_set_given[MAX_SPOT_LIGHTS];
    // Local light shadows (vk_light_shadow.c)
    RenderTarget lshadow;             // D32 atlas, LOCAL_SHADOW_TILES tiles
    u32 lshadow_tile;                 // tile size (px)
    mat4 local_vps[LOCAL_SHADOW_TILES];
    vec4 local_planes[LOCAL_SHADOW_TILES][6];   // culling planes per tile (untiled frustum)
    vec4 local_sphere[LOCAL_SHADOW_TILES];      // caster sphere per tile (light position, radius)
    u32 local_tile_count;
    vec4 local_rect[LOCAL_SHADOW_TILES];        // uv origin xy, uv size z
    u32 local_px[LOCAL_SHADOW_TILES][3];        // atlas texels: x, y, size
    float local_bias[LOCAL_SHADOW_TILES];       // the light's Bias (hardware depth bias scale)
    u32 local_layers[LOCAL_SHADOW_TILES];       // the light's culling mask
    vec4 light_shadow[MAX_POINT_LIGHTS];
    vec3 lshadow_prev[16];            // last frame's shadowed point lights (selection hysteresis)
    u32 lshadow_prev_count;
    float lshadow_ms;                 // CPU record time of the local shadow pass
    u32 lshadow_draws;
    Cvar *r_light_shadows, *r_light_shadow_count, *r_light_shadow_quality;
    RenderWeather weather;            // this frame (render_set_weather)
    RenderTarget occl;                // top-down rain occlusion depth (render.c)
    mat4 occl_vp;
    vec4 occl_info;                   // centre x, z, half extent, top y
    float occl_range;
    bool occl_valid;
    TextureId puddle_bound;           // puddle mask in frame set binding 10
    bool weather_bindings;
    TextureId spot_cookie, spot_cookie_bound;
    LineVertex *line_cpu[2];   // [0] depth tested, [1] overlay
    u32 line_count[2];
    u32 line_cap;
    TextVertex *text_cpu;
    u32 text_count, text_cap;
    UiVertex *ui_verts; u32 ui_vcount, ui_vcap;
    u32 *ui_indices; u32 ui_icount, ui_icap;
    UiDrawCmd *ui_cmds; u32 ui_ccount, ui_ccap;

    RenderCamera cam;
    RenderEnvironment env;
    mat4 view, proj, view_proj, vm_proj, shadow_vp;   // shadow_vp: the widest cascade (volumetrics, particles, decals)
    mat4 shadow_vps[4];
    vec4 shadow_splits;
    vec4 shadow_bias;
    mat4 shadow_vp_moon;                        // the moon map (moon_active)
    bool shadow_active[4];                      // shadow atlas tiles drawn this frame: cascades 0..3
    bool moon_active;
    // Unity-style controls (docs/SHADOWS_UNITY_MODEL.md): this frame's quality knobs, the
    // directional light's settings, its effective type / filter.
    QualityShadowSettings shq;
    LightShadowSettings sun_shadow;
    ShadowType sun_type;
    ShadowFilter sun_filter;
    // Cascade fit (shadow_csm.c): layout, per tile the untiled matrix (caster culling), sphere radius /
    // snapped centre (vgeo / classic LOD, r_shadow_debug 8); the viewmodel tile; filter / check state.
    CsmLayout csm;
    mat4 shadow_vp_ndc[5];                      // cascades 0..3, 4 the moon map
    float shadow_radius[5];
    vec3 shadow_center[5];
    float shadow_dist;                          // m: the smoothed shadow distance (r_shadow_distance x level reach)
    CsmTile shadow_vm_tile;
    bool shadow_vm_active, shadow_taa;
    float shadow_vm_pcss;                       // viewmodel tile PCSS weight (crossfades on a tier change)
    float shadow_dbg_bias_m;                    // cascade 0 constant bias (m), readout
    u32 shadow_drawn;                           // tiles rendered this frame: bits 0..3 atlas, 4 viewmodel, 5 moon
    u32 shadow_unrendered;                      // frames that sampled an unrendered tile (must stay 0)
    // temporal state (TAA / motion blur / velocity)
    mat4 proj_j, vm_proj_j;                     // jittered projections (this frame)
    mat4 view_proj_nj, prev_view_proj_nj, vm_view_proj_nj, prev_vm_view_proj_nj;
    vec2 jitter, prev_jitter;                   // NDC
    vec3 prev_cam_pos;
    u32 frame_index;
    bool have_prev, camera_cut;
    bool fx_ads;
    float fx_ads_focus;
    RenderSight sight;                          // this frame's weapon sight (render_set_sight)
    float pip_fov;                              // scope PiP camera FOV (deg)
    mat4 *prev_models_cpu;                      // this frame's previous-model list (index = DrawItem.prev_slot - 1)
    u32 prev_model_count;
    struct ObjSlot *obj_slots;                  // object_id -> last model / palette (hash, OBJ_SLOTS)
    mat4 *prev_bones;                           // last frame's bone palettes (copy of skin_bones)
    u32 prev_bone_count;
    float target_scale;                         // render resolution / output resolution
    vec4 frustum[6];
    float render_scale_applied;
    char screenshot_path[512];
    RenderStats stats, last_stats;

    // shader hot reload
    double next_shader_poll;
    i64 shader_src_mtime;

    // cvars
    Cvar *r_vsync, *r_wireframe, *r_render_scale, *r_aniso, *r_texture_quality;
    Cvar *r_aa, *r_brightness, *r_gamma, *r_grid, *r_vignette, *r_shader_hotreload, *r_culling;
    Cvar *r_vm_fov, *r_cpu_skinning, *r_bloom, *r_bloom_intensity, *r_hdr_output;
    // Unity-style shadow quality (docs/SHADOWS_UNITY_MODEL.md)
    Cvar *r_shadows, *r_shadow_resolution, *r_shadow_projection, *r_shadow_cascades, *r_shadow_max_distance, *r_soft_shadow_quality;
    Cvar *r_shadow_split2, *r_shadow_split4[3], *r_shadow_border, *r_shadow_near_offset, *r_shadowmask_mode, *r_shadowmask_distance;
    Cvar *r_shadow_min_radius;
    Cvar *r_shadow_debug;       // sky-driven shadow views (docs/SHADOWS.md)
    Cvar *r_vm_shadow;          // the viewmodel shadow tile
    float contact_scale;        // contact shadow ray length scale (penumbra-limited, compute_matrices)
    Cvar *r_stretch;   // stretched resolution simulator (0 normal .. 1 extreme)
    float exposure_boost;
    u32 aniso_mod, texq_mod;
} Renderer;

// render_ui.c (frame graph hooks)
void render_ui_build(int ldr, u32 out_w, u32 out_h);   // before the final pass: backdrop blur, item views
void render_ui_final_reads(int final_pass);
bool render_ui_wants_wait(void);
void render_ui_frame_done(void);
void render_ui_shutdown(void);
const RenderWeather *render_weather_state(void);   // this frame's weather (post passes)

// vk_device.c
bool vkd_create_device(struct SDL_Window *window);
void vkd_destroy_device(void);
bool vkd_create_swapchain(void);
void vkd_destroy_swapchain(void);
void vkd_create_targets(void);
void vkd_destroy_targets(void);
void vkd_create_shadow_map(u32 size);
void vkd_create_vm_shadow_map(u32 size);   // the viewmodel tile (bindings 16 / 17)
void vkd_create_moon_shadow_map(u32 size); // the moon map (bindings 18 / 19)

// vk_resources.c
GpuBuffer vkr_buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
void vkr_buffer_destroy_now(GpuBuffer *b);
GpuBuffer vkr_buffer_create_readback(VkDeviceSize size);
void vkr_defer_buffer(GpuBuffer *b);
void vkr_defer_image(VkImage image, VkImageView view, VmaAllocation alloc);
void vkr_flush_deferred(FrameResources *f);
void vkr_record_uploads(VkCommandBuffer cmd);
void vkr_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                       VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, u32 mips);
RenderTarget vkr_target_create(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect);
void vkr_target_destroy(RenderTarget *t);
void vkr_create_samplers(void);
void vkr_destroy_samplers(void);
void vkr_recreate_linear_sampler(void);   // anisotropy / texture quality change
void vkg_samplers_changed(void);          // vk_vgeo.c: re-enter the texture table with the new material sampler
VkDescriptorSet vkr_alloc_tex_set(VkImageView view, VkSampler sampler);
void vkr_write_tex_set(VkDescriptorSet set, VkImageView view, VkSampler sampler);
void vkr_rewrite_all_texture_sets(void);
// Set 3 of layout_lit for a material's glTF maps (index into R->mat_sets, 0 = no maps / table full);
// maps that are not ready yet sample white (flat for the normal map) until they are.
u32 vkr_material_set(const Material *m);
// A texture slot over an image owned elsewhere (UI render targets): sampled clamped by the UI.
TextureId vkr_texture_wrap(VkImage image, VkImageView view, u32 w, u32 h, VkFormat fmt);
void vkr_texture_rewrap(TextureId id, VkImage image, VkImageView view, u32 w, u32 h);
void vkr_texture_unwrap(TextureId id);
void vkr_destroy_all_resources(void);

// vk_pipelines.c
bool vkp_create_layouts(void);
bool vkp_create_pipelines(void);
void vkp_destroy_pipelines(void);
void vkp_destroy_layouts(void);
VkShaderModule vkp_load_shader(const char *name);
void vkp_register_shader(const char *name);   // hot reload / recompile list ("post/taa.comp")

// post/post_core.c: frame graph post-processing stack
bool post_create_pipelines(void);

// surface_tex.c: terrain / building surface texture arrays + the terrain material map (frame set
// bindings 12-14, docs/TEXTURES.md)
void vks_init(void);                       // 1-texel placeholders (the bindings are always valid)
void vks_shutdown(void);
void vks_write_bindings(void);             // before recording: rewrites the frame sets when they changed
void vks_samplers_changed(void);           // the material sampler was recreated (r_aniso, r_texture_quality)
void vks_frame_data(FrameData *fd);
// Ground cover table, ground colours and macro variation (ground.c, docs/FOLIAGE_GRASS.md).
void vkgr_init(void);
void vkgr_frame_data(FrameData *fd);
// Terrain shading resources (terrain_tex.c, docs/TERRAIN_SHADING.md): frame set bindings 18..26.
void vkt_init(void);
void vkt_shutdown(void);
void vkt_write_bindings(void);
void vkt_samplers_changed(void);
void vkt_frame_data(FrameData *fd, int slot);
void vkr_upload_image(VkImage image, u32 w, u32 h, u32 mips, u32 layers, const void *data, const size_t *mip_offsets, size_t size);
void vkr_cancel_image_upload(VkImage image);

// vk_light_shadow.c: shadows of point / spot lights (docs/LIGHT_SHADOWS.md)
void vkls_init(void);
void vkls_shutdown(void);
void vkls_select(FrameData *fd);                         // this frame's shadowed lights, tiles, matrices -> frame data
void vkls_record(VkCommandBuffer cmd, FrameResources *f); // renders the atlas tiles (after the sun shadow pass)
// render.c: draws every shadow caster (static + GPU-skinned) touching sphere and inside planes,
// with shadow cascade index `cascade` (push constants flags.w). Returns the draw count.
u32 render_draw_shadow_casters(VkCommandBuffer cmd, FrameResources *f, vec4 sphere, const vec4 planes[6], u32 cascade, u32 layers);

// vk_vgeo.c: virtualized geometry in the frame (docs/VGEO.md)
void vkg_init(void);
void vkg_shutdown(void);
void vkg_graph_cull(int depth_res);    // frame graph, before the prepass: culling against last frame's Hi-Z, raster pass 1
bool vkg_wants_occluders(void);        // render.c adds vgeo_occluders (static legacy depth) between the two
void vkg_graph_cull_pass2(int depth_res);   // this frame's Hi-Z (vgeo + static occluders), pass 2 cull + raster
void vkg_graph_hiz(int depth_res, int velocity_res);     // frame graph, after the prepass: this frame's Hi-Z (next frame's history)
void vkg_record_shadow_cull(VkCommandBuffer cmd);   // before the shadow pass: cascade views
void vkg_draw_shadow(VkCommandBuffer cmd, u32 cascade);   // inside the shadow pass, per cascade tile (3 = the moon tile)
void vkg_draw_gbuffer(VkCommandBuffer cmd);   // prepass, before the legacy draws
void vkg_draw_lit(VkCommandBuffer cmd);       // opaque pass
void vkg_frame_update(void);                  // per frame before recording (render_set_camera): grass fidelity level
void vkg_bench_camera(RenderCamera *cam);     // vgeo_bench: spline camera override (render_set_camera)
void vkg_bench_frame(void);                   // vgeo_bench: per-frame timings
void vkg_submit_fallback(void);
// Skinned draws vgeo takes this frame (main view): routed[i] = 1; returns their count.
u32 vkg_route_skinned(const SkinnedDraw *draws, u32 n, const mat4 *bones, u8 *routed);
// Point / spot light shadow tile: vgeo instances near the light as discrete LOD meshes (PIPE_SHADOW bound).
u32 vkg_draw_local_shadow(VkCommandBuffer cmd, vec4 sphere, const vec4 planes[6], u32 cascade, u32 tile_px, u32 layers);               // r_era 2015 / 2010: discrete LODs through the legacy draws (after compute_matrices)

// vk_water.c: water surfaces (docs/WATER.md "Rendering")
void vkw_init(void);
void vkw_shutdown(void);
bool vkw_create_pipelines(void);
// Frame graph, after the opaque scene is fogged and before the scene copy: snapshot, ripple sim,
// surface pass (writes hdr, depth, velocity).
void vkw_build_graph(int hdr, int depth, int velocity);
void vkw_build_graph_pip(int hdr, int depth, mat4 view_proj);   // the scope PiP (after its fog)
// vk_classic.c: the classic instanced renderer of the vgeo scene content (r_vgeo 0, docs/CLASSIC_RENDERER.md)
struct VgScene;
struct VgBuilt;
struct VgDisplace;
void vkc_init(void);
void vkc_shutdown(void);
void vkc_register_mesh(u32 mesh, const struct VgBuilt *b, bool persistent);   // LOD chain + impostor textures
struct VgTextures;
void vkc_set_textures(const struct VgTextures *t);
void vkc_samplers_changed(void);                              // the vgeo texture table (module-owned slots)
void vkc_chunk_loaded(const struct VgScene *s, u32 handle);                    // placement chunk: per-cell distance limits
// views: r_vgeo 0 (main view + sun cascade batches); false: the scene for the local light tiles only.
void vkc_frame(const struct VgScene *s, const struct VgDisplace *disp, float density, float lod_px, float shadow_lod_px, float small_impostor_px, bool era2010,
               bool shadows, bool views);
struct VgViewGpu;
struct VgWindTex;
void vkc_wind_frame(const struct VgViewGpu *v, const struct VgWindTex *w);   // after the wind tile upload: the frame's wind (set 4)
void vkc_draw_gbuffer(VkCommandBuffer cmd);   // inside the prepass (frame set bound)
void vkc_draw_lit(VkCommandBuffer cmd);       // inside the opaque pass (depth EQUAL)
void vkc_draw_pip(VkCommandBuffer cmd);       // inside the scope PiP pass (own culling / LOD, lit forward; docs/LOD.md)
// The scope PiP view of this frame (setup_scope_pip): view-projection, eye, pixels per unit tangent; false: none.
bool render_scope_cull(mat4 *vp, vec3 *eye, float *px_scale);
void vkc_draw_shadow(VkCommandBuffer cmd, u32 cascade);   // inside the sun shadow pass
u32 vkc_draw_local_shadow(VkCommandBuffer cmd, vec4 sphere, const vec4 planes[6], u32 view, u32 tile_px, u32 layers);
void vkc_stats(void);

// vk_vfx.c: GPU particles, decals, distortion
bool vkx_init(void);
void vkx_shutdown(void);
bool vkx_create_pipelines(void);
void vkx_targets_changed(void);               // after vkd_create_targets (depth / scene copy / bloom views)
void vkx_record_compute(VkCommandBuffer cmd); // frame start, before the shadow pass
void vkx_draw_decals(VkCommandBuffer cmd, bool dynamic);    // inside the scene pass, depth read-only
void vkx_copy_scene(VkCommandBuffer cmd);     // outside rendering: HDR -> scene copy (distortion)
void vkx_draw_particles(VkCommandBuffer cmd); // inside the scene pass, depth read-only
void vkx_draw_particles_pip(VkCommandBuffer cmd, VkDescriptorSet frame_set);
// Inside the scope PiP decal pass (PiP depth as the read-only attachment and sampled).
void vkx_draw_decals_pip(VkCommandBuffer cmd, VkDescriptorSet frame_set, VkImageView depth_view, mat4 view_proj, mat4 view, mat4 proj, vec3 cam, u32 w, u32 h);   // inside the scope picture-in-picture pass
void vkx_timestamp(VkCommandBuffer cmd, int slot);
RenderTarget *vkx_scene_copy(void);           // opaque HDR copy (distortion; TAA "hdr_opaque"), SHADER_READ_ONLY
void vkx_begin_frame_queries(VkCommandBuffer cmd);

#endif
