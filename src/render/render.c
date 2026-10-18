// Frame orchestration: particle compute -> shadow pass -> frame graph (G-buffer prepass,
// screen-space lighting, lit scene, fog, transparency, viewmodel, post stack, final + UI) -> present.
#include "render/phone_view.h"
#include "render/shadow_csm.h"
#include "vk_internal.h"
#include "post/post.h"
#include "sky/sky.h"
#include "graph/fg.h"
#include "skeletal_mesh.h"
#include "lod_fade.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include "stb_image_write.h"

static void on_render_cvar_changed(Cvar *cv) { (void)cv; }

// Stretched resolution simulator (r_stretch): the competitive "4:3 stretched" look. The 3D view is
// projected as if the screen were k times narrower (same vertical FOV) and fills the full width, so
// everything is k times wider. 0.25 ~ 4:3 on 16:9 (k 1.33), 1 = k 2.5. The UI is not stretched.
#define STRETCH_MAX 2.5f
float render_stretch_factor(void) { return 1.0f + clampf(R->r_stretch ? R->r_stretch->f : 0.0f, 0.0f, 1.0f) * (STRETCH_MAX - 1.0f); }
static float view_aspect(void) {
    return (float)R->swap_extent.width / (float)DG_MAX(1u, R->swap_extent.height) / render_stretch_factor();
}
// Whole-frame GPU time (Engine Fidelity benchmark / overlay): a timestamp pair per frame slot.
static VkQueryPool g_frame_qp;
static bool g_frame_qp_written[FRAMES_IN_FLIGHT];
static float g_gpu_frame_ms;
static bool (*g_cam_override)(RenderCamera *cam);
void render_set_camera_override(bool (*fn)(RenderCamera *cam)) { g_cam_override = fn; }
static size_t obj_slot_size(void);

void render_register_cvars(void) {
    Renderer *r = R;
    r->r_vsync = cvar_reg_bool("r_vsync", true, CVAR_ARCHIVE, "vertical sync");
    r->r_wireframe = cvar_reg_bool("r_wireframe", false, 0, "wireframe rendering");
    r->r_render_scale = cvar_reg_float("r_render_scale", 1.0f, 0.25f, 2.0f, CVAR_ARCHIVE, "3D resolution scale");
    r->r_aniso = cvar_reg_int("r_aniso", 8, 1, 16, CVAR_ARCHIVE, "anisotropic filtering level");
    r->r_texture_quality = cvar_reg_int("r_texture_quality", 2, 0, 2, CVAR_ARCHIVE, "0 low, 1 medium, 2 high");
    r->r_aa = cvar_reg_int("r_aa", 3, 0, 3, CVAR_ARCHIVE, "anti-aliasing: 0 none, 1 FXAA, 2 SMAA 1x, 3 TAA");
    r->r_brightness = cvar_reg_float("r_brightness", 1.0f, 0.3f, 3.0f, CVAR_ARCHIVE, "display brightness (after the tonemapper)");
    r->r_gamma = cvar_reg_float("r_gamma", 1.0f, 0.5f, 2.0f, CVAR_ARCHIVE, "gamma adjustment");
    r->r_grid = cvar_reg_bool("r_grid", true, CVAR_ARCHIVE, "draw unit grid on dev geometry");
    r->r_vignette = cvar_reg_float("r_vignette", 0.25f, 0.0f, 1.0f, CVAR_ARCHIVE, "vignette strength");
    r->r_shader_hotreload = cvar_reg_bool("r_shader_hotreload", true, 0, "watch shader sources and reload on change");
    r->r_culling = cvar_reg_bool("r_culling", true, 0, "frustum culling");
    r->r_vm_fov = cvar_reg_float("r_viewmodel_fov", 0.0f, 0.0f, 120.0f, CVAR_ARCHIVE, "viewmodel FOV (0 = same as world, keeps IK hands aligned)");
    r->r_cpu_skinning = cvar_reg_bool("r_cpu_skinning", false, 0, "skin characters on the CPU (fallback / debugging)");
    r->r_bloom = cvar_reg_bool("r_bloom", true, CVAR_ARCHIVE, "bloom (HDR mip chain)");
    r->r_hdr_output = cvar_reg_bool("r_hdr_output", false, CVAR_ARCHIVE, "HDR10 output (PQ) when the display supports it");
    r->r_bloom_intensity = cvar_reg_float("r_bloom_intensity", 0.05f, 0.0f, 0.5f, CVAR_ARCHIVE, "bloom strength (energy conserving mix)");
    // Unity-style shadow quality (docs/SHADOWS_UNITY_MODEL.md; Engine Fidelity sets defaults / bounds).
    QualityShadowSettings dq;
    shs_quality_default(&dq);
    r->r_shadows = cvar_reg_int("r_shadows", dq.mode, 0, 2, CVAR_ARCHIVE, "Shadows: 0 Disable, 1 Hard Only (Soft lights render Hard), 2 Hard and Soft");
    r->r_shadow_resolution = cvar_reg_int("r_shadow_resolution", dq.resolution, 0, 3, CVAR_ARCHIVE,
                                          "Shadow Resolution for lights on 'Use Quality Settings': 0 Low, 1 Medium, 2 High, 3 Very High (sun cascades 512 .. 4096, spot / point 256 .. 2048)");
    r->r_shadow_projection = cvar_reg_int("r_shadow_projection", dq.projection, 0, 1, CVAR_ARCHIVE, "Shadow Projection: 0 Close Fit (denser, swims), 1 Stable Fit (sphere fit + texel snapping)");
    r->r_shadow_cascades = cvar_reg_int("r_shadow_cascades", dq.cascades, 1, 4, CVAR_ARCHIVE, "Shadow Cascades: 1 No Cascades, 2 Two, 4 Four (3 = 4)");
    r->r_shadow_max_distance = cvar_reg_float("r_shadow_max_distance", dq.distance, 10.0f, 1000.0f, CVAR_ARCHIVE, "Shadow Distance (m): the last cascade ends here, fading over the cascade border");
    r->r_soft_shadow_quality = cvar_reg_int("r_soft_shadow_quality", dq.soft_quality, 0, 3, CVAR_ARCHIVE,
                                            "Soft Shadow Quality: 0 Low (tent 3x3), 1 Medium (5x5), 2 High (7x7), 3 Ultra (PCSS, contact hardening; sun and viewmodel)");
    r->r_shadow_split2 = cvar_reg_float("r_shadow_split2", dq.split2 * 100.0f, 1.0f, 99.0f, CVAR_ARCHIVE, "two-cascade split (% of the shadow distance; Unity default 33.3)");
    r->r_shadow_split4[0] = cvar_reg_float("r_shadow_split4_1", dq.split4[0] * 100.0f, 1.0f, 97.0f, CVAR_ARCHIVE, "four-cascade split 1 (% of the shadow distance; Unity default 6.7)");
    r->r_shadow_split4[1] = cvar_reg_float("r_shadow_split4_2", dq.split4[1] * 100.0f, 2.0f, 98.0f, CVAR_ARCHIVE, "four-cascade split 2 (%; Unity default 20)");
    r->r_shadow_split4[2] = cvar_reg_float("r_shadow_split4_3", dq.split4[2] * 100.0f, 3.0f, 99.0f, CVAR_ARCHIVE, "four-cascade split 3 (%; Unity default 46.7)");
    r->r_shadow_border = cvar_reg_float("r_shadow_cascade_border", dq.border * 100.0f, 0.0f, 50.0f, CVAR_ARCHIVE,
                                        "Cascade Border: blend band at the end of each cascade, and the shadow distance fade (% of the cascade)");
    r->r_shadow_near_offset = cvar_reg_float("r_shadow_near_offset", dq.near_plane_offset, 2.0f, 5000.0f, CVAR_ARCHIVE,
                                             "Shadow Near Plane Offset (m): how far each cascade's light box reaches towards the light beyond its receivers (casters outside the view)");
    r->r_shadowmask_mode = cvar_reg_int("r_shadowmask_mode", dq.shadowmask, 0, 1, CVAR_ARCHIVE, "Shadowmask Mode (stub, no baked data yet): 0 Shadowmask, 1 Distance Shadowmask");
    r->r_shadowmask_distance = cvar_reg_float("r_shadowmask_distance", dq.shadowmask_distance, 0.0f, 2000.0f, CVAR_ARCHIVE, "Distance Shadowmask cut-over (m; stub)");
    r->r_shadow_debug = cvar_reg_int("r_shadow_debug", 0, 0, 8, 0,
                                     "shadow views: 1 readout (sky, cascades, bias, filter, jitter), 2 penumbra width, 3 blocker distance, 4 cascade / mode, 5 cloud shadow, 6 texel size, 7 viewmodel tile coverage, 8 fitting spheres + light frustums");
    r->r_vm_shadow = cvar_reg_bool("r_vm_shadow", true, CVAR_ARCHIVE, "first-person rig (arms, held item) in its own high-resolution shadow tile (docs/SHADOWS.md)");
    r->r_stretch = cvar_reg_float("r_stretch", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                                  "stretched resolution simulator: 0 normal .. 1 extreme (the 3D view squeezed into a narrower aspect, stretched to the screen)");
    r->r_shadow_min_radius = cvar_reg_float("r_shadow_min_radius", 0.0f, 0.0f, 4.0f, CVAR_ARCHIVE, "props smaller than this radius (m) cast no sun shadow");
    r->r_vsync->on_change = on_render_cvar_changed;
}

// Unity-style quality knobs from the cvars (docs/SHADOWS_UNITY_MODEL.md), read at the frame
// boundary: the shadow maps are sized from them before compute_matrices builds the frame.
static void shadow_quality_read(QualityShadowSettings *q) {
    shs_quality_default(q);
    q->mode = (ShadowsMode)R->r_shadows->i;
    q->resolution = (ShadowRes)R->r_shadow_resolution->i;
    q->projection = (ShadowProjection)R->r_shadow_projection->i;
    q->distance = R->r_shadow_max_distance->f;
    q->cascades = R->r_shadow_cascades->i;
    q->split2 = R->r_shadow_split2->f * 0.01f;
    for (int i = 0; i < 3; i++) q->split4[i] = R->r_shadow_split4[i]->f * 0.01f;
    q->border = R->r_shadow_border->f * 0.01f;
    q->near_plane_offset = R->r_shadow_near_offset->f;
    q->shadowmask = (ShadowmaskMode)R->r_shadowmask_mode->i;
    q->shadowmask_distance = R->r_shadowmask_distance->f;
    q->soft_quality = (SoftQuality)R->r_soft_shadow_quality->i;
    shs_quality_sanitize(q);
}

// The sun's cascade tile (its Resolution or the quality's), the atlas of 2 x 2 tiles within the
// device limit; 16 texels (unused) when the sun casts nothing.
static bool sun_casts(void) { return shs_effective_type(R->sun_shadow.type, R->shq.mode) != SHADOW_NONE; }
static u32 shadow_atlas_size(void) {
    if (!sun_casts()) return 16;
    u32 s = 2u * shs_light_texels(&R->sun_shadow, &R->shq, SHADOW_LIGHT_DIRECTIONAL), lim = R->gpu_props.limits.maxImageDimension2D;
    while (lim && s > lim) s /= 2u;
    return s;
}
static u32 vm_shadow_size(void) { return sun_casts() ? csm_vm_texels(shadow_atlas_size() / 2u) : 8u; }
static u32 moon_shadow_size(void) { return sun_casts() ? DG_MIN(shadow_atlas_size() / 2u, 2048u) : 16u; }

void render_set_sun_shadow(const LightShadowSettings *s) {
    R->sun_shadow = *s;
    shs_light_sanitize(&R->sun_shadow);
}
void render_get_sun_shadow(LightShadowSettings *out) { *out = R->sun_shadow; }

static void create_frames(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        FrameResources *f = &R->frames[i];
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = R->queue_family;
        VK_CHECK(vkCreateCommandPool(R->device, &pci, NULL, &f->pool));
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = f->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(R->device, &cai, &f->cmd));
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(R->device, &fci, NULL, &f->fence));
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(R->device, &sci, NULL, &f->image_available));
        f->ubo = vkr_buffer_create(sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f->ubo_pip = vkr_buffer_create(sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        for (int k = 0; k < 2; k++) f->ubo_phone[k] = vkr_buffer_create(sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f->dyn = vkr_buffer_create(DYN_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        f->bones = vkr_buffer_create(sizeof(mat4) * MAX_SKIN_BONES * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);   // + last frame's
        f->instances = vkr_buffer_create(sizeof(GpuSkinInstance) * MAX_SKIN_DRAWS * 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);   // main + shadow / vm + phone
        f->prev_models = vkr_buffer_create(sizeof(mat4) * (MAX_DRAWS + MAX_VM_DRAWS), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = R->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &R->frame_layout;
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &f->frame_set));
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &f->frame_set_pip));
        for (int k = 0; k < 2; k++) VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &f->frame_set_phone[k]));
        // The PiP / phone sets: their own frame UBO, the same skinning / previous-model buffers.
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(f, sets);
        GpuBuffer *ubos[FRAME_SET_COUNT] = { &f->ubo, &f->ubo_pip, &f->ubo_phone[0], &f->ubo_phone[1] };
        for (int s = 0; s < FRAME_SET_COUNT; s++) {
            VkDescriptorBufferInfo bi = { ubos[s]->buffer, 0, sizeof(FrameData) };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = sets[s];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
            VkDescriptorBufferInfo sb[3] = { { f->bones.buffer, 0, VK_WHOLE_SIZE }, { f->instances.buffer, 0, VK_WHOLE_SIZE },
                                             { f->prev_models.buffer, 0, VK_WHOLE_SIZE } };
            VkWriteDescriptorSet ws[3];
            for (int k = 0; k < 3; k++) {
                ws[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                ws[k].dstSet = sets[s];
                ws[k].dstBinding = (u32)(2 + k);
                ws[k].descriptorCount = 1;
                ws[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                ws[k].pBufferInfo = &sb[k];
            }
            vkUpdateDescriptorSets(R->device, 3, ws, 0, NULL);
        }
    }
}

// Frame sets: the sky module's persistent environment / SH / cloud shadow (kept in GENERAL).
static void write_sky_bindings(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int s = 0; s < FRAME_SET_COUNT; s++) {
            if (!sets[s]) continue;
            VkDescriptorImageInfo env = { R->sampler_clamp, sky_env_view(), VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo cs = env;
            cs.imageView = sky_cloud_shadow_view();
            VkDescriptorBufferInfo sh = { sky_sh_buffer(), 0, sky_sh_size() };
            VkWriteDescriptorSet w[3];
            for (int k = 0; k < 3; k++) {
                w[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[k].dstSet = sets[s];
                w[k].dstBinding = (u32)(5 + k);
                w[k].descriptorCount = 1;
            }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].pImageInfo = &env;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[1].pBufferInfo = &sh;
            w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[2].pImageInfo = &cs;
            vkUpdateDescriptorSets(R->device, 3, w, 0, NULL);
        }
    }
}

static void destroy_frames(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        FrameResources *f = &R->frames[i];
        vkr_flush_deferred(f);
        vkr_buffer_destroy_now(&f->ubo);
        vkr_buffer_destroy_now(&f->ubo_pip);
        for (int k = 0; k < 2; k++) vkr_buffer_destroy_now(&f->ubo_phone[k]);
        vkr_buffer_destroy_now(&f->dyn);
        vkr_buffer_destroy_now(&f->bones);
        vkr_buffer_destroy_now(&f->instances);
        vkr_buffer_destroy_now(&f->prev_models);
        vkDestroySemaphore(R->device, f->image_available, NULL);
        vkDestroyFence(R->device, f->fence, NULL);
        vkDestroyCommandPool(R->device, f->pool, NULL);
    }
}

bool render_init(SDL_Window *window) {
    g_engine.render = (Renderer *)calloc(1, sizeof(Renderer));
    render_register_cvars();
    if (!vkd_create_device(window)) return false;
    vkr_create_samplers();
    vkp_create_layouts();
    create_frames();
    if (!vkd_create_swapchain()) { log_error("render: initial swapchain creation failed"); return false; }
    R->target_scale = 1.0f;
    vkd_create_targets();
    shs_light_default(&R->sun_shadow, SHADOW_LIGHT_DIRECTIONAL);
    shadow_quality_read(&R->shq);
    vkd_create_shadow_map(shadow_atlas_size());
    vkd_create_vm_shadow_map(vm_shadow_size());
    vkd_create_moon_shadow_map(moon_shadow_size());
    vkls_init();
    if (!vkp_create_pipelines()) { log_error("render: pipeline creation failed"); return false; }
    if (!vkx_init()) log_warn("render: VFX pipelines unavailable (particles disabled)");
    fg_init();
    {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * FRAMES_IN_FLIGHT;
        VK_CHECK(vkCreateQueryPool(R->device, &qi, NULL, &g_frame_qp));
    }
    vkg_init();
    lod_init();   // LOD ranges / cross-fades (docs/LOD.md): after r_vgeo
    if (!post_init()) { log_error("render: post-processing stack unavailable"); return false; }
    vkw_init();   // after the post stack (its descriptor layout, samplers)
    sky_init();
    write_sky_bindings();
    vks_init();
    vkgr_init();
    vkt_init();

    R->draws = (DrawItem *)malloc(sizeof(DrawItem) * MAX_DRAWS);
    R->vm_draws = (DrawItem *)malloc(sizeof(DrawItem) * MAX_VM_DRAWS);
    R->skin_draws = (SkinnedDraw *)malloc(sizeof(SkinnedDraw) * MAX_SKIN_DRAWS);
    R->skin_bones = (mat4 *)malloc(sizeof(mat4) * MAX_SKIN_BONES);
    R->prev_bones = (mat4 *)malloc(sizeof(mat4) * MAX_SKIN_BONES);
    R->prev_models_cpu = (mat4 *)malloc(sizeof(mat4) * (MAX_DRAWS + MAX_VM_DRAWS));
    R->obj_slots = (struct ObjSlot *)calloc(OBJ_SLOTS, obj_slot_size());
    R->line_cap = 262144;
    R->line_cpu[0] = (LineVertex *)malloc(sizeof(LineVertex) * R->line_cap);
    R->line_cpu[1] = (LineVertex *)malloc(sizeof(LineVertex) * R->line_cap);
    R->text_cap = 196608;
    R->text_cpu = (TextVertex *)malloc(sizeof(TextVertex) * R->text_cap);
    R->ui_vcap = 262144; R->ui_icap = 524288; R->ui_ccap = 8192;
    R->ui_verts = (UiVertex *)malloc(sizeof(UiVertex) * R->ui_vcap);
    R->ui_indices = (u32 *)malloc(sizeof(u32) * R->ui_icap);
    R->ui_cmds = (UiDrawCmd *)malloc(sizeof(UiDrawCmd) * R->ui_ccap);

    u32 white = 0xFFFFFFFFu;
    R->white_tex = render_texture_create(1, 1, &white, false, false);
    u32 flat = 0xFFFF8080u;   // tangent-space (0, 0, 1)
    R->flat_normal_tex = render_texture_create(1, 1, &flat, false, false);
    R->text_atlas = R->white_tex;

    RenderEnvironment env = { 0 };
    env.sun_dir = v3_norm(v3(0.4f, 1.0f, 0.3f));
    env.sun_color = v3(1, 0.95f, 0.9f);
    env.sun_intensity = 2.5f;
    env.sky_ambient = v3(0.5f, 0.55f, 0.65f);
    env.ground_ambient = v3(0.25f, 0.22f, 0.2f);
    env.ambient_intensity = 0.6f;
    env.fog_color = v3(0.5f, 0.55f, 0.6f);
    env.shadow_extent = 40.0f;
    R->env = env;
    R->cam.fov_deg = 75.0f;
    R->cam.znear = 0.05f;
    R->cam.zfar = 1000.0f;
    R->aniso_mod = R->r_aniso->modified;
    R->texq_mod = R->r_texture_quality->modified;
    return true;
}

