// Private renderer state shared by the render/*.c files.
#ifndef DG_VK_INTERNAL_H
#define DG_VK_INTERNAL_H

#include "volk.h"
#include "vk_mem_alloc.h"
#include "render.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/cvar.h"

#define FRAMES_IN_FLIGHT 2
#define MAX_SWAP_IMAGES 8
#define MAX_MESHES 16384
#define MAX_TEXTURES 1024
#define MAX_DRAWS 16384
#define MAX_VM_DRAWS 512
#define MAX_DEFERRED 4096
#define MAX_UPLOADS 4096
#define DYN_BUFFER_SIZE (16u << 20)

#define R (g_engine.render)

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) log_fatal("Vulkan error %d at %s:%d (%s)", (int)_r, __FILE__, __LINE__, #x); } while (0)

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

typedef enum UploadKind { UPLOAD_BUFFER, UPLOAD_TEXTURE } UploadKind;
typedef struct Upload {
    UploadKind kind;
    GpuBuffer staging;
    VkBuffer dst;
    VkDeviceSize size;
    u32 texture;          // index into textures
    int levels_in_data;   // >1 for raw uploads with explicit mips
    size_t level_offsets[16];
    bool gen_mips;
} Upload;

typedef struct FrameResources {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore image_available;
    GpuBuffer ubo;
    VkDescriptorSet frame_set;
    GpuBuffer dyn;        // host-visible: lines, text, ui
    VkDeviceSize dyn_used;
    Deferred deferred[MAX_DEFERRED];
    int deferred_count;
} FrameResources;

// Must match shaders/common.glsl (std140).
typedef struct GpuPointLight { vec4 pos_radius; vec4 color_intensity; } GpuPointLight;
typedef struct FrameData {
    mat4 view_proj, view, proj, vm_proj, shadow_vp;
    vec4 camera_pos, sun_dir, sun_color, sky_ambient, ground_ambient, fog, params, screen;
    GpuPointLight lights[MAX_POINT_LIGHTS];
} FrameData;

typedef struct PushLit {
    mat4 model;
    vec4 color;
    vec4 params;
    u32 flags[4];
} PushLit;

typedef enum PipelineId {
    PIPE_LIT, PIPE_LIT_WIRE, PIPE_LIT_BLEND, PIPE_SHADOW, PIPE_LINES, PIPE_LINES_OVERLAY,
    PIPE_TEXT, PIPE_POST, PIPE_UI, PIPE_COUNT
} PipelineId;

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
    RenderTarget hdr, depth, shadow;
    u32 shadow_size;
    VkDescriptorSet hdr_set;   // set-1 style descriptor for post

    // descriptors
    VkDescriptorSetLayout frame_layout;   // set 0
    VkDescriptorSetLayout tex_layout;     // set 1
    VkDescriptorPool pool;
    VkPipelineLayout layout_lit;          // set0 + set1 + push lit
    VkPipelineLayout layout_post;         // set1 + push
    VkPipelineLayout layout_ui;           // set1 + push
    VkPipeline pipes[PIPE_COUNT];
    VkSampler sampler_linear;   // material textures (aniso)
    VkSampler sampler_clamp;    // post / ui
    VkSampler sampler_shadow;   // compare

    FrameResources frames[FRAMES_IN_FLIGHT];
    u32 frame_slot;
    u32 image_index;
    bool frame_active;

    GpuMesh meshes[MAX_MESHES];
    u32 mesh_free_hint;
    GpuTexture textures[MAX_TEXTURES];
    TextureId white_tex;
    TextureId text_atlas;

    Upload uploads[MAX_UPLOADS];
    int upload_count;

    // per-frame queued work
    DrawItem *draws;
    u32 draw_count;
    DrawItem *vm_draws;
    u32 vm_draw_count;
    GpuPointLight lights[MAX_POINT_LIGHTS];
    u32 light_count;
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
    mat4 view, proj, view_proj, vm_proj, shadow_vp;
    vec4 frustum[6];
    float render_scale_applied;
    char screenshot_path[512];
    RenderStats stats;

    // shader hot reload
    double next_shader_poll;
    i64 shader_src_mtime;

    // cvars
    Cvar *r_vsync, *r_wireframe, *r_render_scale, *r_shadow_quality, *r_aniso, *r_texture_quality;
    Cvar *r_aa, *r_brightness, *r_gamma, *r_grid, *r_vignette, *r_shader_hotreload, *r_culling;
    Cvar *r_vm_fov;
    u32 aniso_mod, texq_mod;
} Renderer;

// vk_device.c
bool vkd_create_device(struct SDL_Window *window);
void vkd_destroy_device(void);
bool vkd_create_swapchain(void);
void vkd_destroy_swapchain(void);
void vkd_create_targets(void);
void vkd_destroy_targets(void);
void vkd_create_shadow_map(u32 size);

// vk_resources.c
GpuBuffer vkr_buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
void vkr_buffer_destroy_now(GpuBuffer *b);
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
VkDescriptorSet vkr_alloc_tex_set(VkImageView view, VkSampler sampler);
void vkr_write_tex_set(VkDescriptorSet set, VkImageView view, VkSampler sampler);
void vkr_rewrite_all_texture_sets(void);
void vkr_destroy_all_resources(void);

// vk_pipelines.c
bool vkp_create_layouts(void);
bool vkp_create_pipelines(void);
void vkp_destroy_pipelines(void);
void vkp_destroy_layouts(void);

#endif