void render_wait_idle(void) {
    if (R && R->device) vkDeviceWaitIdle(R->device);
}

void render_shutdown(void) {
    if (!R) return;
    if (R->device) {
        vkDeviceWaitIdle(R->device);
        if (g_frame_qp) vkDestroyQueryPool(R->device, g_frame_qp, NULL);
        g_frame_qp = VK_NULL_HANDLE;
        sky_shutdown();
        post_shutdown();
        vkg_shutdown();
        fg_shutdown();
        vkx_shutdown();
        vkw_shutdown();
        render_ui_shutdown();
        phone_view_shutdown();
        vkr_target_destroy(&R->occl);
        vkls_shutdown();
        vks_shutdown();
        vkt_shutdown();
        vkp_destroy_pipelines();
        vkr_destroy_all_resources();
        vkr_target_destroy(&R->shadow);
        vkr_target_destroy(&R->vm_shadow);
        vkr_target_destroy(&R->moon_shadow);
        vkd_destroy_targets();
        vkd_destroy_swapchain();
        destroy_frames();
        vkp_destroy_layouts();
        vkr_destroy_samplers();
    }
    vkd_destroy_device();
    free(R->draws); free(R->vm_draws);
    free(R->skin_draws); free(R->skin_bones); free(R->prev_bones); free(R->prev_models_cpu); free(R->obj_slots);
    free(R->line_cpu[0]); free(R->line_cpu[1]); free(R->text_cpu);
    free(R->ui_verts); free(R->ui_indices); free(R->ui_cmds);
    free(R);
    g_engine.render = NULL;
}

static void targets_changed(void) {
    fg_targets_changed();
    post_targets_changed();
    post_create_pipelines();   // the final pass depends on the swapchain format
    R->camera_cut = true;
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(R->device);
    vkd_destroy_targets();
    if (!vkd_create_swapchain()) { R->swap_dirty = true; return; }
    vkd_create_targets();
    vkp_create_pipelines(); // swapchain format can change
    targets_changed();
}

// Render resolution / output: r_render_scale (supersampling / plain scaling) times the
// upscaler's render scale (TAA-U, FSR) when one is active.
static float desired_target_scale(void) {
    return clampf(R->r_render_scale->f * post_render_scale(), 0.25f, 2.0f);
}

static void apply_setting_changes(void) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_engine.window, &w, &h);
    if (w <= 0 || h <= 0) return;
    if ((u32)w != R->swap_extent.width || (u32)h != R->swap_extent.height) R->swap_dirty = true;
    if ((R->r_vsync->i != 0) != R->vsync_applied) R->swap_dirty = true;
    if (R->r_hdr_output->i != R->hdr_output_applied) R->swap_dirty = true;
    if (R->swap_dirty) recreate_swapchain();
    if (fabsf(desired_target_scale() - R->render_scale_applied) > 1e-4f) {
        vkDeviceWaitIdle(R->device);
        vkd_destroy_targets();
        R->target_scale = desired_target_scale();
        vkd_create_targets();
        targets_changed();
    }
    // Shadow resolution (Engine Fidelity): on the frame boundary, before compute_matrices; the new
    // maps are rendered this frame before any pass samples them (no stale texel grid, no unwritten map).
    shadow_quality_read(&R->shq);
    if (shadow_atlas_size() != R->shadow_size) {
        vkDeviceWaitIdle(R->device);
        vkd_create_shadow_map(shadow_atlas_size());
    }
    if (vm_shadow_size() != R->vm_shadow_size) {
        vkDeviceWaitIdle(R->device);
        vkd_create_vm_shadow_map(vm_shadow_size());
    }
    if (moon_shadow_size() != R->moon_shadow_size) {
        vkDeviceWaitIdle(R->device);
        vkd_create_moon_shadow_map(moon_shadow_size());
    }
    if (R->r_aniso->modified != R->aniso_mod || R->r_texture_quality->modified != R->texq_mod) {
        R->aniso_mod = R->r_aniso->modified;
        R->texq_mod = R->r_texture_quality->modified;
        vkDeviceWaitIdle(R->device);
        vkr_textures_quality_changed();   // world textures drop levels (or want a reload) first: new views
        vkr_recreate_linear_sampler();
    } else if (R->texq_retry) {
        vkDeviceWaitIdle(R->device);
        vkr_textures_quality_changed();
    }
}

bool render_begin_frame(void) {
    R->frame_active = false;
    R->draw_count = R->vm_draw_count = R->light_count = 0;
    R->spot_count = 0;
    R->skin_draw_count = R->skin_bone_count = 0;
    R->line_count[0] = R->line_count[1] = 0;
    R->text_count = 0;
    R->ui_vcount = R->ui_icount = R->ui_ccount = 0;
    memset(&R->sight, 0, sizeof(R->sight));   // the game sets this frame's sight (render_set_sight)
    R->last_stats = R->stats;
    R->stats.draw_calls = R->stats.triangles = R->stats.culled = 0;

    apply_setting_changes();
    if (R->swap_dirty || !R->swapchain) return false;

    FrameResources *f = &R->frames[R->frame_slot];
    VK_CHECK(vkWaitForFences(R->device, 1, &f->fence, VK_TRUE, UINT64_MAX));
    vkr_flush_deferred(f);
    f->dyn_used = 0;
    phone_view_frame_begin(R->frame_slot);   // this slot's phone camera copies are complete

    VkResult res = vkAcquireNextImageKHR(R->device, R->swapchain, UINT64_MAX, f->image_available, VK_NULL_HANDLE, &R->image_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) { R->swap_dirty = true; return false; }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) { log_error("acquire failed %d", (int)res); return false; }
    VK_CHECK(vkResetFences(R->device, 1, &f->fence));
    R->frame_active = true;
    return true;
}

void render_set_camera(const RenderCamera *cam) {
    R->cam = *cam;
    vkg_bench_camera(&R->cam);   // vgeo_bench flythrough
    vkg_frame_update();          // grass fades / shadows follow the fidelity level (outside the frame's recording)
    if (g_cam_override) g_cam_override(&R->cam);   // bench_fidelity flythrough
    cam = &R->cam;
    float aspect = view_aspect();
    R->view = mat4_fps_view(cam->pos, cam->yaw, cam->pitch, cam->roll);
    R->proj = mat4_perspective(cam->fov_deg * DG_DEG2RAD, aspect, cam->znear, cam->zfar);
    R->view_proj = mat4_mul(R->proj, R->view);
}
void render_set_environment(const RenderEnvironment *env) { R->env = *env; }

void render_draw(const DrawItem *item) {
    if (!item->mesh || R->draw_count >= MAX_DRAWS) return;
    R->draws[R->draw_count++] = *item;
}

void render_draw_skinned(const SkinnedDraw *d) {
    if (!d->mesh || !d->palette || !d->bone_count || R->skin_draw_count >= MAX_SKIN_DRAWS) return;
    if (R->skin_bone_count + d->bone_count > MAX_SKIN_BONES) return;
    if (!R->meshes[d->mesh].used || !R->meshes[d->mesh].skinned) return;
    SkinnedDraw *s = &R->skin_draws[R->skin_draw_count++];
    *s = *d;
    if (!s->object_id && (s->flags & MAT_VIEWMODEL)) s->object_id = 0x40000000u | R->skin_draw_count;
    memcpy(R->skin_bones + R->skin_bone_count, d->palette, sizeof(mat4) * d->bone_count);
    // The palette pointer is replaced by its offset into this frame's bone buffer.
    s->palette = NULL;
    s->bone_count = R->skin_bone_count | ((u32)d->bone_count << 20);
    R->skin_bone_count += d->bone_count;
}

void render_draw_viewmodel(const DrawItem *item) {
    if (!item->mesh || R->vm_draw_count >= MAX_VM_DRAWS) return;
    DrawItem *d = &R->vm_draws[R->vm_draw_count];
    *d = *item;
    d->mat.flags |= MAT_VIEWMODEL;
    // Viewmodel parts move with the camera: without an id, the draw order identifies them (their
    // velocity must come from their own motion, not from the camera rotation).
    if (!d->object_id) d->object_id = 0x80000000u | (R->vm_draw_count + 1);
    R->vm_draw_count++;
}

void render_add_light(const PointLight *l) {
    if (R->light_count >= MAX_POINT_LIGHTS) return;
    R->light_flags[R->light_count] = l->flags;
    R->light_set_given[R->light_count] = l->shadow != NULL;
    if (l->shadow) R->light_set[R->light_count] = *l->shadow;
    else shs_light_default(&R->light_set[R->light_count], SHADOW_LIGHT_POINT);
    GpuPointLight *g = &R->lights[R->light_count++];
    g->pos_radius = v4(l->pos.x, l->pos.y, l->pos.z, l->radius);
    g->color_intensity = v4(l->color.x, l->color.y, l->color.z, l->intensity);
}

void render_add_spot(const SpotLight *l) {
    if (R->spot_count >= MAX_SPOT_LIGHTS) return;
    R->spot_flags[R->spot_count] = l->flags;
    R->spot_set_given[R->spot_count] = l->shadow != NULL;
    if (l->shadow) R->spot_set[R->spot_count] = *l->shadow;
    else shs_light_default(&R->spot_set[R->spot_count], SHADOW_LIGHT_SPOT);
    GpuSpotLight *g = &R->spots[R->spot_count++];
    vec3 d = v3_norm(l->dir);
    float outer = clampf(l->outer_deg, 1.0f, 85.0f) * DG_DEG2RAD, inner = clampf(l->inner_deg, 0.0f, l->outer_deg) * DG_DEG2RAD;
    vec3 up = fabsf(d.y) > 0.95f ? v3(1, 0, 0) : v3(0, 1, 0);
    g->vp = mat4_mul(mat4_perspective(2.0f * outer, 1.0f, 0.05f, fmaxf(l->range, 1.0f)), mat4_look_at(l->pos, v3_add(l->pos, d), up));
    g->pos_range = v4(l->pos.x, l->pos.y, l->pos.z, l->range);
    g->dir_cos = v4(d.x, d.y, d.z, cosf(outer));
    g->color_intensity = v4(l->color.x, l->color.y, l->color.z, l->intensity);
    g->params = v4(cosf(inner), 0, 0, 0);
}

void render_set_spot_cookie(TextureId t) { R->spot_cookie = t; }

static RenderLensDrop g_lens_drops[RENDER_LENS_DROPS];

void render_set_weather(const RenderWeather *w) {
    R->weather = *w;
    int n = DG_CLAMP(w->drop_count, 0, RENDER_LENS_DROPS);
    if (w->drops && n) memcpy(g_lens_drops, w->drops, sizeof(RenderLensDrop) * (size_t)n);
    R->weather.drops = g_lens_drops;
    R->weather.drop_count = w->drops ? n : 0;
}

const RenderWeather *render_weather_state(void) { return &R->weather; }

// Frame set bindings 9 (rain occlusion depth) and 10 (puddle mask); rewritten when they change.
static void write_weather_bindings(void) {
    if (!R->occl.image) {
        u32 n = 512;
        R->occl = vkr_target_create(n, n, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    TextureId pm = R->weather.puddle_mask && R->textures[R->weather.puddle_mask].used && R->textures[R->weather.puddle_mask].ready ? R->weather.puddle_mask : R->white_tex;
    if (R->weather_bindings && pm == R->puddle_bound) return;
    if (R->weather_bindings) vkDeviceWaitIdle(R->device);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int s = 0; s < FRAME_SET_COUNT; s++) {
            if (!sets[s]) continue;
            VkDescriptorImageInfo ii[2] = { { R->sampler_clamp, R->occl.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_clamp, R->textures[pm].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet w[2];
            for (int k = 0; k < 2; k++) {
                w[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[k].dstSet = sets[s];
                w[k].dstBinding = (u32)(9 + k);
                w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[k].pImageInfo = &ii[k];
            }
            vkUpdateDescriptorSets(R->device, 2, w, 0, NULL);
        }
    }
    R->puddle_bound = pm;
    R->weather_bindings = true;
}

// Frame set binding 8: the spot cookie (white until one is set / uploaded).
static void write_cookie_binding(void) {
    TextureId t = R->spot_cookie && R->textures[R->spot_cookie].used && R->textures[R->spot_cookie].ready ? R->spot_cookie : R->white_tex;
    if (t == R->spot_cookie_bound || !t) return;
    vkDeviceWaitIdle(R->device);   // rare: the cookie changes once
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int s = 0; s < FRAME_SET_COUNT; s++) {
            if (!sets[s]) continue;
            VkDescriptorImageInfo ii = { R->sampler_clamp, R->textures[t].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = sets[s];
            w.dstBinding = 8;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ii;
            vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
        }
    }
    R->spot_cookie_bound = t;
}

LineVertex *render_lines_alloc(u32 count, bool depth_test) {
    int k = depth_test ? 0 : 1;
    if (R->line_count[k] + count > R->line_cap) return NULL;
    LineVertex *v = R->line_cpu[k] + R->line_count[k];
    R->line_count[k] += count;
    return v;
}

TextVertex *render_text_alloc(u32 count) {
    if (R->text_count + count > R->text_cap) {
        static int warned;
        if (!warned++) log_warn("render: 3D text buffer full (%u verts)", R->text_cap);
        return NULL;
    }
    TextVertex *v = R->text_cpu + R->text_count;
    R->text_count += count;
    return v;
}

void render_set_text_atlas(TextureId atlas) { R->text_atlas = atlas ? atlas : R->white_tex; }

void render_ui(const UiVertex *verts, u32 vcount, const u32 *indices, u32 icount, const UiDrawCmd *cmds, u32 ccount) {
    if (R->ui_vcount + vcount > R->ui_vcap || R->ui_icount + icount > R->ui_icap || R->ui_ccount + ccount > R->ui_ccap) return;
    u32 vbase = R->ui_vcount, ibase = R->ui_icount;
    memcpy(R->ui_verts + vbase, verts, sizeof(UiVertex) * vcount);
    for (u32 i = 0; i < icount; i++) R->ui_indices[ibase + i] = indices[i] + vbase;
    for (u32 i = 0; i < ccount; i++) {
        UiDrawCmd c = cmds[i];
        c.index_offset += ibase;
        R->ui_cmds[R->ui_ccount++] = c;
    }
    R->ui_vcount += vcount;
    R->ui_icount += icount;
}

// ------------------------------------------------------------------ helpers

static void extract_frustum(mat4 m, vec4 out[6]) {
    // Gribb/Hartmann on column-major view_proj (Vulkan depth 0..1).
    float *a = m.m;
    vec4 row0 = v4(a[0], a[4], a[8], a[12]);
    vec4 row1 = v4(a[1], a[5], a[9], a[13]);
    vec4 row2 = v4(a[2], a[6], a[10], a[14]);
    vec4 row3 = v4(a[3], a[7], a[11], a[15]);
    out[0] = v4(row3.x + row0.x, row3.y + row0.y, row3.z + row0.z, row3.w + row0.w);
    out[1] = v4(row3.x - row0.x, row3.y - row0.y, row3.z - row0.z, row3.w - row0.w);
    out[2] = v4(row3.x + row1.x, row3.y + row1.y, row3.z + row1.z, row3.w + row1.w);
    out[3] = v4(row3.x - row1.x, row3.y - row1.y, row3.z - row1.z, row3.w - row1.w);
    out[4] = row2;
    out[5] = v4(row3.x - row2.x, row3.y - row2.y, row3.z - row2.z, row3.w - row2.w);
    for (int i = 0; i < 6; i++) {
        float l = sqrtf(out[i].x * out[i].x + out[i].y * out[i].y + out[i].z * out[i].z);
        out[i].x /= l; out[i].y /= l; out[i].z /= l; out[i].w /= l;
    }
}

static bool sphere_visible(const vec4 planes[6], vec3 c, float r) {
    for (int i = 0; i < 6; i++)
        if (planes[i].x * c.x + planes[i].y * c.y + planes[i].z * c.z + planes[i].w < -r) return false;
    return true;
}

static void item_bounds(const DrawItem *d, vec3 *center, float *radius) {
    AABB b = R->meshes[d->mesh].bounds;
    vec3 lc = v3_scale(v3_add(b.min, b.max), 0.5f);
    vec3 he = v3_scale(v3_sub(b.max, b.min), 0.5f);
    const float *m = d->model.m;
    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    float sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    *center = mat4_mul_point(d->model, lc);
    *radius = v3_len(he) * fmaxf(sx, fmaxf(sy, sz));
}

typedef struct SortKey { u64 key; u32 index; } SortKey;
static int cmp_sortkey(const void *a, const void *b) {
    u64 ka = ((const SortKey *)a)->key, kb = ((const SortKey *)b)->key;
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

static void set_viewport(VkCommandBuffer cmd, u32 w, u32 h) {
    VkViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static VkDeviceSize dyn_push(FrameResources *f, const void *data, VkDeviceSize size) {
    VkDeviceSize off = (f->dyn_used + 15) & ~(VkDeviceSize)15;
    if (off + size > f->dyn.size) return (VkDeviceSize)-1;
    memcpy((u8 *)f->dyn.mapped + off, data, (size_t)size);
    f->dyn_used = off + size;
    return off;
}

static bool g_froxel_fog_draws;   // transparent pass: blended lit draws apply the froxel fog

static void draw_precip(VkCommandBuffer cmd, VkDescriptorSet frame_set);
static u32 g_shadow_cascade;   // shadow pass: the cascade being drawn (push constants flags.w)
static float g_shadow_min_radius;   // sun cascades: smaller casters skipped (r_shadow_min_radius)
static bool g_shadow_cull;          // sun / viewmodel tiles: casters outside the tile's light frustum skipped
static vec4 g_shadow_planes[6];     // that frustum (the box reaches CSM_REACH towards the light)
static bool g_shadow_vm_pass;       // the viewmodel tile is being drawn (CPU-skinned viewmodel draws take its view)
static u32 g_shadow_layers = SHADOW_LAYERS_ALL;   // the light's culling mask (DrawItem / SkinnedDraw layer bits)

// glTF materials (docs/PBR.md): the doubleSided / alphaMode MASK variant of the pass pipeline `base`.
static PipelineId item_pipe(PipelineId base, u32 flags) {
    if (base == PIPE_LIT) return (flags & MAT_ALPHA_MASK) ? PIPE_LIT_MASK : (flags & MAT_TWO_SIDED) ? PIPE_LIT_2S : PIPE_LIT;
    if (base == PIPE_PREPASS) return (flags & MAT_ALPHA_MASK) ? PIPE_PREPASS_MASK : (flags & MAT_TWO_SIDED) ? PIPE_PREPASS_2S : PIPE_PREPASS;
    return base;
}

// Shadow casters (docs/SHADOWS_UNITY_MODEL.md "Cast Shadows", docs/LOD.md): On culls back faces as the
// material does, Two Sided / two-sided materials don't; terrain geomorph casters have their own vertex
// shader; alpha-masked casters alpha-test. `masked_2s`: masked casters without the alpha test
// (local light tiles).
static PipelineId shadow_caster_pipe(u32 flags, bool masked_2s) {
    if (flags & MAT_ALPHA_MASK) return masked_2s ? PIPE_SHADOW_2S : PIPE_SHADOW_MASK;
    bool two_sided = (flags & (MAT_SHADOW_TWO_SIDED | MAT_TWO_SIDED)) != 0;
    if (flags & MAT_TERRAIN_MORPH) return two_sided ? PIPE_SHADOW_MORPH_2S : PIPE_SHADOW_MORPH;
    return two_sided ? PIPE_SHADOW_2S : PIPE_SHADOW;
}

static u32 f32_bits(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

// pipe: the pipeline the caller bound (glTF materials switch to its variants and back), shadow passes
// ignore it.
static void draw_items(VkCommandBuffer cmd, DrawItem *items, u32 count, bool shadow_pass, bool cull, int alpha_mode, PipelineId pipe) {
    // alpha_mode: 0 = opaque only, 1 = blended only, 2 = all
    if (!count) return;
    ArenaMark mark = arena_mark(&g_engine.frame_arena);
    SortKey *keys = ARENA_PUSH_N(&g_engine.frame_arena, SortKey, count);
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        DrawItem *d = &items[i];
        if (!R->meshes[d->mesh].used) continue;
        bool blended = d->mat.color.w < 0.999f && !(d->mat.flags & MAT_ALPHA_MASK);
        if (alpha_mode == 0 && blended) continue;
        if (alpha_mode == 1 && !blended) continue;
        if (shadow_pass && (d->mat.flags & MAT_NO_SHADOW)) continue;
        if (shadow_pass && !((g_shadow_layers >> (d->layer & 7)) & 1u)) continue;   // the light's culling mask
        if (!shadow_pass && (d->mat.flags & MAT_SHADOW_ONLY)) continue;
        if (shadow_pass && (g_shadow_min_radius > 0.0f || g_shadow_cull)) {
            vec3 c; float r;
            item_bounds(d, &c, &r);
            if (r < g_shadow_min_radius) continue;
            if (g_shadow_cull && !sphere_visible(g_shadow_planes, c, r)) { R->stats.culled++; continue; }
        }
        if (cull) {
            vec3 c; float r;
            item_bounds(d, &c, &r);
            if (!sphere_visible(R->frustum, c, r)) { R->stats.culled++; continue; }
        }
        TextureId tex = (d->mat.flags & MAT_TEXTURED) && d->mat.texture ? d->mat.texture : R->white_tex;
        // pipeline variant (2 bits) | glTF map set (9) | albedo texture (10) | mesh (32)
        u64 variant = (u64)(item_pipe(pipe, d->mat.flags) != pipe) + (u64)((d->mat.flags & MAT_ALPHA_MASK) != 0);
        u64 ms = shadow_pass ? 0 : vkr_material_set(&d->mat);
        // Shadow passes: by caster pipeline (shadow_caster_pipe: culling, terrain geomorph, alpha test),
        // masked ones by texture.
        bool masked = shadow_pass && (d->mat.flags & MAT_ALPHA_MASK) != 0;
        u64 sp = shadow_pass ? (u64)shadow_caster_pipe(d->mat.flags, false) : 0u;
        keys[n].key = shadow_pass ? (sp << 56) | (masked ? ((u64)tex << 32) : 0u) | d->mesh : ((variant << 51) | (ms << 42) | ((u64)tex << 32) | d->mesh);
        keys[n].index = i;
        n++;
    }
    qsort(keys, n, sizeof(SortKey), cmp_sortkey);
    u32 bound_mesh = 0, bound_tex = 0, bound_ms = ~0u;
    PipelineId bound_pipe = pipe;
    for (u32 k = 0; k < n; k++) {
        DrawItem *d = &items[keys[k].index];
        GpuMesh *m = &R->meshes[d->mesh];
        if (shadow_pass && !(d->mat.flags & MAT_ALPHA_MASK)) {
            PipelineId sp = shadow_caster_pipe(d->mat.flags, false);
            if (pipe == PIPE_SHADOW && sp != bound_pipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[sp]);
                bound_pipe = sp;
            }
        }
        if (shadow_pass && (d->mat.flags & MAT_ALPHA_MASK)) {
            if (bound_pipe != PIPE_SHADOW_MASK) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW_MASK]);
                bound_pipe = PIPE_SHADOW_MASK;
            }
            TextureId tex = (d->mat.flags & MAT_TEXTURED) && d->mat.texture ? d->mat.texture : R->white_tex;
            if (!R->textures[tex].used || !R->textures[tex].ready) tex = R->white_tex;
            if (tex != bound_tex) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[tex].set, 0, NULL);
                bound_tex = tex;
            }
        }
        if (!shadow_pass) {
            PipelineId p = item_pipe(pipe, d->mat.flags);
            if (p != bound_pipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[p]);
                bound_pipe = p;
            }
            TextureId tex = (d->mat.flags & MAT_TEXTURED) && d->mat.texture ? d->mat.texture : R->white_tex;
            if (!R->textures[tex].used || !R->textures[tex].ready) tex = R->white_tex;
            if (tex != bound_tex) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[tex].set, 0, NULL);
                bound_tex = tex;
            }
            u32 ms = (u32)((keys[k].key >> 42) & 511u);
            if (ms != bound_ms) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 3, 1, &R->mat_sets[ms].set, 0, NULL);
                bound_ms = ms;
            }
        }
        if (d->mesh != bound_mesh) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m->vb.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
            bound_mesh = d->mesh;
        }
        PushLit pc;
        pc.model = d->model;
        pc.color = d->mat.color;
        pc.params = v4(d->mat.roughness, d->mat.metallic, d->mat.emissive, d->mat.tex_scale);
        pc.flags[0] = d->mat.flags;
        pc.flags[1] = shadow_pass ? 0 : d->prev_slot;   // previous model (velocity), 0 = static
        pc.flags[2] = g_froxel_fog_draws ? 1u : 0u;      // blended draws after the fog pass: froxel fog
        pc.flags[3] = shadow_pass ? g_shadow_cascade : 0;
        pc.pbr[0] = f32_bits(d->mat.normal_scale);
        pc.pbr[1] = f32_bits(d->mat.occlusion_strength);
        pc.pbr[2] = d->mat.emissive_rgb;
        pc.pbr[3] = f32_bits(d->mat.alpha_cutoff);
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3;
    }
    if (bound_pipe != pipe) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[pipe]);
    arena_pop_to(mark);
}


// ------------------------------------------------------------------ skinned characters

// fade: shadow pass, PIPE_SKIN_SHADOW_FADE (docs/LOD.md); mat_set: the PBR map set (set 3, vkr_material_set;
// ~0 in the shadow pass, whose shaders sample none).
typedef struct SkinBatch { u32 mesh, texture, first, count; bool fade; u32 mat_set; } SkinBatch;
typedef struct SkinPass { SkinBatch batches[MAX_SKIN_DRAWS]; u32 batch_count; } SkinPass;
typedef struct CpuSkin { VkDeviceSize offset; bool ok; } CpuSkin;

// Skinned passes: 0 = main (world), 1 = shadow, 2 = viewmodel (first-person arms, after the depth
// clear), 3 = phone cameras (the world + the local player's own body, MAT_SELF: selfies).
static u8 g_skin_routed[MAX_SKIN_DRAWS];   // this frame's skinned draws vgeo takes (main view, vkg_route_skinned)

static bool skin_in_pass(const SkinnedDraw *d, int pass) {
    bool vm = (d->flags & MAT_VIEWMODEL) != 0;
    if (pass == 0 && g_skin_routed[d - R->skin_draws]) return false;
    if (pass == 1) return d->cast_shadow && !vm;
    if (pass == 2) return vm;
    if (pass == 3) return !vm && (!(d->flags & MAT_SHADOW_ONLY) || (d->flags & MAT_SELF));
    return !vm && (d->flags & MAT_SHADOW_ONLY) == 0;
}

static bool prev_palette(u32 id, u32 *off, u32 *count);
static u32 g_skin_shadow_inst[MAX_SKIN_DRAWS];   // skinned draw -> its instance record in the shadow pass (~0: none)
static bool g_skin_shadow_gpu;                   // this frame's shadow pass instances are valid (GPU skinning)

// Writes instance records for one pass (culled, sorted by mesh) starting at `base`.
static u32 build_skin_pass(FrameResources *f, int pass, u32 base, SkinPass *out) {
    bool shadow = pass == 1;
    ArenaMark mark = arena_mark(&g_engine.frame_arena);
    u64 *keys = ARENA_PUSH_N(&g_engine.frame_arena, u64, R->skin_draw_count + 1);
    u32 *draw_of = ARENA_PUSH_N(&g_engine.frame_arena, u32, R->skin_draw_count + 1);
    u32 *order = ARENA_PUSH_N(&g_engine.frame_arena, u32, R->skin_draw_count + 1);
    SkinBatchRun *runs = ARENA_PUSH_N(&g_engine.frame_arena, SkinBatchRun, R->skin_draw_count + 1);
    u32 n = 0;
    for (u32 i = 0; i < R->skin_draw_count; i++) {
        const SkinnedDraw *d = &R->skin_draws[i];
        if (!skin_in_pass(d, pass)) continue;
        if (pass == 0 && R->r_culling->i && d->radius > 0 && !sphere_visible(R->frustum, d->center, d->radius)) { R->stats.culled++; continue; }
        keys[n] = ((u64)d->texture << 32) | d->mesh;
        if (shadow && d->fade != 0.0f) keys[n] |= 1ull << 63;   // fading casters batch apart (dithered shadow pipeline)
        draw_of[n] = i;
        n++;
    }
    // Instanced batches per (texture, mesh): modular characters sharing parts batch per (part, LOD).
    u32 nb = skin_batch_build(keys, n, order, runs);
    GpuSkinInstance *inst = (GpuSkinInstance *)f->instances.mapped;
    for (u32 k = 0; k < n; k++) {
        const SkinnedDraw *d = &R->skin_draws[draw_of[order[k]]];
        GpuSkinInstance *g = &inst[base + k];
        if (shadow) g_skin_shadow_inst[draw_of[order[k]]] = base + k;
        g->color = d->color;
        g->params = v4(d->roughness, d->metallic, d->emissive, d->fade);
        g->recon = d->recon;
        g->info[0] = d->bone_count & 0xFFFFFu;
        g->info[1] = d->flags;
        g->info[2] = 0;
        float ns = d->normal_scale != 0.0f ? d->normal_scale : 1.0f;
        memcpy(&g->info[3], &ns, sizeof(ns));   // skin.vert -> PBR_PARAMS.x (shaders/pbr_maps.glsl)
        // Last frame's palette of the same object (appended after this frame's bones).
        if (!shadow && d->object_id && !R->camera_cut) {
            u32 off, count;
            if (prev_palette(d->object_id, &off, &count) && count == (d->bone_count >> 20) && off + count <= R->prev_bone_count)
                g->info[2] = R->skin_bone_count + off + 1;
        }
    }
    out->batch_count = nb;
    for (u32 b = 0; b < nb; b++) {
        out->batches[b] = (SkinBatch){ (u32)(runs[b].key & 0xFFFFFFFFu), (u32)((runs[b].key >> 32) & 0x7FFFFFFFu), base + runs[b].first, runs[b].count, (runs[b].key >> 63) != 0, ~0u };
        if (shadow) continue;
        // PBR maps of the batch: batches are per (albedo, mesh) and a mesh (one GPU part LOD) has one
        // set of maps, so the batch's first draw speaks for all of it.
        const SkinnedDraw *d = &R->skin_draws[draw_of[order[runs[b].first]]];
        Material mm;
        memset(&mm, 0, sizeof(mm));
        mm.flags = d->flags & MAT_PBR_MAPS;
        mm.mr_tex = d->mr_tex;
        mm.normal_tex = d->normal_tex;
        mm.occlusion_tex = d->occlusion_tex;
        mm.emissive_tex = d->emissive_tex;
        out->batches[b].mat_set = vkr_material_set(&mm);
    }
    arena_pop_to(mark);
    return n;
}

// fade: -1 every batch, 0 / 1 only the batches without / with a shadow fade (shadow pass pipelines).
static void draw_skin_pass_f(VkCommandBuffer cmd, const SkinPass *p, int fade) {
    u32 bound_tex = 0xFFFFFFFFu, bound_ms = 0xFFFFFFFFu;
    for (u32 b = 0; b < p->batch_count; b++) {
        const SkinBatch *sb = &p->batches[b];
        if (fade >= 0 && (int)sb->fade != fade) continue;
        GpuMesh *m = &R->meshes[sb->mesh];
        // Albedo of textured skins (set 1); white otherwise.
        TextureId tex = sb->texture && R->textures[sb->texture].used && R->textures[sb->texture].ready ? sb->texture : R->white_tex;
        if (tex != bound_tex) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[tex].set, 0, NULL);
            bound_tex = tex;
        }
        // PBR maps (set 3): normal / ORM / emissive of textured skins (set 0 = none).
        if (sb->mat_set != ~0u && sb->mat_set != bound_ms && sb->mat_set < R->mat_set_count) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 3, 1, &R->mat_sets[sb->mat_set].set, 0, NULL);
            bound_ms = sb->mat_set;
        }
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m->vb.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m->index_count, sb->count, 0, 0, sb->first);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3 * sb->count;
    }
}
static void draw_skin_pass(VkCommandBuffer cmd, const SkinPass *p) { draw_skin_pass_f(cmd, p, -1); }

// CPU fallback: skin into lit vertices in the dynamic buffer (same palettes as the GPU path).
static void cpu_skin_all(FrameResources *f, CpuSkin *out) {
    for (u32 i = 0; i < R->skin_draw_count; i++) {
        const SkinnedDraw *d = &R->skin_draws[i];
        GpuMesh *m = &R->meshes[d->mesh];
        out[i].ok = false;
        if (!m->cpu_verts) continue;
        VkDeviceSize off = (f->dyn_used + 15) & ~(VkDeviceSize)15;
        VkDeviceSize size = sizeof(Vertex) * m->vertex_count;
        if (off + size > f->dyn.size) continue;
        Vertex *dst = (Vertex *)((u8 *)f->dyn.mapped + off);
        const mat4 *pal = R->skin_bones + (d->bone_count & 0xFFFFFu);
        u32 nb = d->bone_count >> 20;
        for (u32 v = 0; v < m->vertex_count; v++) {
            const SkinVertex *sv = &m->cpu_verts[v];
            vec3 p = v3(0, 0, 0), nn = v3(0, 0, 0);
            for (int k = 0; k < 4; k++) {
                if (!sv->weights[k] || sv->joints[k] >= nb) continue;
                float w = (float)sv->weights[k] / 255.0f;
                p = v3_madd(p, mat4_mul_point(pal[sv->joints[k]], sv->pos), w);
                nn = v3_madd(nn, mat4_mul_dir(pal[sv->joints[k]], sv->normal), w);
            }
            dst[v].pos = p;
            dst[v].normal = v3_norm(nn);
            dst[v].uv = sv->uv;
            dst[v].color = sv->color;
        }
        f->dyn_used = off + size;
        out[i].offset = off;
        out[i].ok = true;
    }
}

static bool sphere_in_planes(const vec4 planes[6], vec3 c, float r) {
    for (int p = 0; p < 6; p++)
        if (planes[p].x * c.x + planes[p].y * c.y + planes[p].z * c.z + planes[p].w < -r) return false;
    return true;
}

// Local light shadow tiles (vk_light_shadow.c): every opaque shadow caster touching the light's
// sphere and the tile's frustum, static then GPU-skinned (one instance of the shadow pass records).
u32 render_draw_shadow_casters(VkCommandBuffer cmd, FrameResources *f, vec4 sphere, const vec4 planes[6], u32 cascade, u32 layers) {
    (void)f;
    vec3 lc = v3(sphere.x, sphere.y, sphere.z);
    // The 6 faces of a point light share its sphere: the casters touching it are found once
    // (bounds kept), each face then only tests its frustum.
    static u32 *cand;
    static vec4 *cand_b;
    static u32 cand_n, cand_cap, cand_frame = ~0u;
    static vec4 cand_sphere;
    if (cand_frame != R->frame_index || memcmp(&cand_sphere, &sphere, sizeof(vec4)) != 0) {
        if (cand_cap < R->draw_count) {
            cand_cap = R->draw_count + 256;
            cand = (u32 *)realloc(cand, sizeof(u32) * cand_cap);
            cand_b = (vec4 *)realloc(cand_b, sizeof(vec4) * cand_cap);
        }
        cand_n = 0;
        for (u32 i = 0; i < R->draw_count; i++) {
            DrawItem *d = &R->draws[i];
            if (!R->meshes[d->mesh].used || (d->mat.flags & MAT_NO_SHADOW) || d->mat.color.w < 0.999f) continue;
            vec3 c;
            float r;
            item_bounds(d, &c, &r);
            if (v3_len2(v3_sub(c, lc)) > (r + sphere.w) * (r + sphere.w)) continue;
            cand[cand_n] = i;
            cand_b[cand_n++] = v4(c.x, c.y, c.z, r);
        }
        cand_frame = R->frame_index;
        cand_sphere = sphere;
    }
    u32 n = 0, bound_mesh = 0;
    PipelineId bound_pipe = PIPE_SHADOW;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
    for (u32 k = 0; k < cand_n; k++) {
        DrawItem *d = &R->draws[cand[k]];
        if (!((layers >> (d->layer & 7)) & 1u)) continue;   // the light's culling mask
        if (!sphere_in_planes(planes, v3(cand_b[k].x, cand_b[k].y, cand_b[k].z), cand_b[k].w)) continue;
        // Cast Shadows Two Sided / two-sided or masked materials: no back-face culling; terrain geomorph.
        PipelineId sp = shadow_caster_pipe(d->mat.flags, true);
        if (sp != bound_pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[sp]);
            bound_pipe = sp;
        }
        GpuMesh *m = &R->meshes[d->mesh];
        if (d->mesh != bound_mesh) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m->vb.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
            bound_mesh = d->mesh;
        }
        PushLit pc;
        memset(&pc, 0, sizeof(pc));
        pc.model = d->model;
        pc.color = d->mat.color;
        pc.flags[0] = d->mat.flags;
        pc.flags[3] = cascade;
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3;
        n++;
    }
    if (!g_skin_shadow_gpu) return n;
    int bound = -1;   // the bound pipeline: 0 opaque casters, 1 fading casters (docs/LOD.md)
    for (u32 i = 0; i < R->skin_draw_count; i++) {
        const SkinnedDraw *d = &R->skin_draws[i];
        if (g_skin_shadow_inst[i] == ~0u || d->radius <= 0 || !((layers >> (d->layer & 7)) & 1u)) continue;
        if (v3_len2(v3_sub(d->center, lc)) > (d->radius + sphere.w) * (d->radius + sphere.w) || !sphere_in_planes(planes, d->center, d->radius)) continue;
        int want = d->fade != 0.0f ? 1 : 0;
        if (bound != want) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[want ? PIPE_SKIN_SHADOW_FADE : PIPE_SKIN_SHADOW]);
            if (bound < 0) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
                PushLit pc;
                memset(&pc, 0, sizeof(pc));
                pc.flags[3] = cascade;
                vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
            }
            bound = want;
        }
        GpuMesh *m = &R->meshes[d->mesh];
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m->vb.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, g_skin_shadow_inst[i]);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3;
        n++;
    }
    return n;
}

static void draw_cpu_skinned(VkCommandBuffer cmd, FrameResources *f, const CpuSkin *cs, int pass) {
    bool shadow = pass == 1;
    for (u32 i = 0; i < R->skin_draw_count; i++) {
        const SkinnedDraw *d = &R->skin_draws[i];
        if (!cs[i].ok) continue;
        if (!skin_in_pass(d, pass)) continue;
        if (pass == 0 && R->r_culling->i && d->radius > 0 && !sphere_visible(R->frustum, d->center, d->radius)) continue;
        GpuMesh *m = &R->meshes[d->mesh];
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &cs[i].offset);
        vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        PushLit pc;
        pc.model = mat4_identity();
        pc.color = d->color;
        pc.params = v4(d->roughness, d->metallic, d->emissive, 1.0f);
        pc.flags[0] = d->flags & ~(u32)(MAT_SHADOW_ONLY | MAT_PBR_MAPS);   // CPU fallback: no maps (set 3 = none)
        pc.flags[1] = pc.flags[2] = 0;
        pc.flags[3] = pass == 1 || g_shadow_vm_pass ? g_shadow_cascade : 0;
        memset(pc.pbr, 0, sizeof(pc.pbr));
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3;
    }
}

// ------------------------------------------------------------------ previous transforms (velocity)

typedef struct ObjSlot {
    u32 id;
    u32 model_frame, pal_frame;   // frame_index the entry was written (0 = never)
    mat4 model;
    u32 pal_offset, pal_count;    // into prev_bones (the palette of pal_frame)
} ObjSlot;

static size_t obj_slot_size(void) { return sizeof(ObjSlot); }

static ObjSlot *obj_find(u32 id, bool insert) {
    if (!id || !R->obj_slots) return NULL;
    u32 h = (id * 2654435761u) & (OBJ_SLOTS - 1);
    for (u32 n = 0; n < 32; n++) {
        ObjSlot *s = &R->obj_slots[(h + n) & (OBJ_SLOTS - 1)];
        if (s->id == id) return s;
        // Free or stale for more than a second: reuse.
        bool stale = s->id && R->frame_index > DG_MAX(s->model_frame, s->pal_frame) + 120;
        if (insert && (!s->id || stale)) {
            memset(s, 0, sizeof(*s));
            s->id = id;
            return s;
        }
    }
    return NULL;
}

// Moving draws: last frame's model goes into this frame's DrawPrev list (prev_slot = index + 1).
static void assign_prev_model(DrawItem *d) {
    d->prev_slot = 0;
    if (!d->object_id) return;
    ObjSlot *s = obj_find(d->object_id, true);
    if (!s) return;
    if (s->model_frame == R->frame_index && !R->camera_cut && R->prev_model_count < MAX_DRAWS) {
        R->prev_models_cpu[R->prev_model_count] = s->model;
        d->prev_slot = ++R->prev_model_count;
    } else if (R->prev_model_count < MAX_DRAWS) {
        // First frame of the object: no motion.
        R->prev_models_cpu[R->prev_model_count] = d->model;
        d->prev_slot = ++R->prev_model_count;
    }
}

static void store_prev_models(DrawItem *items, u32 count) {
    for (u32 i = 0; i < count; i++) {
        if (!items[i].object_id) continue;
        ObjSlot *s = obj_find(items[i].object_id, true);
        if (!s) continue;
        s->model = items[i].model;
        s->model_frame = R->frame_index + 1;   // read next frame as "frame_index - 1 + 1"
    }
}

static bool prev_palette(u32 id, u32 *off, u32 *count) {
    ObjSlot *s = obj_find(id, false);
    if (!s || s->pal_frame != R->frame_index) return false;
    *off = s->pal_offset;
    *count = s->pal_count;
    return true;
}

// ------------------------------------------------------------------ frame matrices

// Validation (docs/SHADOWS.md "Tests"): every tile the lit shaders will sample this frame
// (lighting.glsl's rules on the uniforms) was rendered this frame. A miss is counted and logged
// (shadow_info, shadow_switch_stress fail on it).
static void shadow_check_sampled(const FrameData *fd) {
    u32 want = 0;
    bool on = fd->sun_color.w > 0.5f && fd->ssky0.y > 0.0f;
    int count = (int)fd->shadow_cfg3.x;
    int first = count > 1 && fd->ssky0.z <= 0.0f ? 1 : 0;
    if (on)
        for (int k = first; k < count; k++) want |= 1u << k;
    if (fd->ssky1.w > 0.0f && fd->sun_color.w > 0.5f) want |= 1u << 5;
    if (on && fd->shadow_vm.z > 0.0f) want |= 1u << 4;
    u32 miss = want & ~R->shadow_drawn;
    if (miss) {
        if (!R->shadow_unrendered++) log_error("shadows: frame %u samples unrendered shadow tiles 0x%x (drawn 0x%x)", R->frame_index, miss, R->shadow_drawn);
    }
}

static void compute_matrices(FrameData *fd) {
    float aspect = view_aspect();
    RenderCamera *c = &R->cam;
    R->view = mat4_fps_view(c->pos, c->yaw, c->pitch, c->roll);
    R->proj = mat4_perspective(c->fov_deg * DG_DEG2RAD, aspect, c->znear, c->zfar);
    R->view_proj = mat4_mul(R->proj, R->view);
    float vm_fov = render_vm_fov();
    R->vm_proj = mat4_perspective(vm_fov * DG_DEG2RAD, aspect, 0.01f, 100.0f);
    extract_frustum(R->view_proj, R->frustum);

    // Sub-pixel jitter (Halton 2,3) for TAA / TAA-U: an NDC translation after the projection.
    // R->view_proj stays unjittered for game queries (world_to_screen, culling).
    R->prev_jitter = R->jitter;
    R->jitter = v2(0, 0);
    if (post_wants_jitter()) {
        vec2 px = post_jitter_pixels(R->frame_index);
        R->jitter = v2(2.0f * px.x / (float)R->hdr.w, 2.0f * px.y / (float)R->hdr.h);
    }
    mat4 jt = mat4_translate(v3(R->jitter.x, R->jitter.y, 0.0f));
    R->proj_j = mat4_mul(jt, R->proj);
    R->vm_proj_j = mat4_mul(jt, R->vm_proj);
    // Camera cut: teleports / respawns reset the temporal history (and the velocities).
    if (!R->have_prev || v3_len(v3_sub(c->pos, R->prev_cam_pos)) > 4.0f) R->camera_cut = true;
    R->prev_view_proj_nj = R->have_prev && !R->camera_cut ? R->view_proj_nj : R->view_proj;
    R->prev_vm_view_proj_nj = R->have_prev && !R->camera_cut ? R->vm_view_proj_nj : mat4_mul(R->vm_proj, R->view);
    R->view_proj_nj = R->view_proj;
    R->vm_view_proj_nj = mat4_mul(R->vm_proj, R->view);
    R->prev_cam_pos = c->pos;

    // Directional shadow (docs/SHADOWS.md "Cascades", docs/SHADOWS_UNITY_MODEL.md): Unity's quality
    // knobs (R->shq: Shadows, Resolution, Projection, Distance, Cascades + splits, Border, Near Plane
    // Offset) and the sun's Light settings (R->sun_shadow: type, resolution, strength, bias, normal
    // bias, culling mask) over shadow_csm.c: 1 / 2 / 4 cascades in a 2 x 2 atlas at Unity's split
    // percentages; Stable Fit = sector spheres snapped to whole texels, Close Fit = the slice's
    // light-space bounds (denser, swims). The moon has its own map. Bias and filter units follow each
    // cascade's texel size. Matrices, tiles and uniforms are all built here, in the frame that
    // renders and samples them.
    RenderEnvironment *e = &R->env;
    QualityShadowSettings *q = &R->shq;
    const LightShadowSettings *sl = &R->sun_shadow;
    R->sun_type = shs_effective_type(sl->type, q->mode);
    R->sun_filter = shs_filter(R->sun_type, q->soft_quality, SHADOW_LIGHT_DIRECTIONAL);
    vec3 L = v3_norm(e->sun_dir);
    float sun_elev = asinf(clampf(L.y, -1.0f, 1.0f));
    // The fit never looks up from under the horizon (the sun owns the cascades while any of its
    // disc is up): at least 1 deg; the shading keeps the true direction (a sub-degree difference).
    vec3 Lfit = L;
    if (Lfit.y < 0.0175f) {
        float hz = sqrtf(Lfit.x * Lfit.x + Lfit.z * Lfit.z);
        Lfit = hz > 1e-4f ? v3(Lfit.x / hz * 0.99985f, 0.0175f, Lfit.z / hz * 0.99985f) : v3(0, 1, 0);
        sun_elev = fmaxf(sun_elev, 1.0f * DG_DEG2RAD);
    }
    mat4 lview = csm_light_view(Lfit);
    float low_sun = csm_low_sun(sun_elev);
    // A Shadow Distance change glides over ~0.2 s instead of resizing every cascade at once.
    float fdt = (float)g_engine.frame_dt;
    if (R->shadow_dist <= 0.0f || R->camera_cut || fdt <= 0.0f) R->shadow_dist = q->distance;
    else R->shadow_dist = q->distance + (R->shadow_dist - q->distance) * expf(-fdt / 0.08f);
    if (fabsf(R->shadow_dist - q->distance) < 0.01f) R->shadow_dist = q->distance;
    QualityShadowSettings qd = *q;
    qd.distance = R->shadow_dist;
    float splits[4];
    int ncasc = shs_cascade_splits(&qd, splits);
    u32 tile = R->shadow_size / 2;
    CsmParams cp = { ncasc, tile, R->shadow_dist, c->fov_deg, aspect, splits };
    csm_layout(&cp, &R->csm);
    bool close_fit = q->projection == SHADOW_CLOSE_FIT;
    float reach = q->near_plane_offset;
    // View basis from the view matrix: the spheres / slices sit along it.
    vec3 fwd = v3_norm(v3(-R->view.m[2], -R->view.m[6], -R->view.m[10]));
    vec3 right = v3_norm(v3(R->view.m[0], R->view.m[4], R->view.m[8])), upv = v3_norm(v3(R->view.m[1], R->view.m[5], R->view.m[9]));
    float tan_v = tanf(c->fov_deg * 0.5f * DG_DEG2RAD), tan_h = tan_v * aspect;
    float bias[4], span[4], nbias[4], mpt[4];
    for (int k = 0; k < ncasc; k++) {
        CsmTile t;
        if (close_fit) {
            float d0 = k ? splits[k - 1] * (1.0f - q->border) : 0.0f;
            csm_fit_close(&lview, c->pos, fwd, right, upv, tan_v, tan_h, d0, splits[k], tile, reach, &t);
        } else {
            csm_fit_reach(&lview, v3_madd(c->pos, fwd, R->csm.ahead[k]), R->csm.radius[k], tile, R->csm.radius[k] + 2.0f, reach, &t);
        }
        float cm, nm;
        shs_bias_m(t.texel_m, sl->bias, sl->normal_bias, low_sun, &cm, &nm);
        bias[k] = cm / t.span;
        nbias[k] = nm;
        span[k] = t.span;
        mpt[k] = t.texel_m;
        R->csm.texel_m[k] = t.texel_m;
        R->csm.radius[k] = t.radius;
        R->shadow_radius[k] = t.radius;
        R->shadow_center[k] = t.center;
        R->shadow_vp_ndc[k] = t.vp;
        // NDC -> the cascade's atlas tile (x right, y down in the atlas).
        mat4 tm = mat4_identity();
        tm.m[0] = 0.5f;
        tm.m[5] = 0.5f;
        tm.m[12] = (k & 1) ? 0.5f : -0.5f;
        tm.m[13] = (k >> 1) ? 0.5f : -0.5f;
        R->shadow_vps[k] = mat4_mul(tm, t.vp);
    }
    // Unused cascade slots repeat the last one (shadow_vp / bias.w users: decals, froxels, particles).
    for (int k = ncasc; k < 4; k++) {
        R->shadow_vps[k] = R->shadow_vps[ncasc - 1];
        R->shadow_vp_ndc[k] = R->shadow_vp_ndc[ncasc - 1];
        R->shadow_radius[k] = R->shadow_radius[ncasc - 1];
        R->shadow_center[k] = R->shadow_center[ncasc - 1];
        bias[k] = bias[ncasc - 1];
        span[k] = span[ncasc - 1];
        nbias[k] = nbias[ncasc - 1];
        mpt[k] = mpt[ncasc - 1];
    }
    R->shadow_splits = v4(splits[0], splits[1], splits[2], splits[3]);
    R->shadow_vp = R->shadow_vps[3];
    R->shadow_bias = v4(bias[0], bias[1], bias[2], bias[3]);

    // Sky-driven shadows (shadow_sky.c, docs/SHADOWS.md): which tiles are drawn (none for a sun of type
    // None, under Shadows Disable or once the sky cutoff's fade has ended; no cascade 0 in wide mode),
    // the moon map while the sun owns the cascades, the filter's units. Levels without the
    // time-of-day sky: a clear sun, always cast.
    const ShadowSkyOut *ss = e->shadow_sky_valid ? &e->shadow_sky : NULL;
    bool shadows_on = q->mode != SHADOWS_DISABLE && R->sun_type != SHADOW_NONE && R->shadow_size > 16;
    bool key_cast = !ss || ss->key.cast;
    float near_w = ss && R->sun_filter.mode == SHF_PCSS ? ss->near_weight : 1.0f;
    for (int k = 0; k < 4; k++) R->shadow_active[k] = shadows_on && key_cast && k < ncasc && !(k == 0 && ncasc >= 2 && near_w <= 0.0f);
    float moon_fade = ss && ss->second_shadowed && e->fill_intensity > 0.0f ? ss->second.fade : 0.0f;
    R->moon_active = shadows_on && moon_fade > 0.0f && R->moon_shadow_size > 16;
    int mk = DG_MIN(1, ncasc - 1);
    float moon_r = R->csm.radius[mk];
    vec4 moon_u = v4(0, 1, 0, 0);
    R->shadow_radius[4] = moon_r;
    if (R->moon_active) {
        // One map around the camera (the middle cascade's sphere) along the moon, snapped.
        vec3 L2 = v3_norm(e->fill_dir);
        if (L2.y < 0.0175f) {
            float hz = sqrtf(L2.x * L2.x + L2.z * L2.z);
            L2 = hz > 1e-4f ? v3(L2.x / hz * 0.99985f, 0.0175f, L2.z / hz * 0.99985f) : v3(0, 1, 0);
        }
        mat4 mview = csm_light_view(L2);
        CsmTile t;
        csm_fit_reach(&mview, v3_madd(c->pos, fwd, R->csm.ahead[mk]), moon_r, R->moon_shadow_size, moon_r + 2.0f, reach, &t);
        float cm, nm;
        shs_bias_m(t.texel_m, sl->bias, sl->normal_bias, csm_low_sun(asinf(clampf(L2.y, -1.0f, 1.0f))), &cm, &nm);
        moon_u = v4(t.texel_m, t.span, cm / t.span, nm);
        R->shadow_center[4] = t.center;
        R->shadow_vp_ndc[4] = t.vp;
        R->shadow_vp_moon = t.vp;
    }

    // The viewmodel tile (docs/SHADOWS.md "The viewmodel tile"): the first-person rig's own map,
    // fitted every frame around its drawn bounds, snapped, along the key light; it follows the
    // directional light's type, quality and strength.
    {
        vec3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
        bool any = false;
        for (u32 i = 0; i < R->vm_draw_count; i++) {
            vec3 bc;
            float br;
            item_bounds(&R->vm_draws[i], &bc, &br);
            lo = v3_min(lo, v3_sub(bc, v3(br, br, br)));
            hi = v3_max(hi, v3_add(bc, v3(br, br, br)));
            any = true;
        }
        for (u32 i = 0; i < R->skin_draw_count; i++) {
            const SkinnedDraw *d = &R->skin_draws[i];
            if (!(d->flags & MAT_VIEWMODEL) || d->radius <= 0.0f) continue;
            lo = v3_min(lo, v3_sub(d->center, v3(d->radius, d->radius, d->radius)));
            hi = v3_max(hi, v3_add(d->center, v3(d->radius, d->radius, d->radius)));
            any = true;
        }
        vec3 rig = any ? v3_scale(v3_add(lo, hi), 0.5f) : c->pos;
        // A rig centre far from the eye is a bad bound (a skinned draw without one): the eye's front.
        if (v3_len(v3_sub(rig, c->pos)) > 1.5f) rig = v3_madd(c->pos, fwd, 0.45f);
        R->shadow_vm_active = shadows_on && key_cast && any && R->r_vm_shadow->i && R->vm_shadow_size > 8;   // (the viewmodel is always drawn: no full-screen scope mode)
        R->shadow_vm_tile = (CsmTile){ 0 };
        if (R->shadow_vm_active) csm_fit_vm(&lview, rig, R->vm_shadow_size, &R->shadow_vm_tile);
    }
    {
        int model = R->sun_filter.mode == SHF_PCSS ? 2 : (R->sun_filter.mode == SHF_TENT ? 1 : 0);
        float key_r = ss ? ss->key.radius_deg : 0.265f, moon_rad = ss ? ss->second.radius_deg : 0.26f;
        float pcss_w = R->sun_filter.mode == SHF_PCSS ? 1.0f : 0.0f;
        fd->shadow_vp_moon = R->moon_active ? R->shadow_vp_moon : mat4_identity();
        fd->ssky0 = v4(tanf(key_r * DG_DEG2RAD), ss ? ss->key.fade : 1.0f, near_w, (float)model);
        fd->ssky1 = v4(0.0f, ss ? ss->pcss_max_texels : 32.0f, ss ? ss->pcss_search_m : 30.0f, R->moon_active ? moon_fade : 0.0f);
        // Contact shadows: rays only as long as the penumbra stays under contact_px pixels (a soft
        // source has a sharp contact zone that is short); 0.35 m is the clear-sun length.
        float px8 = 8.0f * 2.0f * tanf(c->fov_deg * DG_DEG2RAD * 0.5f) / (float)DG_MAX(R->hdr.h, 1u);
        float pen_len = (ss ? ss->contact_px : 1.5f) * px8 / fmaxf(2.0f * tanf(key_r * DG_DEG2RAD), 1e-5f);
        R->contact_scale = clampf(pen_len / 0.35f, 0.1f, 1.0f);
        fd->ssky2 = v4(tanf(moon_rad * DG_DEG2RAD), (float)R->r_shadow_debug->i, R->contact_scale, moon_r);
        fd->shadow_mpt = v4(mpt[0], mpt[1], mpt[2], mpt[3]);
        fd->shadow_span = v4(span[0], span[1], span[2], span[3]);
        fd->shadow_nbias = v4(nbias[0], nbias[1], nbias[2], nbias[3]);
        // The kernel rotates per frame only under TAA (it averages the pattern); otherwise it is fixed.
        R->shadow_taa = post_wants_jitter();
        fd->shadow_cfg = v4(R->shadow_taa ? 1.0f : 0.0f, pcss_w, ss ? ss->pcf_ref_m : 2.0f, ss ? ss->min_texels : 1.0f);
        float era_m = ss && ss->fixed_texels > 0.0f ? ss->fixed_texels * mpt[0] : 0.0f;
        float vm_k = tanf(c->fov_deg * 0.5f * DG_DEG2RAD) / tanf(fmaxf(vm_fov, 1.0f) * 0.5f * DG_DEG2RAD);
        fd->shadow_cfg2 = v4(era_m, low_sun, R->shadow_dist, vm_k);
        fd->shadow_cfg3 = v4((float)ncasc, close_fit ? 0.0f : 1.0f, fmaxf(q->border, 0.01f), sl->strength);
        fd->shadow_cfg4 = v4((float)R->sun_filter.mode, R->sun_filter.tent, 0.0f, 0.0f);
        fd->shadow_moon = moon_u;
        fd->shadow_moon2 = v4(1.0f / (float)DG_MAX(R->moon_shadow_size, 1u), 0, 0, 0);
        const CsmTile *vt = &R->shadow_vm_tile;
        CsmBias vb = csm_bias_vm(vt->texel_m, sun_elev);
        vb.const_m *= fmaxf(sl->bias, 0.0f);
        vb.normal_m *= fmaxf(sl->normal_bias, 0.0f);
        fd->shadow_vp_vm = R->shadow_vm_active ? vt->vp : mat4_identity();
        fd->shadow_vm = v4(vt->texel_m, vt->span, R->shadow_vm_active ? 1.0f : 0.0f, vb.normal_m);
        fd->shadow_vm2 = v4(1.0f / (float)DG_MAX(R->vm_shadow_size, 1u), vt->span > 0.0f ? vb.const_m / vt->span : 0.0f, CSM_VM_SKIP, pcss_w);
        R->shadow_dbg_bias_m = bias[0] * span[0];
    }
    {
        // LOD cross-fades (docs/LOD.md): the dither turns per frame where TAA resolves it into a blend
        bool on = lod_active();
        const LodTable *lt = lod_table();
        float rot = on && R->r_aa->i == 3 ? (float)(1u + (R->frame_index & 63u)) : 0.0f;
        u32 dbg = (lod_fade_debug() ? 1u : 0u) | (lod_debug() >= 2 ? 2u : 0u);
        fd->lod_fade = v4(rot, (float)dbg, on ? 1.0f : 0.0f, on ? lt->ter_detail * lod_scale(LODC_TERRAIN) / lt->cat[LODC_TERRAIN] : 1.0f);
        fd->lod_terrain = v4(lt->ter_morph, 32.0f * 1.41421356f, 0, 0);
    }

    fd->view_proj = mat4_mul(R->proj_j, R->view);
    fd->view = R->view;
    fd->proj = R->proj_j;
    fd->vm_proj = R->vm_proj_j;
    fd->shadow_vp = R->shadow_vp;
    memcpy(fd->shadow_vps, R->shadow_vps, sizeof(fd->shadow_vps));
    fd->shadow_splits = R->shadow_splits;
    fd->shadow_bias = R->shadow_bias;
    fd->view_proj_nj = R->view_proj_nj;
    fd->prev_view_proj_nj = R->prev_view_proj_nj;
    fd->vm_view_proj_nj = R->vm_view_proj_nj;
    fd->prev_vm_view_proj_nj = R->prev_vm_view_proj_nj;
    fd->jitter = v4(R->jitter.x, R->jitter.y, R->prev_jitter.x, R->prev_jitter.y);
    fd->post_flags = v4(0, 0, 0, 0);
    fd->post_params = v4(1, 1, 1, 1);
    fd->post_fog = v4(0, 80, 0, 0);
    fd->ss_view_proj = mat4_mul(R->proj_j, R->view);   // used only by a second camera (ss_info.x, the scope PiP)
    fd->ss_view = R->view;
    fd->ss_info = v4(0, 0, 0, 0);
    fd->camera_pos = v4(c->pos.x, c->pos.y, c->pos.z, (float)g_engine.real_time);
    fd->sun_dir = v4(L.x, L.y, L.z, e->sun_intensity);
    fd->sun_color = v4(e->sun_color.x, e->sun_color.y, e->sun_color.z, R->shadow_active[0] || R->shadow_active[1] ? 1.0f : 0.0f);
    fd->sky_ambient = v4(e->sky_ambient.x, e->sky_ambient.y, e->sky_ambient.z, e->ambient_intensity);
    fd->ground_ambient = v4(e->ground_ambient.x, e->ground_ambient.y, e->ground_ambient.z, (float)R->light_count);
    // Volumetric fog replaces the analytic per-pixel fog of the lit shaders.
    bool vol = post_settings()->volumetrics > 0 && fg_pass_enabled("vol_inject");
    fd->fog = v4(e->fog_color.x, e->fog_color.y, e->fog_color.z, vol ? 0.0f : e->fog_density);
    // z: unused (the filter follows the sky: ssky0 / ssky1)
    fd->params = v4(R->r_grid->i ? 1.0f : 0.0f, 1.0f / (float)R->shadow_size, 0.0f, 0.0f);
    fd->screen = v4((float)R->hdr.w, (float)R->hdr.h, 1.0f / (float)R->hdr.w, 1.0f / (float)R->hdr.h);
    {
        // Time-of-day sky: fill light, environment / SH ambient, cloud shadow (lit shaders).
        bool atmo = sky_active();
        vec3 fd_dir = v3_len2(e->fill_dir) > 0 ? v3_norm(e->fill_dir) : v3(0, 1, 0);
        fd->fill_dir = v4(fd_dir.x, fd_dir.y, fd_dir.z, atmo ? e->fill_intensity : 0.0f);
        fd->fill_color = v4(e->fill_color.x, e->fill_color.y, e->fill_color.z, 0.0f);
        fd->sky_flags = v4(atmo ? 1.0f : 0.0f, e->ambient_scale > 0 ? e->ambient_scale : 1.0f, (float)(sky_env_mips() - 1), atmo && e->cloud_cover > 0.001f ? 1.0f : 0.0f);
        vec2 co;
        float csz;
        sky_cloud_shadow_region(&co, &csz);
        fd->cshadow = v4(co.x, co.y, 1.0f / fmaxf(csz, 1.0f), 0.0f);
    }
    vkls_select(fd);   // shadowed point / spot lights (writes spots[].params.y before the copy)
    memcpy(fd->lights, R->lights, sizeof(GpuPointLight) * R->light_count);
    memcpy(fd->spots, R->spots, sizeof(GpuSpotLight) * R->spot_count);
    fd->spot_info = v4((float)R->spot_count, R->spot_cookie_bound == R->spot_cookie && R->spot_cookie ? 1.0f : 0.0f, 0, 0);
    {
        const RenderWeather *wx = &R->weather;
        if (wx->occlusion_update && wx->occlusion_extent > 1.0f) {
            // Top-down ortho around the (snapped) centre: 60 m above it down to 60 m below.
            float e = wx->occlusion_extent, top = wx->occlusion_center.y + 60.0f;
            R->occl_range = 120.0f;
            vec3 eye = v3(wx->occlusion_center.x, top, wx->occlusion_center.z);
            mat4 view = mat4_look_at(eye, v3_add(eye, v3(0, -1, 0)), v3(0, 0, -1));
            R->occl_vp = mat4_mul(mat4_ortho(-e, e, -e, e, 0.0f, R->occl_range), view);
            R->occl_info = v4(eye.x, eye.z, e, top);
        }
        fd->occl_vp = R->occl_vp;
        fd->wx0 = v4(wx->wetness, wx->puddles, wx->snow_cover, wx->lightning);
        fd->wx1 = v4(wx->rain, wx->snowfall, wx->time, R->occl_valid ? 1.0f : 0.0f);
        float ws = sqrtf(wx->wind.x * wx->wind.x + wx->wind.z * wx->wind.z);
        fd->wx2 = v4(wx->wind.x, wx->wind.z, ws, R->puddle_bound && R->puddle_bound != R->white_tex ? 1.0f : 0.0f);
        fd->wx3 = R->occl_info;
        fd->wx4 = v4(R->occl_range, (float)wx->precip_quality, wx->puddle_reflections ? 1.0f : 0.0f, 0);
        fd->wx5 = v4(wx->gust, fmodf(wx->time * 0.7f, 6.2831853f), wx->time, atan2f(wx->wind.x, -wx->wind.z));
        fd->puddle_xf = wx->puddle_xform;
    }
    vks_frame_data(fd);
    vkgr_frame_data(fd);   // ground cover table, ground colours, macro variation (ground.c)
    vkt_frame_data(fd, (int)R->frame_slot);   // terrain parameters into this slot's buffer (terrain_tex.c)
}

static void save_screenshot(GpuBuffer *buf, u32 w, u32 h) {
    u8 *px = (u8 *)buf->mapped;
    bool bgra = R->swap_format == VK_FORMAT_B8G8R8A8_UNORM || R->swap_format == VK_FORMAT_B8G8R8A8_SRGB;
    for (u32 i = 0; i < w * h; i++) {
        if (bgra) { u8 t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        px[i * 4 + 3] = 255;
    }
    stbi_write_png_compression_level = 1; // stb's default level is very slow for full-screen captures
    if (stbi_write_png(R->screenshot_path, (int)w, (int)h, 4, px, (int)w * 4)) log_info("screenshot saved: %s", R->screenshot_path);
    else log_error("screenshot: failed to write %s", R->screenshot_path);
}

// ------------------------------------------------------------------ frame graph passes

static VkImageLayout g_swap_layout;
static struct {
    FrameResources *f;
    CpuSkin *cpu_skinned;
    SkinPass *skin_main, *skin_vm, *skin_phone;
    bool any_skin_vm;
    VkDeviceSize line_off[2], text_off, ui_voff, ui_ioff;
    FgRes hdr, depth, vm_depth, normal, material, velocity, swap, scene_copy, ldr;
    PostLitInputs lit;
    // Scope picture-in-picture (RSIGHT_SCOPE_PIP while aiming)
    FgRes pip_color, pip_depth;
    u32 pip_size;
    vec4 pip_frustum[6];
    mat4 pip_from_view;     // main view space -> PiP clip
    mat4 pip_view, pip_proj, pip_vp;   // the PiP camera
} F;

static void begin_pass(VkCommandBuffer cmd, VkImageView color, bool clear_color, VkImageView depth, VkImageLayout depth_layout, bool clear_depth) {
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = color;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[0] = R->env.fog_color.x;
    ca.clearValue.color.float32[1] = R->env.fog_color.y;
    ca.clearValue.color.float32[2] = R->env.fog_color.z;
    ca.clearValue.color.float32[3] = 1.0f;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = depth;
    da.imageLayout = depth_layout;
    da.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = R->hdr.w;
    ri.renderArea.extent.height = R->hdr.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = color ? 1 : 0;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = depth ? &da : NULL;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, R->hdr.w, R->hdr.h);
}

static void begin_gbuffer(VkCommandBuffer cmd, VkImageView depth, bool clear) {
    VkRenderingAttachmentInfo ca[3];
    VkImageView views[3] = { fg_view(F.normal), fg_view(F.material), fg_view(F.velocity) };
    for (int i = 0; i < 3; i++) {
        ca[i] = (VkRenderingAttachmentInfo){ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca[i].imageView = views[i];
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    // Sky: normal 0, material roughness 1, velocity 0 (camera-only reprojection reconstructs it).
    ca[1].clearValue.color.float32[0] = 1.0f;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = depth;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = R->hdr.w;
    ri.renderArea.extent.height = R->hdr.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, R->hdr.w, R->hdr.h);
}

static void bind_frame(VkCommandBuffer cmd) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &F.f->frame_set, 0, NULL);
}

static void draw_skinned(VkCommandBuffer cmd, int pass, PipelineId gpu_pipe, PipelineId cpu_pipe, const SkinPass *sp) {
    if (F.cpu_skinned) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[cpu_pipe]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
        Material none;
        memset(&none, 0, sizeof(none));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 3, 1, &R->mat_sets[vkr_material_set(&none)].set, 0, NULL);   // lit.frag: no glTF maps
        draw_cpu_skinned(cmd, F.f, F.cpu_skinned, pass);
    } else if (sp->batch_count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[gpu_pipe]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
        draw_skin_pass(cmd, sp);
    }
}

// G-buffer prepass: depth + normal / material / velocity for every opaque world draw.
static void exec_prepass(VkCommandBuffer cmd, void *u) {
    (void)u;
    begin_gbuffer(cmd, R->depth.view, true);
    bind_frame(cmd);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
    post_push_lit_inputs(cmd, NULL, false);
    vkg_draw_gbuffer(cmd);   // vgeo surfaces first (depth export), the legacy draws test against them
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_PREPASS]);
    draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 0, PIPE_PREPASS);
    vkc_draw_gbuffer(cmd);   // classic instanced vgeo content (r_vgeo 0)
    draw_skinned(cmd, 0, PIPE_PREPASS_SKIN, PIPE_PREPASS, F.skin_main);
    vkCmdEndRendering(cmd);
}

// vgeo (docs/VGEO.md "Foliage support"): the depth of the large static legacy surfaces (walls, level
// brushes) before this frame's vgeo Hi-Z, so pass 2 occlusion counts them; the prepass clears and
// redraws everything after.
static void exec_vgeo_occluders(VkCommandBuffer cmd, void *u) {
    (void)u;
    begin_gbuffer(cmd, R->depth.view, true);
    bind_frame(cmd);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
    post_push_lit_inputs(cmd, NULL, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_PREPASS]);
    ArenaMark mark = arena_mark(&g_engine.frame_arena);
    DrawItem *occ = ARENA_PUSH_N(&g_engine.frame_arena, DrawItem, R->draw_count + 1);
    u32 n = 0;
    for (u32 i = 0; i < R->draw_count; i++) {
        const DrawItem *d = &R->draws[i];
        if (d->object_id || d->mat.color.w < 0.999f || (d->mat.flags & (MAT_SHADOW_ONLY | MAT_VIEWMODEL)) || !R->meshes[d->mesh].used) continue;
        vec3 c;
        float r;
        item_bounds(d, &c, &r);
        if (r >= 2.0f) occ[n++] = *d;
    }
    draw_items(cmd, occ, n, false, true, 0, PIPE_PREPASS);
    arena_pop_to(mark);
    vkCmdEndRendering(cmd);
}

// Lit static opaque (depth from the prepass, LESS_OR_EQUAL: shades each pixel once).
static void exec_opaque(VkCommandBuffer cmd, void *u) {
    (void)u;
    begin_pass(cmd, R->hdr.view, true, R->depth.view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, false);
    bind_frame(cmd);
    post_push_lit_inputs(cmd, &F.lit, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT]);
    draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 0, R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT);
    vkc_draw_lit(cmd);   // classic instanced vgeo content (depth EQUAL to its prepass)
    vkg_draw_lit(cmd);   // vgeo pixels (depth equal to the exported depth)
    vkCmdEndRendering(cmd);
}

// Decals on the static scene; the characters drawn next cover any decal pixel on them.
static void exec_decals(VkCommandBuffer cmd, void *u) {
    (void)u;
    begin_pass(cmd, R->hdr.view, false, R->depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, false);
    vkx_draw_decals(cmd, false);
    vkCmdEndRendering(cmd);
}

// Skinned decals (wounds, blood stains) on the characters just drawn.
static void exec_decals_dynamic(VkCommandBuffer cmd, void *u) {
    (void)u;
    begin_pass(cmd, R->hdr.view, false, R->depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, false);
    vkx_draw_decals(cmd, true);
    vkCmdEndRendering(cmd);
}

static void exec_opaque_dynamic(VkCommandBuffer cmd, void *u) {
    (void)u;
    begin_pass(cmd, R->hdr.view, false, R->depth.view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, false);
    bind_frame(cmd);
    post_push_lit_inputs(cmd, &F.lit, false);
    draw_skinned(cmd, 0, R->r_wireframe->i ? PIPE_SKIN_WIRE : PIPE_SKIN, R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT, F.skin_main);
    vkCmdEndRendering(cmd);
}

static void exec_scene_copy(VkCommandBuffer cmd, void *u) {
    (void)u;
    vkx_copy_scene(cmd);
}

// Transparency: blended items, world lines / text, particles (depth read-only, sampled).
static void exec_transparent(VkCommandBuffer cmd, void *u) {
    (void)u;
    FrameResources *f = F.f;
    begin_pass(cmd, R->hdr.view, false, R->depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, false);
    bind_frame(cmd);
    post_push_lit_inputs(cmd, NULL, true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LIT_BLEND]);
    g_froxel_fog_draws = true;
    draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 1, PIPE_LIT_BLEND);
    g_froxel_fog_draws = false;
    if (R->line_count[0] && F.line_off[0] != (VkDeviceSize)-1) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LINES]);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &F.line_off[0]);
        vkCmdDraw(cmd, R->line_count[0], 1, 0, 0);
        R->stats.draw_calls++;
    }
    if (R->text_count && F.text_off != (VkDeviceSize)-1) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_TEXT]);
        TextureId atlas = R->textures[R->text_atlas].ready ? R->text_atlas : R->white_tex;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[atlas].set, 0, NULL);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &F.text_off);
        vkCmdDraw(cmd, R->text_count, 1, 0, 0);
        R->stats.draw_calls++;
    }
    vkx_draw_particles(cmd);
    draw_precip(cmd, f->frame_set);
    vkCmdEndRendering(cmd);
}

// Precipitation (shaders/precip.vert): rain / snow streaks around the camera + splashes, after the
// particles. Counts scale with the intensity and the quality.
static void draw_precip(VkCommandBuffer cmd, VkDescriptorSet frame_set) {
    const RenderWeather *wx = &R->weather;
    float amount = fmaxf(wx->rain, wx->snowfall);
    if (wx->precip_quality <= 0 || amount < 0.01f || !R->pipes[PIPE_PRECIP]) return;
    static const u32 streaks[4] = { 0, 4000, 9000, 16000 }, splashes[4] = { 0, 300, 700, 1300 };
    int q = DG_CLAMP(wx->precip_quality, 0, 3);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_PRECIP]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &frame_set, 0, NULL);
    PushLit pc;
    memset(&pc, 0, sizeof(pc));
    pc.model = mat4_identity();
    float snow_frac = wx->rain + wx->snowfall > 1e-4f ? wx->snowfall / (wx->rain + wx->snowfall) : 0.0f;
    pc.params = v4(snow_frac, amount, 14.0f, 16.0f);
    u32 n = (u32)((float)streaks[q] * amount);
    if (n) {
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDraw(cmd, 6, n, 0, 0);
        R->stats.draw_calls++;
    }
    u32 ns = (u32)((float)splashes[q] * wx->rain);
    if (ns && R->occl_valid) {
        pc.flags[0] = 1;
        pc.params.y = wx->rain;
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDraw(cmd, 6, ns, 0, 0);
        R->stats.draw_calls++;
    }
}

// Viewmodel G-buffer (own depth): marks first-person pixels (no SSR / motion blur / DoF) and
// writes their velocity for TAA. Then the lit viewmodel + overlay lines.
static void exec_vm_prepass(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkRenderingAttachmentInfo ca[3];
    VkImageView views[3] = { fg_view(F.normal), fg_view(F.material), fg_view(F.velocity) };
    for (int i = 0; i < 3; i++) {
        ca[i] = (VkRenderingAttachmentInfo){ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca[i].imageView = views[i];
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = R->vm_depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = R->hdr.w;
    ri.renderArea.extent.height = R->hdr.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, R->hdr.w, R->hdr.h);
    if (R->vm_draw_count || F.any_skin_vm) {
        bind_frame(cmd);
        post_push_lit_inputs(cmd, NULL, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_PREPASS]);
        draw_items(cmd, R->vm_draws, R->vm_draw_count, false, false, 2, PIPE_PREPASS);
        draw_skinned(cmd, 2, PIPE_PREPASS_SKIN, PIPE_PREPASS, F.skin_vm);
    }
    vkCmdEndRendering(cmd);
}

static void exec_viewmodel(VkCommandBuffer cmd, void *u) {
    (void)u;
    FrameResources *f = F.f;
    begin_pass(cmd, R->hdr.view, false, R->vm_depth.view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, false);
    bind_frame(cmd);
    post_push_lit_inputs(cmd, NULL, false);
    if (R->vm_draw_count || F.any_skin_vm) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT]);
        draw_items(cmd, R->vm_draws, R->vm_draw_count, false, false, 2, R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT);
        // First-person arms (body model), same depth as the viewmodel weapon.
        draw_skinned(cmd, 2, R->r_wireframe->i ? PIPE_SKIN_WIRE : PIPE_SKIN, R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT, F.skin_vm);
    }
    if (R->line_count[1] && F.line_off[1] != (VkDeviceSize)-1) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LINES_OVERLAY]);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &F.line_off[1]);
        vkCmdDraw(cmd, R->line_count[1], 1, 0, 0);
        R->stats.draw_calls++;
    }
    vkCmdEndRendering(cmd);
}

// Scope picture-in-picture: the opaque scene (static + characters + the classic / vgeo content,
// lit forward like the main opaque pass, with the main view's AO / contact / SSR / SSGI through its
// projection) from the objective lens at the scope FOV into a square target; then its decals, the
// sky and fog (post_scope_sky) and its transparency (scope_pip_fx). The local player's own weapon
// is in the viewmodel list and never drawn here; the near plane skips the rest of the body.
static void exec_scope_pip(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = fg_view(F.pip_color);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[0] = R->env.fog_color.x;
    ca.clearValue.color.float32[1] = R->env.fog_color.y;
    ca.clearValue.color.float32[2] = R->env.fog_color.z;
    ca.clearValue.color.float32[3] = 1.0f;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = fg_view(F.pip_depth);
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = F.pip_size;
    ri.renderArea.extent.height = F.pip_size;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, F.pip_size, F.pip_size);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &F.f->frame_set_pip, 0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
    post_push_lit_inputs(cmd, &F.lit, false);   // the main view's (frame.ss_info: sampled through its projection)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LIT]);
    vec4 saved[6];
    memcpy(saved, R->frustum, sizeof(saved));
    memcpy(R->frustum, F.pip_frustum, sizeof(saved));
    draw_items(cmd, R->draws, R->draw_count, false, true, 0, PIPE_LIT);
    memcpy(R->frustum, saved, sizeof(saved));
    draw_skinned(cmd, 0, PIPE_SKIN, PIPE_LIT, F.skin_main);
    vkc_draw_pip(cmd);   // the vgeo scene content, culled / LOD-selected with the PiP projection (docs/LOD.md)
    vkCmdEndRendering(cmd);
}

// Scope PiP decals (bullet holes, blood, scorch; skinned wounds): projected onto the PiP depth.
static void exec_scope_pip_decals(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = fg_view(F.pip_color);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = fg_view(F.pip_depth);
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = F.pip_size;
    ri.renderArea.extent.height = F.pip_size;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, F.pip_size, F.pip_size);
    vkx_draw_decals_pip(cmd, F.f->frame_set_pip, fg_view(F.pip_depth), F.pip_vp, F.pip_view, F.pip_proj, R->sight.objective_pos, F.pip_size, F.pip_size);
    vkCmdEndRendering(cmd);
}

// Scope PiP transparency after its sky / fog (post_scope_sky), as the main transparent pass:
// blended surfaces (glass, ...) with the froxel fog, particles (smoke, sparks, blood, muzzle
// flashes of others ...; soft against the main view's depth, fogged by its froxels) and
// precipitation through the scope camera, depth read-only, so they composite over the sky.
static void exec_scope_pip_fx(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = fg_view(F.pip_color);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = fg_view(F.pip_depth);
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = F.pip_size;
    ri.renderArea.extent.height = F.pip_size;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, F.pip_size, F.pip_size);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &F.f->frame_set_pip, 0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
    post_push_lit_inputs(cmd, NULL, true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LIT_BLEND]);
    {
        vec4 saved[6];
        memcpy(saved, R->frustum, sizeof(saved));
        memcpy(R->frustum, F.pip_frustum, sizeof(saved));
        g_froxel_fog_draws = true;
        draw_items(cmd, R->draws, R->draw_count, false, true, 1, PIPE_LIT_BLEND);
        g_froxel_fog_draws = false;
        memcpy(R->frustum, saved, sizeof(saved));
    }
    vkx_draw_particles_pip(cmd, F.f->frame_set_pip);
    draw_precip(cmd, F.f->frame_set_pip);
    vkCmdEndRendering(cmd);
}

// Phone camera views (src/render/phone_view.c): the scene from another camera into its own
// target, drawn like the scope PiP (opaque static + characters + particles + rain, lit forward,
// no screen-space inputs). The local player's phone, hands and held items are in the viewmodel
// list and never drawn here.
typedef struct AltScene { FgRes color, depth; u32 w, h; vec4 frustum[6]; int slot; } AltScene;
static AltScene g_alt[2];

static void exec_alt_scene(VkCommandBuffer cmd, void *u) {
    const AltScene *a = (const AltScene *)u;
    VkDescriptorSet set = F.f->frame_set_phone[a->slot];
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = fg_view(a->color);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[0] = R->env.fog_color.x;
    ca.clearValue.color.float32[1] = R->env.fog_color.y;
    ca.clearValue.color.float32[2] = R->env.fog_color.z;
    ca.clearValue.color.float32[3] = 1.0f;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = fg_view(a->depth);
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = a->w;
    ri.renderArea.extent.height = a->h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, a->w, a->h);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &set, 0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
    post_push_lit_inputs(cmd, NULL, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LIT]);
    vec4 saved[6];
    memcpy(saved, R->frustum, sizeof(saved));
    memcpy(R->frustum, a->frustum, sizeof(saved));
    draw_items(cmd, R->draws, R->draw_count, false, true, 0, PIPE_LIT);
    memcpy(R->frustum, saved, sizeof(saved));
    draw_skinned(cmd, 3, PIPE_SKIN, PIPE_LIT, F.skin_phone);
    vkx_draw_particles_pip(cmd, set);
    draw_precip(cmd, set);
    vkCmdEndRendering(cmd);
}

// Registers a scene pass for phone view `slot` (its frame UBO = this frame's with the view's
// camera). Returns the pass index (< 0 when disabled).
int render_alt_scene_pass(int slot, const char *name, FgRes color, FgRes depth, u32 w, u32 h, mat4 view, mat4 proj, vec3 cam, FrameData *fd) {
    AltScene *a = &g_alt[slot & 1];
    a->color = color;
    a->depth = depth;
    a->w = w;
    a->h = h;
    a->slot = slot & 1;
    mat4 vp = mat4_mul(proj, view);
    extract_frustum(vp, a->frustum);
    FrameData *pd = (FrameData *)F.f->ubo_phone[a->slot].mapped;
    memcpy(pd, fd, sizeof(FrameData));
    pd->view_proj = vp;
    pd->view = view;
    pd->proj = proj;
    pd->view_proj_nj = pd->prev_view_proj_nj = vp;
    pd->jitter = v4(0, 0, 0, 0);
    pd->post_flags = v4(0, 0, 0, 0);
    pd->post_fog = v4(0, 80, 0, 0);
    pd->fog = v4(R->env.fog_color.x, R->env.fog_color.y, R->env.fog_color.z, R->env.fog_density);
    pd->camera_pos = v4(cam.x, cam.y, cam.z, fd->camera_pos.w);
    pd->screen = v4((float)w, (float)h, 1.0f / (float)w, 1.0f / (float)h);
    int p = fg_pass(name, exec_alt_scene, a);
    fg_write(p, color, FG_COLOR);
    fg_write(p, depth, FG_DEPTH);
    return p;
}

// The scope PiP view for LOD selection (docs/LOD.md "Scopes"): the classic renderer culls it with its
// own projection, characters take the larger of their main-view and PiP pixel sizes.
static struct { bool valid; mat4 vp; vec3 eye, fwd; float tan_half, size; } g_scope_lod;

bool render_scope_cull(mat4 *vp, vec3 *eye, float *px_scale) {
    if (!g_scope_lod.valid || !lod_scope()) return false;
    *vp = g_scope_lod.vp;
    *eye = g_scope_lod.eye;
    *px_scale = g_scope_lod.size / (2.0f * g_scope_lod.tan_half);
    return true;
}

bool render_scope_view(vec3 *eye, vec3 *fwd, float *tan_half, float *size_px) {
    if (!g_scope_lod.valid || !lod_scope()) return false;
    *eye = g_scope_lod.eye;
    *fwd = g_scope_lod.fwd;
    *tan_half = g_scope_lod.tan_half;
    *size_px = g_scope_lod.size;
    return true;
}

// PiP camera + its frame UBO; returns false when no PiP is needed this frame.
static bool setup_scope_pip(FrameData *fd) {
    const RenderSight *s = &R->sight;
    R->pip_fov = 0;
    g_scope_lod.valid = false;
    if (s->kind != RSIGHT_SCOPE_PIP || s->ads_t < 0.02f) return false;
    vec3 fwd = v3_norm(s->fwd), up = v3_norm(s->up);
    if (v3_len2(fwd) < 0.5f) return false;
    // The ocular's angular radius from the eye, magnified: the PiP covers exactly what the lens
    // can show (plus a margin for the chromatic aberration / eye box shift).
    float d = fmaxf(v3_dist(s->lens_pos, R->cam.pos), 0.02f);
    float tan_vm = tanf(render_vm_fov() * 0.5f * DG_DEG2RAD);
    float tan_main = tanf(fmaxf(R->cam.fov_deg, 10.0f) * 0.5f * DG_DEG2RAD);
    float t_lens = (s->lens_radius / d) * tan_main / fmaxf(tan_vm, 1e-3f);   // the lens (weapon projection) in main-view tangents
    // Shown magnified by k against the main view (post_sights.c): the zoom is against the base FOV, the
    // main view may already be narrowed by the ADS world zoom.
    float tan_base = tanf(fmaxf(s->base_fov > 1.0f ? s->base_fov : R->cam.fov_deg, 10.0f) * 0.5f * DG_DEG2RAD);
    float k = fmaxf(s->zoom, 1.0f) * tan_main / fmaxf(tan_base, 1e-4f);
    float tan_half = clampf(1.15f * t_lens / fmaxf(k, 1.0f), tanf(0.25f * DG_DEG2RAD), tanf(30.0f * DG_DEG2RAD));
    R->pip_fov = 2.0f * atanf(tan_half) * DG_RAD2DEG;
    u32 n = (u32)DG_CLAMP((int)lrintf((float)R->hdr.h * clampf(s->pip_scale, 0.2f, 2.0f)), 128, 2048);
    F.pip_size = n;
    mat4 view = mat4_look_at(s->objective_pos, v3_add(s->objective_pos, fwd), up);
    mat4 proj = mat4_perspective(2.0f * atanf(tan_half), 1.0f, 0.3f, R->cam.zfar);
    mat4 vp = mat4_mul(proj, view);
    extract_frustum(vp, F.pip_frustum);
    F.pip_from_view = mat4_mul(vp, mat4_inverse_rigid(R->view));
    g_scope_lod.valid = true;
    g_scope_lod.vp = vp;
    g_scope_lod.eye = s->objective_pos;
    g_scope_lod.fwd = fwd;
    g_scope_lod.tan_half = tan_half;
    g_scope_lod.size = (float)n;
    // The scope sees the world exactly like the main view (docs/ATTACHMENTS.md "The PiP pass"): the
    // same lighting, weather, shadows and fog settings (frame.fog, post_fog: the froxels when the main
    // view has them), and the main view's screen-space inputs (AO, contact shadows, SSR, SSGI) and
    // froxel volume looked up through the main projection (ss_info). Only the camera and the target
    // differ.
    FrameData *pd = (FrameData *)F.f->ubo_pip.mapped;
    memcpy(pd, fd, sizeof(FrameData));
    pd->view_proj = vp;
    pd->view = view;
    pd->proj = proj;
    pd->view_proj_nj = pd->prev_view_proj_nj = vp;
    pd->jitter = v4(0, 0, 0, 0);
    pd->ss_view_proj = mat4_mul(R->proj_j, R->view);
    pd->ss_view = R->view;
    pd->ss_info = v4(1, 0, 0, 0);
    pd->camera_pos = v4(s->objective_pos.x, s->objective_pos.y, s->objective_pos.z, fd->camera_pos.w);
    pd->screen = v4((float)n, (float)n, 1.0f / (float)n, 1.0f / (float)n);
    F.pip_view = view;
    F.pip_proj = proj;
    F.pip_vp = vp;
    return true;
}

// Final: post encode (CAS, grain, dither, sRGB / PQ) + UI into the swapchain image.
static void exec_final(VkCommandBuffer cmd, void *u) {
    (void)u;
    FrameResources *f = F.f;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = R->swap_views[R->image_index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = R->swap_extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, R->swap_extent.width, R->swap_extent.height);
    post_final_draw(cmd, F.ldr);
    if (R->ui_ccount && F.ui_voff != (VkDeviceSize)-1 && F.ui_ioff != (VkDeviceSize)-1) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_UI]);
        float scr[4] = { 2.0f / (float)R->swap_extent.width, 2.0f / (float)R->swap_extent.height, 1.0f / (float)R->swap_extent.width,
                         1.0f / (float)R->swap_extent.height };
        vkCmdPushConstants(cmd, R->layout_ui, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(scr), scr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &F.ui_voff);
        vkCmdBindIndexBuffer(cmd, f->dyn.buffer, F.ui_ioff, VK_INDEX_TYPE_UINT32);
        u32 bound = 0;
        for (u32 i = 0; i < R->ui_ccount; i++) {
            UiDrawCmd *c = &R->ui_cmds[i];
            if (!c->index_count) continue;
            TextureId tex = c->texture && R->textures[c->texture].used && R->textures[c->texture].ready ? c->texture : R->white_tex;
            if (tex != bound) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_ui, 0, 1, &R->textures[tex].set, 0, NULL);
                bound = tex;
            }
            int x0 = DG_CLAMP(c->clip_x, 0, (int)R->swap_extent.width);
            int y0 = DG_CLAMP(c->clip_y, 0, (int)R->swap_extent.height);
            int x1 = DG_CLAMP(c->clip_x + c->clip_w, 0, (int)R->swap_extent.width);
            int y1 = DG_CLAMP(c->clip_y + c->clip_h, 0, (int)R->swap_extent.height);
            if (x1 <= x0 || y1 <= y0) continue;
            VkRect2D sc = { { x0, y0 }, { (u32)(x1 - x0), (u32)(y1 - y0) } };
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDrawIndexed(cmd, c->index_count, 1, c->index_offset, 0, 0);
            R->stats.draw_calls++;
        }
    }
    vkCmdEndRendering(cmd);
}

// Builds this frame's graph:
//   prepass -> [hi-z, contact, GTAO, SSGI, SSR, froxels] -> opaque -> decals -> opaque_dynamic ->
//   fog_apply -> scene_copy -> transparent -> [colour pyramid, depth history] -> vm_prepass ->
//   viewmodel -> [AA ... tonemap] -> final + UI
static void build_frame_graph(FrameData *fd) {
    fg_begin(R->hdr.w, R->hdr.h, R->swap_extent.width, R->swap_extent.height);
    F.hdr = fg_import("hdr", R->hdr.image, R->hdr.view, R->hdr.format, R->hdr.w, R->hdr.h, VK_IMAGE_ASPECT_COLOR_BIT, &R->hdr.layout);
    F.depth = fg_import("depth", R->depth.image, R->depth.view, R->depth.format, R->depth.w, R->depth.h, VK_IMAGE_ASPECT_DEPTH_BIT, &R->depth.layout);
    F.vm_depth = fg_import("vm_depth", R->vm_depth.image, R->vm_depth.view, R->vm_depth.format, R->vm_depth.w, R->vm_depth.h, VK_IMAGE_ASPECT_DEPTH_BIT,
                           &R->vm_depth.layout);
    g_swap_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    F.swap = fg_import("swapchain", R->swap_images[R->image_index], R->swap_views[R->image_index], R->swap_format, R->swap_extent.width,
                       R->swap_extent.height, VK_IMAGE_ASPECT_COLOR_BIT, &g_swap_layout);
    RenderTarget *sc = vkx_scene_copy();
    F.scene_copy = sc ? fg_import("hdr_opaque", sc->image, sc->view, sc->format, sc->w, sc->h, VK_IMAGE_ASPECT_COLOR_BIT, &sc->layout) : FG_NONE;
    FgTexDesc gd = { "gb_normal", GB_NORMAL_FORMAT, FG_SIZE_RENDER, 0, 0, 0, 1, 0 };
    F.normal = fg_texture(&gd);
    gd.name = "gb_material";
    gd.format = GB_MATERIAL_FORMAT;
    F.material = fg_texture(&gd);
    // Velocity only when something reads it (TAA / TAA-U, motion blur, temporal AO / SSR / SSGI):
    // otherwise the G-buffer's third attachment is a null view and its writes are discarded.
    F.velocity = FG_NONE;
    if (post_needs_velocity()) {
        gd.name = "gb_velocity";
        gd.format = GB_VELOCITY_FORMAT;
        F.velocity = fg_texture(&gd);
    }

    PostFrameIn in;
    memset(&in, 0, sizeof(in));
    in.hdr = F.hdr;
    in.depth = F.depth;
    in.normal = F.normal;
    in.material = F.material;
    in.velocity = F.velocity;
    in.hdr_opaque = F.scene_copy;
    in.vm_depth = F.vm_depth;
    in.swap = F.swap;
    in.render_w = R->hdr.w;
    in.render_h = R->hdr.h;
    in.output_w = R->swap_extent.width;
    in.output_h = R->swap_extent.height;
    in.view = R->view;
    in.proj = R->proj;   // unjittered: the post shaders add / remove the jitter themselves
    in.view_proj = fd->view_proj;
    in.inv_view_proj = mat4_inverse(fd->view_proj);
    in.view_proj_nj = R->view_proj_nj;
    in.prev_view_proj_nj = R->prev_view_proj_nj;
    in.inv_proj = mat4_inverse(R->proj);
    in.cam_pos = R->cam.pos;
    in.jitter = R->jitter;
    in.prev_jitter = R->prev_jitter;
    in.znear = R->cam.znear;
    in.zfar = R->cam.zfar;
    in.fov_y = R->cam.fov_deg * DG_DEG2RAD;
    in.sun_dir = v3_norm(R->env.sun_dir);
    in.sun_color = R->env.sun_color;
    in.sun_intensity = R->env.sun_intensity;
    in.sky = R->env.sky_ambient;
    in.ground = R->env.ground_ambient;
    in.ambient = R->env.ambient_intensity;
    in.dt = (float)g_engine.frame_dt;
    in.time = (float)g_engine.real_time;
    in.frame = R->frame_index;
    in.camera_cut = R->camera_cut;
    in.sky_on = R->env.sky;
    in.sky_zenith = R->env.sky_zenith;
    in.sky_horizon = R->env.sky_horizon;
    in.clouds = R->env.clouds;
    in.ads = R->fx_ads;
    in.ads_focus_dist = R->fx_ads_focus;
    post_begin_frame(&in);
    sky_build();   // atmosphere LUTs, sky view, aerial perspective, cloud shadow, environment refilter

    vkg_graph_cull(F.depth);   // vgeo culling (docs/VGEO.md), against last frame's Hi-Z
    int p;
    if (vkg_wants_occluders()) {   // static legacy occluders' depth: vgeo pass 2 occlusion counts walls
        p = fg_pass("vgeo_occluders", exec_vgeo_occluders, NULL);
        fg_write(p, F.depth, FG_DEPTH);
        fg_write(p, F.normal, FG_COLOR);
        fg_write(p, F.material, FG_COLOR);
        fg_write(p, F.velocity, FG_COLOR);
    }
    vkg_graph_cull_pass2(F.depth);
    p = fg_pass("prepass", exec_prepass, NULL);
    fg_write(p, F.depth, FG_DEPTH);
    fg_write(p, F.normal, FG_COLOR);
    fg_write(p, F.material, FG_COLOR);
    fg_write(p, F.velocity, FG_COLOR);
    vkg_graph_hiz(F.depth, F.velocity);    // this frame's occlusion Hi-Z

    post_screen_space(&F.lit);
    const PostSettings *ps = post_settings();
    fd->post_flags = v4(F.lit.ao_on ? 1.0f : 0.0f, F.lit.contact_on ? 1.0f : 0.0f, F.lit.ssr_on ? 1.0f : 0.0f, F.lit.ssgi_on ? 1.0f : 0.0f);
    fd->post_params = v4(ps->ssr_intensity, ps->ssgi_intensity, ps->ao_intensity, 1.0f);

    p = fg_pass("opaque", exec_opaque, NULL);
    fg_write(p, F.hdr, FG_COLOR);
    fg_write(p, F.depth, FG_DEPTH);
    if (F.lit.ao_on) fg_read(p, F.lit.ao, FG_SAMPLED);
    if (F.lit.contact_on) fg_read(p, F.lit.contact, FG_SAMPLED);
    if (F.lit.ssr_on) fg_read(p, F.lit.ssr, FG_SAMPLED);
    if (F.lit.ssgi_on) fg_read(p, F.lit.ssgi, FG_SAMPLED);
    if (p < 0) log_warn("render: opaque pass disabled (r_post_disable)");

    p = fg_pass("decals", exec_decals, NULL);
    fg_write(p, F.hdr, FG_COLOR);
    fg_read(p, F.depth, FG_DEPTH_READ);

    p = fg_pass("opaque_dynamic", exec_opaque_dynamic, NULL);
    fg_write(p, F.hdr, FG_COLOR);
    fg_write(p, F.depth, FG_DEPTH);
    if (F.lit.ao_on) fg_read(p, F.lit.ao, FG_SAMPLED);
    if (F.lit.contact_on) fg_read(p, F.lit.contact, FG_SAMPLED);
    if (F.lit.ssr_on) fg_read(p, F.lit.ssr, FG_SAMPLED);
    if (F.lit.ssgi_on) fg_read(p, F.lit.ssgi, FG_SAMPLED);

    p = fg_pass("decals_dynamic", exec_decals_dynamic, NULL);
    fg_write(p, F.hdr, FG_COLOR);
    fg_read(p, F.depth, FG_DEPTH_READ);

    post_sky();
    post_fog_apply();

    vkw_build_graph(F.hdr, F.depth, F.velocity);   // water surfaces (part of the opaque scene for TAA / particles)

    if (F.scene_copy != FG_NONE) {
        p = fg_pass("scene_copy", exec_scene_copy, NULL);
        fg_write(p, F.hdr, FG_COLOR);
        fg_write(p, F.scene_copy, FG_SAMPLED);
        if (p < 0) F.scene_copy = FG_NONE;
    }

    p = fg_pass("transparent", exec_transparent, NULL);
    fg_write(p, F.hdr, FG_COLOR);
    fg_read(p, F.depth, FG_DEPTH_READ);
    fg_read(p, F.scene_copy, FG_SAMPLED);
    fg_read(p, post_froxels(), FG_SAMPLED);
    fd->post_fog = v4(post_froxels() >= 0 ? 1.0f : 0.0f, 80.0f, 0.0f, 0.0f);

    post_capture_scene(F.hdr);

    p = fg_pass("vm_prepass", exec_vm_prepass, NULL);
    fg_write(p, F.vm_depth, FG_DEPTH);
    fg_write(p, F.normal, FG_COLOR);
    fg_write(p, F.material, FG_COLOR);
    fg_write(p, F.velocity, FG_COLOR);
    p = fg_pass("viewmodel", exec_viewmodel, NULL);
    fg_write(p, F.hdr, FG_COLOR);
    fg_write(p, F.vm_depth, FG_DEPTH);

    F.pip_color = F.pip_depth = FG_NONE;
    if (setup_scope_pip(fd)) {
        FgTexDesc pd = { "scope_pip", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, F.pip_size, F.pip_size, 1, 1, 0 };
        F.pip_color = fg_texture(&pd);
        pd.name = "scope_pip_depth";
        pd.format = R->depth.format;
        F.pip_depth = fg_texture(&pd);
        p = fg_pass("scope_pip", exec_scope_pip, NULL);
        fg_write(p, F.pip_color, FG_COLOR);
        fg_write(p, F.pip_depth, FG_DEPTH);
        // The main view's screen-space inputs (kept alive until here).
        if (F.lit.ao_on) fg_read(p, F.lit.ao, FG_SAMPLED);
        if (F.lit.contact_on) fg_read(p, F.lit.contact, FG_SAMPLED);
        if (F.lit.ssr_on) fg_read(p, F.lit.ssr, FG_SAMPLED);
        if (F.lit.ssgi_on) fg_read(p, F.lit.ssgi, FG_SAMPLED);
        if (p < 0) F.pip_color = FG_NONE;
        else {
            p = fg_pass("scope_pip_decals", exec_scope_pip_decals, NULL);
            fg_write(p, F.pip_color, FG_COLOR);
            fg_read(p, F.pip_depth, FG_DEPTH_READ);
            post_scope_sky(F.pip_color, F.pip_depth, mat4_inverse(F.pip_vp), R->sight.objective_pos);
            vkw_build_graph_pip(F.pip_color, F.pip_depth, F.pip_vp);   // water, after the fog (as the main view)
            p = fg_pass("scope_pip_fx", exec_scope_pip_fx, NULL);
            fg_write(p, F.pip_color, FG_COLOR);
            fg_read(p, F.pip_depth, FG_DEPTH_READ);
            fg_read(p, F.depth, FG_SAMPLED);               // soft particles: the main view's depth
            fg_read(p, post_froxels(), FG_SAMPLED);        // blended surfaces / particles: its froxels
        }
    }
    post_set_sight(&R->sight, F.pip_color, F.pip_from_view, R->vm_proj, R->pip_fov);
    phone_view_build(fd);   // phone camera views (sampled from the next frame on)

    F.ldr = post_resolve();
    render_ui_build(F.ldr, R->swap_extent.width, R->swap_extent.height);

    p = fg_pass("final", exec_final, NULL);
    fg_read(p, F.ldr, FG_SAMPLED);
    render_ui_final_reads(p);
    fg_write(p, F.swap, FG_COLOR);
    if (p < 0) {
        static int warned;
        if (!warned++) log_warn("render: the final pass can't be disabled (black frame)");
    }
}

// The sun's casters into one cascade tile (k 0..3) or the moon map (k 4): raster draws (culled
// against the tile's light box, Cast Shadows / culling mask in draw_items), vgeo / classic
// instances, skinned characters (batched; per draw when the culling mask excludes a layer).
static void draw_sun_casters(VkCommandBuffer cmd, FrameResources *f, int k, const SkinPass *skin_shadow) {
    const LightShadowSettings *sl = &R->sun_shadow;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
    vkCmdSetDepthBias(cmd, 1.25f * sl->bias, 0.0f, 1.75f * sl->bias);   // Unity's Bias scales the hardware terms too
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
    draw_items(cmd, R->draws, R->draw_count, true, false, 0, PIPE_SHADOW);
    vkg_draw_shadow(cmd, (u32)k);
    if (k < 4) vkc_draw_shadow(cmd, (u32)k);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
    if (F.cpu_skinned) draw_cpu_skinned(cmd, f, F.cpu_skinned, 1);
    else if (skin_shadow->batch_count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SKIN_SHADOW]);
        PushLit pc;
        memset(&pc, 0, sizeof(pc));
        pc.flags[3] = g_shadow_cascade;
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        if ((g_shadow_layers & SHADOW_LAYERS_ALL) == SHADOW_LAYERS_ALL) {
            draw_skin_pass_f(cmd, skin_shadow, 0);
            bool any_fade = false;
            for (u32 b = 0; b < skin_shadow->batch_count; b++) any_fade |= skin_shadow->batches[b].fade;
            if (any_fade) {   // LOD / distance cross-fading casters, dithered (docs/LOD.md)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SKIN_SHADOW_FADE]);
                draw_skin_pass_f(cmd, skin_shadow, 1);
            }
        } else {
            int bound = 0;   // 0 opaque casters, 1 fading casters (PIPE_SKIN_SHADOW_FADE, docs/LOD.md)
            for (u32 i = 0; i < R->skin_draw_count; i++) {
                const SkinnedDraw *d = &R->skin_draws[i];
                if (g_skin_shadow_inst[i] == ~0u || !((g_shadow_layers >> (d->layer & 7)) & 1u)) continue;
                int want = d->fade != 0.0f ? 1 : 0;
                if (want != bound) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[want ? PIPE_SKIN_SHADOW_FADE : PIPE_SKIN_SHADOW]);
                    bound = want;
                }
                GpuMesh *m = &R->meshes[d->mesh];
                VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m->vb.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, g_skin_shadow_inst[i]);
            }
        }
    }
}

void render_end_frame(void) {
    if (!R->frame_active) return;
    write_cookie_binding();
    write_weather_bindings();
    vks_write_bindings();
    vkt_write_bindings();
    FrameResources *f = &R->frames[R->frame_slot];
    VkCommandBuffer cmd = f->cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    u32 slot = R->frame_slot;
    if (g_frame_qp) {
        u64 ts[2];
        if (g_frame_qp_written[slot] && vkGetQueryPoolResults(R->device, g_frame_qp, slot * 2, 2, sizeof(ts), ts, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            g_gpu_frame_ms = (float)(ts[1] - ts[0]) * R->gpu_props.limits.timestampPeriod * 1e-6f;
        vkCmdResetQueryPool(cmd, g_frame_qp, slot * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_frame_qp, slot * 2);
    }

    vkr_record_uploads(cmd);

    FrameData *fd = (FrameData *)f->ubo.mapped;
    compute_matrices(fd);
    vkg_submit_fallback();   // the classic renderer's culling / LOD / instance batches (r_vgeo 0; r_vgeo 1: local light tiles only)
    vkg_bench_frame();
    memset(&F, 0, sizeof(F));
    F.f = f;

    // Previous transforms of moving draws (DrawPrev SSBO, velocity).
    R->prev_model_count = 0;
    for (u32 i = 0; i < R->draw_count; i++) assign_prev_model(&R->draws[i]);
    for (u32 i = 0; i < R->vm_draw_count; i++) assign_prev_model(&R->vm_draws[i]);
    if (R->prev_model_count) memcpy(f->prev_models.mapped, R->prev_models_cpu, sizeof(mat4) * R->prev_model_count);

    for (int k = 0; k < 2; k++)
        if (R->line_count[k]) F.line_off[k] = dyn_push(f, R->line_cpu[k], sizeof(LineVertex) * R->line_count[k]);
    F.text_off = R->text_count ? dyn_push(f, R->text_cpu, sizeof(TextVertex) * R->text_count) : 0;
    {
        static int warned;
        if (F.text_off == (VkDeviceSize)-1 && !warned++)
            log_warn("render: dynamic buffer full (%llu used of %llu, lines %u/%u, text %u verts) - 3D text dropped", (unsigned long long)f->dyn_used,
                     (unsigned long long)f->dyn.size, R->line_count[0], R->line_count[1], R->text_count);
    }
    F.ui_voff = R->ui_vcount ? dyn_push(f, R->ui_verts, sizeof(UiVertex) * R->ui_vcount) : 0;
    F.ui_ioff = R->ui_icount ? dyn_push(f, R->ui_indices, sizeof(u32) * R->ui_icount) : 0;

    // Skinned characters: palettes + per-pass instance lists (GPU) or CPU-skinned vertices.
    // The bone SSBO holds this frame's palettes, then last frame's (previous-palette velocity).
    bool cpu_skin = R->r_cpu_skinning->i != 0;
    g_skin_shadow_gpu = false;
    static SkinPass skin_main, skin_shadow, skin_vm, skin_phone;
    skin_main.batch_count = skin_shadow.batch_count = skin_vm.batch_count = skin_phone.batch_count = 0;
    F.skin_main = &skin_main;
    F.skin_vm = &skin_vm;
    F.skin_phone = &skin_phone;
    for (u32 i = 0; i < R->skin_draw_count; i++) F.any_skin_vm |= (R->skin_draws[i].flags & MAT_VIEWMODEL) != 0;
    ArenaMark skin_mark = arena_mark(&g_engine.frame_arena);
    memset(g_skin_routed, 0, R->skin_draw_count);
    if (R->skin_draw_count && !cpu_skin) vkg_route_skinned(R->skin_draws, R->skin_draw_count, R->skin_bones, g_skin_routed);
    if (R->skin_draw_count) {
        if (cpu_skin) {
            F.cpu_skinned = ARENA_PUSH_N(&g_engine.frame_arena, CpuSkin, R->skin_draw_count);
            cpu_skin_all(f, F.cpu_skinned);
        } else {
            memcpy(f->bones.mapped, R->skin_bones, sizeof(mat4) * R->skin_bone_count);
            if (R->prev_bone_count && R->skin_bone_count + R->prev_bone_count <= MAX_SKIN_BONES * 2)
                memcpy((mat4 *)f->bones.mapped + R->skin_bone_count, R->prev_bones, sizeof(mat4) * R->prev_bone_count);
            memset(g_skin_shadow_inst, 0xFF, sizeof(u32) * R->skin_draw_count);
            g_skin_shadow_gpu = true;
            u32 used = build_skin_pass(f, 0, 0, &skin_main);
            used += build_skin_pass(f, 1, used, &skin_shadow);
            used += build_skin_pass(f, 2, used, &skin_vm);
            if (phone_view_pending()) build_skin_pass(f, 3, used, &skin_phone);
        }
    }

    // ---------------------------------------------------------- GPU particle simulation
    vkx_begin_frame_queries(cmd);
    vkx_record_compute(cmd);

    // ---------------------------------------------------------- shadow pass
    R->shadow_drawn = 0;
    vkg_record_shadow_cull(cmd);   // vgeo cascade views (docs/VGEO.md)
    vkr_image_barrier(cmd, R->shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 1);
    {
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = R->shadow.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea.extent.width = R->shadow.w;
        ri.renderArea.extent.height = R->shadow.h;
        ri.layerCount = 1;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        if (R->shadow_active[0] || R->shadow_active[1] || R->shadow_active[2] || R->shadow_active[3]) {
            // One atlas tile per cascade (the cascade index rides in the push constants). The
            // cascade matrices already map into their tile: full viewport, scissored to the tile.
            // Tiles the sky does not need are skipped (compute_matrices: the sun's type None, under
            // the cutoff, wide mode's cascade 0).
            u32 tile = R->shadow.w / 2;
            g_shadow_min_radius = R->r_shadow_min_radius->f;
            g_shadow_cull = R->r_culling->i != 0;
            g_shadow_layers = R->sun_shadow.culling_mask;
            for (int k = 0; k < 4; k++) {
                if (!R->shadow_active[k]) continue;
                extract_frustum(R->shadow_vp_ndc[k], g_shadow_planes);   // casters outside this tile's light box are skipped
                R->shadow_drawn |= 1u << k;
                VkViewport vp = { 0.0f, 0.0f, (float)R->shadow.w, (float)R->shadow.h, 0.0f, 1.0f };
                VkRect2D sc = { { (int)((k & 1) * tile), (int)((k >> 1) * tile) }, { tile, tile } };
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                g_shadow_cascade = (u32)k;
                draw_sun_casters(cmd, f, k, &skin_shadow);
            }
            g_shadow_cascade = 0;
            g_shadow_min_radius = 0.0f;
            g_shadow_cull = false;
            g_shadow_layers = SHADOW_LAYERS_ALL;
        }
        vkCmdEndRendering(cmd);
    }
    vkr_image_barrier(cmd, R->shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);

    // ---------------------------------------------------------- moon shadow map
    // The moon while the sun still owns the cascades (its own map since the atlas holds 4 cascades):
    // cleared every frame, drawn only while its fade is above 0.
    vkr_image_barrier(cmd, R->moon_shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 1);
    {
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = R->moon_shadow.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea.extent.width = R->moon_shadow.w;
        ri.renderArea.extent.height = R->moon_shadow.h;
        ri.layerCount = 1;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        if (R->moon_active) {
            set_viewport(cmd, R->moon_shadow.w, R->moon_shadow.h);
            g_shadow_min_radius = R->r_shadow_min_radius->f;
            g_shadow_cull = R->r_culling->i != 0;
            g_shadow_layers = R->sun_shadow.culling_mask;
            extract_frustum(R->shadow_vp_ndc[4], g_shadow_planes);
            g_shadow_cascade = SHADOW_PASS_MOON;
            draw_sun_casters(cmd, f, 4, &skin_shadow);
            g_shadow_cascade = 0;
            g_shadow_min_radius = 0.0f;
            g_shadow_cull = false;
            g_shadow_layers = SHADOW_LAYERS_ALL;
            R->shadow_drawn |= 1u << 5;
        }
        vkCmdEndRendering(cmd);
    }
    vkr_image_barrier(cmd, R->moon_shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);

    // ---------------------------------------------------------- viewmodel shadow tile
    // The first-person rig (arms, held item, attachments) alone, in its own map: cleared every frame
    // (an inactive tile reads lit), drawn with this frame's matrices and palettes.
    vkr_image_barrier(cmd, R->vm_shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 1);
    {
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = R->vm_shadow.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea.extent.width = R->vm_shadow.w;
        ri.renderArea.extent.height = R->vm_shadow.h;
        ri.layerCount = 1;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        if (R->shadow_vm_active) {
            set_viewport(cmd, R->vm_shadow.w, R->vm_shadow.h);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
            vkCmdSetDepthBias(cmd, 1.0f, 0.0f, 1.5f);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
            g_shadow_cascade = SHADOW_PASS_VM;
            g_shadow_vm_pass = true;
            draw_items(cmd, R->vm_draws, R->vm_draw_count, true, false, 0, PIPE_SHADOW);
            if (F.cpu_skinned) draw_cpu_skinned(cmd, f, F.cpu_skinned, 2);
            else if (skin_vm.batch_count) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SKIN_SHADOW]);
                PushLit pc;
                memset(&pc, 0, sizeof(pc));
                pc.flags[3] = SHADOW_PASS_VM;
                vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
                draw_skin_pass(cmd, &skin_vm);
            }
            g_shadow_vm_pass = false;
            g_shadow_cascade = 0;
            R->shadow_drawn |= 1u << 4;
        }
        vkCmdEndRendering(cmd);
    }
    vkr_image_barrier(cmd, R->vm_shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
    shadow_check_sampled(fd);

    // ---------------------------------------------------------- point / spot light shadows
    vkls_record(cmd, f);

    // ---------------------------------------------------------- rain occlusion map (weather)
    // Static + dynamic opaque draws from straight above, depth only (cascade 3 of the shadow
    // shaders = frame.occl_vp). Re-rendered when the weather asks (the player moved / periodically).
    if (!R->occl_valid && R->occl.image && R->occl.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        vkr_image_barrier(cmd, R->occl.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
        R->occl.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (R->weather.occlusion_update && R->occl.image && R->weather.occlusion_extent > 1.0f) {
        vkr_image_barrier(cmd, R->occl.image, VK_IMAGE_ASPECT_DEPTH_BIT, R->occl.layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 1);
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = R->occl.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea.extent.width = R->occl.w;
        ri.renderArea.extent.height = R->occl.h;
        ri.layerCount = 1;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        set_viewport(cmd, R->occl.w, R->occl.h);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
        vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
        g_shadow_cascade = SHADOW_PASS_OCCL;
        draw_items(cmd, R->draws, R->draw_count, true, false, 0, PIPE_SHADOW);
        g_shadow_cascade = 0;
        vkCmdEndRendering(cmd);
        vkr_image_barrier(cmd, R->occl.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
        R->occl.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        R->occl_valid = true;
    }

    // ---------------------------------------------------------- frame graph: scene + post + UI
    build_frame_graph(fd);
    fg_execute(cmd);

    // Depth stays readable for next frame's particle collision compute (VFX set binding 19).
    if (R->depth.layout != VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL) {
        vkr_image_barrier(cmd, R->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, R->depth.layout, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
        R->depth.layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }

    VkImage swap = R->swap_images[R->image_index];
    GpuBuffer shot = { 0 };
    bool want_shot = R->screenshot_path[0] != 0;
    if (want_shot) {
        u32 w = R->swap_extent.width, h = R->swap_extent.height;
        shot = vkr_buffer_create_readback((VkDeviceSize)w * h * 4);
        vkr_image_barrier(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, g_swap_layout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 1);
        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = w;
        region.imageExtent.height = h;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer, 1, &region);
        vkr_image_barrier(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0, 1);
    } else {
        vkr_image_barrier(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, g_swap_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_NONE, 0, 1);
    }

    arena_pop_to(skin_mark);
    if (g_frame_qp) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_frame_qp, slot * 2 + 1);
        g_frame_qp_written[slot] = true;
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = f->image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = R->render_done[R->image_index];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(R->queue, 1, &si, f->fence));

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &R->render_done[R->image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &R->swapchain;
    pi.pImageIndices = &R->image_index;
    VkResult pres = vkQueuePresentKHR(R->queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) R->swap_dirty = true;
    else if (pres != VK_SUCCESS) log_error("present failed %d", (int)pres);

    bool want_exr = post_exr_capture_armed();
    bool want_ui = render_ui_wants_wait();
    if (want_shot || want_exr || want_ui) VK_CHECK(vkWaitForFences(R->device, 1, &f->fence, VK_TRUE, UINT64_MAX));
    render_ui_frame_done();
    if (want_shot) {
        vmaInvalidateAllocation(R->vma, shot.alloc, 0, VK_WHOLE_SIZE);
        save_screenshot(&shot, R->swap_extent.width, R->swap_extent.height);
        vkr_buffer_destroy_now(&shot);
        R->screenshot_path[0] = 0;
    }
    if (want_exr) post_exr_finish();

    // Temporal bookkeeping for the next frame: models, palettes, matrices.
    store_prev_models(R->draws, R->draw_count);
    store_prev_models(R->vm_draws, R->vm_draw_count);
    if (!cpu_skin) {
        memcpy(R->prev_bones, R->skin_bones, sizeof(mat4) * R->skin_bone_count);
        R->prev_bone_count = R->skin_bone_count;
        for (u32 i = 0; i < R->skin_draw_count; i++) {
            const SkinnedDraw *d = &R->skin_draws[i];
            if (!d->object_id) continue;
            ObjSlot *s = obj_find(d->object_id, true);
            if (!s) continue;
            s->pal_offset = d->bone_count & 0xFFFFFu;
            s->pal_count = d->bone_count >> 20;
            s->pal_frame = R->frame_index + 1;
        }
    }
    R->have_prev = true;
    R->camera_cut = false;
    R->frame_index++;

    R->frame_slot = (R->frame_slot + 1) % FRAMES_IN_FLIGHT;
    R->frame_active = false;
}

void render_screenshot(const char *path) { dg_strcpy(R->screenshot_path, sizeof(R->screenshot_path), path); }

void render_set_camera_fx(bool ads, float ads_focus_dist) {
    R->fx_ads = ads;
    R->fx_ads_focus = ads_focus_dist;
}
void render_camera_cut(void) { R->camera_cut = true; }
void render_set_sight(const RenderSight *s) { R->sight = *s; }
const RenderSight *render_sight(void) { return &R->sight; }
float render_scope_pip_fov(void) { return R->pip_fov; }
vec3 render_sky_irradiance(vec3 dir) { return sky_irradiance_cpu(dir); }
float render_sky_luminance(float *latest) { return sky_sh_luminance(latest); }
u32 render_sky_refilters(void) { return sky_refilter_count(); }
bool render_load_lut(const char *cube_path) {
    vkDeviceWaitIdle(R->device);   // the LUT image may be in flight
    return post_lut_load_cube(cube_path && cube_path[0] ? cube_path : NULL, 0);
}
bool render_capture_hdr(const char *exr_path) { return post_hdr_capture(exr_path); }
int render_pass_count(void) { return fg_pass_count(); }
const char *render_pass_name(int i) { return fg_pass_name(i); }
float render_pass_ms(int i) { return fg_pass_ms(i); }
u64 render_post_memory(void) { return fg_memory_bytes(); }
float render_gpu_frame_ms(void) { return g_gpu_frame_ms; }
float render_direct_exposure_ev(void) {
    float e = post_direct_exposure();
    return e > 0.0f ? log2f(0.18f / (e * 0.125f)) + post_settings()->exposure_comp : 0.0f;
}
u64 render_gpu_memory_bytes(void) {
    VmaTotalStatistics st;
    vmaCalculateStatistics(R->vma, &st);
    return st.total.statistics.allocationBytes;
}
void render_graph_trim(void) {
    fg_trim();
    R->camera_cut = true;   // the histories were dropped with the pool's unused images
}
void render_graph_report(RenderGraphReport *out) {
    const FgReport *r = fg_last_report();
    memset(out, 0, sizeof(*out));
    out->passes = r->passes;
    out->post_passes = r->post_passes;
    out->dead_passes = r->dead_passes;
    out->unused_res = r->unused_res;
    out->undefined_reads = r->undefined_reads;
    out->pool_images = r->pool_images;
    out->stale_images = r->stale_images;
    out->pool_bytes = r->pool_bytes;
    out->stale_bytes = r->stale_bytes;
    for (int i = 0; i < r->passes && i < 96; i++) out->names[i] = r->names[i];
    for (int i = 0; i < r->post_passes && i < 32; i++) out->post[i] = r->post[i];
    for (int i = 0; i < r->dead_passes && i < 12; i++) out->dead[i] = r->dead[i];
    for (int i = 0; i < r->unused_res && i < 12; i++) out->unused[i] = r->unused[i];
    for (int i = 0; i < r->undefined_reads && i < 12; i++) out->undefined[i] = r->undefined[i];
}
float render_exposure_ev(void) { return post_exposure_ev(); }
bool render_hdr_supported(void) { return R->hdr_supported; }
bool render_hdr_active(void) { return R->swap_hdr; }
void render_render_size(u32 *w, u32 *h) {
    *w = R->hdr.w;
    *h = R->hdr.h;
}

bool render_world_to_screen(vec3 world, vec2 *out_px) {
    vec4 c = mat4_mul_v4(R->view_proj, v4(world.x, world.y, world.z, 1.0f));
    if (c.w <= 0.01f) return false;
    float nx = c.x / c.w, ny = c.y / c.w;
    out_px->x = (nx * 0.5f + 0.5f) * (float)R->swap_extent.width;
    out_px->y = (ny * 0.5f + 0.5f) * (float)R->swap_extent.height;
    return true;
}

// A point placed in viewmodel space (drawn with the viewmodel FOV) -> the world point that
// appears at the same pixel with the world FOV (same depth). Identity when the FOVs match.
float render_vm_fov_for(const RenderCamera *c, float rvm) {
    float hip = rvm > 1.0f ? rvm : (c->vm_fov_deg > 1.0f ? c->vm_fov_deg : c->fov_deg);
    if (c->vm_ads_zoom <= 1.0f) return hip;
    // Aimed, the arms / weapon are always seen at the sight's own FOV (the reference narrowed by
    // vm_ads_zoom), independent of the main camera's and the hip hands FOV.
    float aimed = 2.0f * atanf(tanf(RENDER_ADS_REF_FOV * 0.5f * DG_DEG2RAD) / c->vm_ads_zoom) * DG_RAD2DEG;
    float t = clampf(c->ads_t, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return hip + (aimed - hip) * t;
}

float render_vm_fov(void) { return render_vm_fov_for(&R->cam, R->r_vm_fov->f); }

vec3 render_viewmodel_to_world(vec3 p) {
    float vm_fov = render_vm_fov();
    if (fabsf(vm_fov - R->cam.fov_deg) < 0.01f) return p;
    mat4 view = mat4_fps_view(R->cam.pos, R->cam.yaw, R->cam.pitch, R->cam.roll);
    vec3 v = mat4_mul_point(view, p);
    float k = tanf(R->cam.fov_deg * 0.5f * DG_DEG2RAD) / tanf(vm_fov * 0.5f * DG_DEG2RAD);
    v.x *= k;
    v.y *= k;
    return mat4_mul_point(mat4_inverse_rigid(view), v);
}

mat4 render_view_proj(void) { return R->view_proj; }
int render_width(void) { return (int)R->swap_extent.width; }
int render_height(void) { return (int)R->swap_extent.height; }
RenderStats render_stats(void) { return R->last_stats; }

void render_shadow_info(RenderShadowInfo *o) {
    memset(o, 0, sizeof(*o));
    const FrameData *fd = (const FrameData *)R->frames[R->frame_slot].ubo.mapped;
    vec3 L = v3_norm(R->env.sun_dir);
    float elev = asinf(clampf(L.y, -1.0f, 1.0f));
    o->cascades = R->csm.count;
    o->tile = R->shadow_size / 2;
    o->vm_tile = R->vm_shadow_size;
    for (int k = 0; k < 4; k++) {
        o->split[k] = R->csm.split[k];
        o->radius[k] = R->shadow_radius[k];
        o->texel_m[k] = R->csm.texel_m[k];
        shs_bias_m(R->csm.texel_m[k], R->sun_shadow.bias, R->sun_shadow.normal_bias, csm_low_sun(elev), &o->bias_m[k], &o->nbias_m[k]);
        o->active[k] = R->shadow_active[k];
    }
    for (int k = 0; k < 5; k++) o->center[k] = R->shadow_center[k];
    o->moon_active = R->moon_active;
    o->moon_radius = R->shadow_radius[4];
    o->mode = R->shq.mode;
    o->resolution = R->shq.resolution;
    o->projection = R->shq.projection;
    o->soft_quality = R->shq.soft_quality;
    o->border = R->shq.border;
    o->near_offset = R->shq.near_plane_offset;
    o->shadowmask = (int)R->shq.shadowmask;
    o->shadowmask_distance = R->shq.shadowmask_distance;
    o->sun = R->sun_shadow;
    o->sun_type = R->sun_type;
    o->filter_mode = R->sun_filter.mode;
    o->filter_tent = R->sun_filter.tent;
    o->local_tiles = R->local_tile_count;
    o->local_atlas = R->lshadow.w;
    for (u32 i = 0; i < R->light_count; i++) o->local_lights += R->light_shadow[i].x >= 0.0f;
    for (u32 i = 0; i < R->spot_count; i++) o->local_lights += R->spots[i].params.y > 0.0f;
    o->distance = R->shadow_dist;
    o->low_sun = csm_low_sun(elev);
    o->light_dir = L;
    o->vm_active = R->shadow_vm_active;
    o->taa = R->shadow_taa;
    o->vm_center = R->shadow_vm_tile.center;
    o->vm_radius = R->shadow_vm_tile.radius;
    o->vm_texel_m = R->shadow_vm_tile.texel_m;
    o->vm_pcss = R->shadow_vm_pcss;
    if (fd) {
        o->pcss_w = fd->shadow_cfg.y;
        o->max_texels = fd->ssky1.y;
        o->fade = fd->ssky0.y;
    }
    o->unrendered = R->shadow_unrendered;
}
const char *render_device_name(void) { return R->gpu_props.deviceName; }
