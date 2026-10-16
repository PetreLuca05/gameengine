// Frame orchestration: particle compute -> shadow pass -> frame graph (G-buffer prepass,
// screen-space lighting, lit scene, fog, transparency, viewmodel, post stack, final + UI) -> present.
#include "vk_internal.h"
#include "post/post.h"
#include "sky/sky.h"
#include "graph/fg.h"
#include "skeletal_mesh.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include "stb_image_write.h"

static void on_render_cvar_changed(Cvar *cv) { (void)cv; }
static size_t obj_slot_size(void);

void render_register_cvars(void) {
    Renderer *r = R;
    r->r_vsync = cvar_reg_bool("r_vsync", true, CVAR_ARCHIVE, "vertical sync");
    r->r_wireframe = cvar_reg_bool("r_wireframe", false, 0, "wireframe rendering");
    r->r_render_scale = cvar_reg_float("r_render_scale", 1.0f, 0.25f, 2.0f, CVAR_ARCHIVE, "3D resolution scale");
    r->r_shadow_quality = cvar_reg_int("r_shadow_quality", 2, 0, 3, CVAR_ARCHIVE, "0 off, 1 low, 2 medium, 3 high");
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
    r->r_vsync->on_change = on_render_cvar_changed;
}

static u32 shadow_size_for_quality(int q) {
    switch (q) { case 1: return 1024; case 2: return 2048; case 3: return 4096; default: return 16; }
}

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
        f->dyn = vkr_buffer_create(DYN_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        f->bones = vkr_buffer_create(sizeof(mat4) * MAX_SKIN_BONES * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);   // + last frame's
        f->instances = vkr_buffer_create(sizeof(GpuSkinInstance) * MAX_SKIN_DRAWS * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f->prev_models = vkr_buffer_create(sizeof(mat4) * (MAX_DRAWS + MAX_VM_DRAWS), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = R->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &R->frame_layout;
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &f->frame_set));
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &f->frame_set_pip));
        // The PiP set: its own frame UBO, the same skinning / previous-model buffers.
        VkDescriptorSet sets[2] = { f->frame_set, f->frame_set_pip };
        GpuBuffer *ubos[2] = { &f->ubo, &f->ubo_pip };
        for (int s = 0; s < 2; s++) {
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
        VkDescriptorSet sets[2] = { R->frames[i].frame_set, R->frames[i].frame_set_pip };
        for (int s = 0; s < 2; s++) {
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
    vkd_create_shadow_map(shadow_size_for_quality(R->r_shadow_quality->i));
    if (!vkp_create_pipelines()) { log_error("render: pipeline creation failed"); return false; }
    if (!vkx_init()) log_warn("render: VFX pipelines unavailable (particles disabled)");
    fg_init();
    if (!post_init()) { log_error("render: post-processing stack unavailable"); return false; }
    sky_init();
    write_sky_bindings();

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
        sky_shutdown();
        post_shutdown();
        fg_shutdown();
        vkx_shutdown();
        render_ui_shutdown();
        vkr_target_destroy(&R->occl);
        vkp_destroy_pipelines();
        vkr_destroy_all_resources();
        vkr_target_destroy(&R->shadow);
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
    if (shadow_size_for_quality(R->r_shadow_quality->i) != R->shadow_size) {
        vkDeviceWaitIdle(R->device);
        vkd_create_shadow_map(shadow_size_for_quality(R->r_shadow_quality->i));
    }
    if (R->r_aniso->modified != R->aniso_mod || R->r_texture_quality->modified != R->texq_mod) {
        R->aniso_mod = R->r_aniso->modified;
        R->texq_mod = R->r_texture_quality->modified;
        vkDeviceWaitIdle(R->device);
        vkr_destroy_samplers();
        vkr_create_samplers();
        vkr_rewrite_all_texture_sets();
        vkd_create_shadow_map(R->shadow_size);
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

    VkResult res = vkAcquireNextImageKHR(R->device, R->swapchain, UINT64_MAX, f->image_available, VK_NULL_HANDLE, &R->image_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) { R->swap_dirty = true; return false; }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) { log_error("acquire failed %d", (int)res); return false; }
    VK_CHECK(vkResetFences(R->device, 1, &f->fence));
    R->frame_active = true;
    return true;
}

void render_set_camera(const RenderCamera *cam) {
    R->cam = *cam;
    float aspect = (float)R->swap_extent.width / (float)DG_MAX(1u, R->swap_extent.height);
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
    GpuPointLight *g = &R->lights[R->light_count++];
    g->pos_radius = v4(l->pos.x, l->pos.y, l->pos.z, l->radius);
    g->color_intensity = v4(l->color.x, l->color.y, l->color.z, l->intensity);
}

void render_add_spot(const SpotLight *l) {
    if (R->spot_count >= MAX_SPOT_LIGHTS) return;
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
        VkDescriptorSet sets[2] = { R->frames[i].frame_set, R->frames[i].frame_set_pip };
        for (int s = 0; s < 2; s++) {
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
        VkDescriptorSet sets[2] = { R->frames[i].frame_set, R->frames[i].frame_set_pip };
        for (int s = 0; s < 2; s++) {
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

static void draw_items(VkCommandBuffer cmd, DrawItem *items, u32 count, bool shadow_pass, bool cull, int alpha_mode) {
    // alpha_mode: 0 = opaque only, 1 = blended only, 2 = all
    if (!count) return;
    ArenaMark mark = arena_mark(&g_engine.frame_arena);
    SortKey *keys = ARENA_PUSH_N(&g_engine.frame_arena, SortKey, count);
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        DrawItem *d = &items[i];
        if (!R->meshes[d->mesh].used) continue;
        bool blended = d->mat.color.w < 0.999f;
        if (alpha_mode == 0 && blended) continue;
        if (alpha_mode == 1 && !blended) continue;
        if (shadow_pass && (d->mat.flags & MAT_NO_SHADOW)) continue;
        if (!shadow_pass && (d->mat.flags & MAT_SHADOW_ONLY)) continue;
        if (cull) {
            vec3 c; float r;
            item_bounds(d, &c, &r);
            if (!sphere_visible(R->frustum, c, r)) { R->stats.culled++; continue; }
        }
        TextureId tex = (d->mat.flags & MAT_TEXTURED) && d->mat.texture ? d->mat.texture : R->white_tex;
        keys[n].key = shadow_pass ? (u64)d->mesh : (((u64)tex << 32) | d->mesh);
        keys[n].index = i;
        n++;
    }
    qsort(keys, n, sizeof(SortKey), cmp_sortkey);
    u32 bound_mesh = 0, bound_tex = 0;
    for (u32 k = 0; k < n; k++) {
        DrawItem *d = &items[keys[k].index];
        GpuMesh *m = &R->meshes[d->mesh];
        if (!shadow_pass) {
            TextureId tex = (d->mat.flags & MAT_TEXTURED) && d->mat.texture ? d->mat.texture : R->white_tex;
            if (!R->textures[tex].used || !R->textures[tex].ready) tex = R->white_tex;
            if (tex != bound_tex) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[tex].set, 0, NULL);
                bound_tex = tex;
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
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3;
    }
    arena_pop_to(mark);
}


// ------------------------------------------------------------------ skinned characters

typedef struct SkinBatch { u32 mesh, texture, first, count; } SkinBatch;
typedef struct SkinPass { SkinBatch batches[MAX_SKIN_DRAWS]; u32 batch_count; } SkinPass;
typedef struct CpuSkin { VkDeviceSize offset; bool ok; } CpuSkin;

// Skinned passes: 0 = main (world), 1 = shadow, 2 = viewmodel (first-person arms, after the depth clear).
static bool skin_in_pass(const SkinnedDraw *d, int pass) {
    bool vm = (d->flags & MAT_VIEWMODEL) != 0;
    if (pass == 1) return d->cast_shadow && !vm;
    if (pass == 2) return vm;
    return !vm && (d->flags & MAT_SHADOW_ONLY) == 0;
}

static bool prev_palette(u32 id, u32 *off, u32 *count);

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
        draw_of[n] = i;
        n++;
    }
    // Instanced batches per (texture, mesh): modular characters sharing parts batch per (part, LOD).
    u32 nb = skin_batch_build(keys, n, order, runs);
    GpuSkinInstance *inst = (GpuSkinInstance *)f->instances.mapped;
    for (u32 k = 0; k < n; k++) {
        const SkinnedDraw *d = &R->skin_draws[draw_of[order[k]]];
        GpuSkinInstance *g = &inst[base + k];
        g->color = d->color;
        g->params = v4(d->roughness, d->metallic, d->emissive, d->fade);
        g->info[0] = d->bone_count & 0xFFFFFu;
        g->info[1] = d->flags;
        g->info[2] = g->info[3] = 0;
        // Last frame's palette of the same object (appended after this frame's bones).
        if (!shadow && d->object_id && !R->camera_cut) {
            u32 off, count;
            if (prev_palette(d->object_id, &off, &count) && count == (d->bone_count >> 20) && off + count <= R->prev_bone_count)
                g->info[2] = R->skin_bone_count + off + 1;
        }
    }
    out->batch_count = nb;
    for (u32 b = 0; b < nb; b++)
        out->batches[b] = (SkinBatch){ (u32)(runs[b].key & 0xFFFFFFFFu), (u32)(runs[b].key >> 32), base + runs[b].first, runs[b].count };
    arena_pop_to(mark);
    return n;
}

static void draw_skin_pass(VkCommandBuffer cmd, const SkinPass *p) {
    u32 bound_tex = 0xFFFFFFFFu;
    for (u32 b = 0; b < p->batch_count; b++) {
        const SkinBatch *sb = &p->batches[b];
        GpuMesh *m = &R->meshes[sb->mesh];
        // Albedo of textured skins (set 1); white otherwise.
        TextureId tex = sb->texture && R->textures[sb->texture].used && R->textures[sb->texture].ready ? sb->texture : R->white_tex;
        if (tex != bound_tex) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[tex].set, 0, NULL);
            bound_tex = tex;
        }
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m->vb.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m->index_count, sb->count, 0, 0, sb->first);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3 * sb->count;
    }
}

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
        pc.flags[0] = d->flags & ~(u32)MAT_SHADOW_ONLY;
        pc.flags[1] = pc.flags[2] = 0;
        pc.flags[3] = pass == 1 ? g_shadow_cascade : 0;
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

static void compute_matrices(FrameData *fd) {
    float aspect = (float)R->swap_extent.width / (float)DG_MAX(1u, R->swap_extent.height);
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

    // Directional shadow: 3 cascades in a 2 x 2 atlas of the shadow map. Each cascade is a fixed
    // size ortho box (a sphere-sized square: rotating the camera never changes it) around the
    // camera, nudged forward, snapped to its own texels: stable under camera motion and as the sun
    // moves (the game also steps the light direction, game_daynight.c).
    RenderEnvironment *e = &R->env;
    float ext = e->shadow_extent > 1.0f ? e->shadow_extent : 40.0f;
    vec3 L = v3_norm(e->sun_dir);
    vec3 up = fabsf(L.y) > 0.99f ? v3(0, 0, 1) : v3(0, 1, 0);
    mat4 lview = mat4_look_at(v3(0, 0, 0), v3_neg(L), up);
    float radii[3] = { 12.0f, fmaxf(ext * 1.1f, 36.0f), fmaxf(ext * 3.5f, 110.0f) };
    vec3 fwd = v3_norm(v3(-sinf(c->yaw), 0, -cosf(c->yaw)));
    u32 tile = R->shadow_size / 2;
    float bias[3];
    for (int k = 0; k < 3; k++) {
        float r = radii[k];
        vec3 center = v3_madd(c->pos, fwd, r * 0.35f);
        if (k == 2) center = v3_lerp(center, e->shadow_center, 0.5f);
        vec3 cl = mat4_mul_point(lview, center);
        float texel = (2.0f * r) / (float)tile;
        cl.x = floorf(cl.x / texel) * texel;
        cl.y = floorf(cl.y / texel) * texel;
        float depth_range = fmaxf(r * 4.0f, 120.0f);
        // Towards the sun the box reaches far out: distant tall casters (the towers on the
        // horizon) throw long shadows at a low sun, and a box that only sometimes contained them
        // (its centre follows the view) made those shadows pop in and out as the camera turned.
        // The depth bias is rescaled so it stays the same in metres.
        float reach = 3000.0f;
        mat4 lproj = mat4_ortho(cl.x - r, cl.x + r, cl.y - r, cl.y + r, -cl.z - reach, -cl.z + depth_range);
        bias[k] = (2.0f * depth_range) / (reach + depth_range);
        // NDC -> the cascade's atlas tile (x right, y down in the atlas).
        mat4 t = mat4_identity();
        t.m[0] = 0.5f;
        t.m[5] = 0.5f;
        t.m[12] = (k & 1) ? 0.5f : -0.5f;
        t.m[13] = (k >> 1) ? 0.5f : -0.5f;
        R->shadow_vps[k] = mat4_mul(t, mat4_mul(lproj, lview));
    }
    R->shadow_splits = v4(radii[0], radii[1], radii[2], 3.0f);
    R->shadow_bias = v4(bias[0], bias[1], bias[2], 0);
    R->shadow_vp = R->shadow_vps[2];

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
    fd->camera_pos = v4(c->pos.x, c->pos.y, c->pos.z, (float)g_engine.real_time);
    fd->sun_dir = v4(L.x, L.y, L.z, e->sun_intensity);
    fd->sun_color = v4(e->sun_color.x, e->sun_color.y, e->sun_color.z, R->r_shadow_quality->i > 0 ? 1.0f : 0.0f);
    fd->sky_ambient = v4(e->sky_ambient.x, e->sky_ambient.y, e->sky_ambient.z, e->ambient_intensity);
    fd->ground_ambient = v4(e->ground_ambient.x, e->ground_ambient.y, e->ground_ambient.z, (float)R->light_count);
    // Volumetric fog replaces the analytic per-pixel fog of the lit shaders.
    bool vol = post_settings()->volumetrics > 0 && fg_pass_enabled("vol_inject");
    fd->fog = v4(e->fog_color.x, e->fog_color.y, e->fog_color.z, vol ? 0.0f : e->fog_density);
    int pcf = R->r_shadow_quality->i >= 3 ? 2 : 1;
    if (e->atmosphere && e->cloud_cover > 0.65f) pcf++;   // weather: overcast skies soften the shadows
    fd->params = v4(R->r_grid->i ? 1.0f : 0.0f, 1.0f / (float)R->shadow_size, (float)pcf, 0.0f);
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
        fd->puddle_xf = wx->puddle_xform;
    }
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
    SkinPass *skin_main, *skin_vm;
    bool any_skin_vm;
    VkDeviceSize line_off[2], text_off, ui_voff, ui_ioff;
    FgRes hdr, depth, vm_depth, normal, material, velocity, swap, scene_copy, ldr;
    PostLitInputs lit;
    // Scope picture-in-picture (RSIGHT_SCOPE_PIP while aiming)
    FgRes pip_color, pip_depth;
    u32 pip_size;
    vec4 pip_frustum[6];
    mat4 pip_from_view;     // main view space -> PiP clip
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_PREPASS]);
    draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 0);
    draw_skinned(cmd, 0, PIPE_PREPASS_SKIN, PIPE_PREPASS, F.skin_main);
    vkCmdEndRendering(cmd);
}

// Lit static opaque (depth from the prepass, LESS_OR_EQUAL: shades each pixel once).
static void exec_opaque(VkCommandBuffer cmd, void *u) {
    (void)u;
    begin_pass(cmd, R->hdr.view, true, R->depth.view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, false);
    bind_frame(cmd);
    post_push_lit_inputs(cmd, &F.lit, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT]);
    draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 0);
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
    draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 1);
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
    if ((R->vm_draw_count || F.any_skin_vm) && !R->sight.hide_viewmodel) {
        bind_frame(cmd);
        post_push_lit_inputs(cmd, NULL, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_PREPASS]);
        draw_items(cmd, R->vm_draws, R->vm_draw_count, false, false, 2);
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
    if ((R->vm_draw_count || F.any_skin_vm) && !R->sight.hide_viewmodel) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT]);
        draw_items(cmd, R->vm_draws, R->vm_draw_count, false, false, 2);
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

// Scope picture-in-picture: the opaque scene (static + characters, lit forward like the main
// opaque pass, no screen-space inputs) from the objective lens at the scope FOV into a square
// target; the sky is filled in by post_scope_sky. The local player's own weapon is in the
// viewmodel list and never drawn here; the near plane skips the rest of the body.
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
    post_push_lit_inputs(cmd, NULL, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LIT]);
    vec4 saved[6];
    memcpy(saved, R->frustum, sizeof(saved));
    memcpy(R->frustum, F.pip_frustum, sizeof(saved));
    draw_items(cmd, R->draws, R->draw_count, false, true, 0);
    memcpy(R->frustum, saved, sizeof(saved));
    draw_skinned(cmd, 0, PIPE_SKIN, PIPE_LIT, F.skin_main);
    // Particles (smoke, sparks, blood, muzzle flashes of others ...) through the scope camera.
    vkx_draw_particles_pip(cmd, F.f->frame_set_pip);
    draw_precip(cmd, F.f->frame_set_pip);
    vkCmdEndRendering(cmd);
}

// PiP camera + its frame UBO; returns false when no PiP is needed this frame.
static bool setup_scope_pip(FrameData *fd) {
    const RenderSight *s = &R->sight;
    R->pip_fov = 0;
    if (s->kind != RSIGHT_SCOPE_PIP || s->ads_t < 0.02f) return false;
    vec3 fwd = v3_norm(s->fwd), up = v3_norm(s->up);
    if (v3_len2(fwd) < 0.5f) return false;
    // The ocular's angular radius from the eye, magnified: the PiP covers exactly what the lens
    // can show (plus a margin for the chromatic aberration / eye box shift).
    float d = fmaxf(v3_dist(s->lens_pos, R->cam.pos), 0.02f);
    float tan_vm = tanf(render_vm_fov() * 0.5f * DG_DEG2RAD);
    float tan_base = tanf(fmaxf(s->base_fov, 10.0f) * 0.5f * DG_DEG2RAD);
    float t_lens = (s->lens_radius / d) * tan_base / fmaxf(tan_vm, 1e-3f);
    float tan_half = clampf(1.15f * t_lens / fmaxf(s->zoom, 1.0f), tanf(0.25f * DG_DEG2RAD), tanf(30.0f * DG_DEG2RAD));
    R->pip_fov = 2.0f * atanf(tan_half) * DG_RAD2DEG;
    u32 n = (u32)DG_CLAMP((int)lrintf((float)R->hdr.h * clampf(s->pip_scale, 0.2f, 2.0f)), 128, 2048);
    F.pip_size = n;
    mat4 view = mat4_look_at(s->objective_pos, v3_add(s->objective_pos, fwd), up);
    mat4 proj = mat4_perspective(2.0f * atanf(tan_half), 1.0f, 0.3f, R->cam.zfar);
    mat4 vp = mat4_mul(proj, view);
    extract_frustum(vp, F.pip_frustum);
    F.pip_from_view = mat4_mul(vp, mat4_inverse_rigid(R->view));
    FrameData *pd = (FrameData *)F.f->ubo_pip.mapped;
    memcpy(pd, fd, sizeof(FrameData));
    pd->view_proj = vp;
    pd->view = view;
    pd->proj = proj;
    pd->view_proj_nj = pd->prev_view_proj_nj = vp;
    pd->jitter = v4(0, 0, 0, 0);
    pd->post_flags = v4(0, 0, 0, 0);
    pd->post_fog = v4(0, 80, 0, 0);
    pd->fog = v4(R->env.fog_color.x, R->env.fog_color.y, R->env.fog_color.z, R->env.fog_density);
    pd->camera_pos = v4(s->objective_pos.x, s->objective_pos.y, s->objective_pos.z, fd->camera_pos.w);
    pd->screen = v4((float)n, (float)n, 1.0f / (float)n, 1.0f / (float)n);
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
    gd.name = "gb_velocity";
    gd.format = GB_VELOCITY_FORMAT;
    F.velocity = fg_texture(&gd);

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

    int p = fg_pass("prepass", exec_prepass, NULL);
    fg_write(p, F.depth, FG_DEPTH);
    fg_write(p, F.normal, FG_COLOR);
    fg_write(p, F.material, FG_COLOR);
    fg_write(p, F.velocity, FG_COLOR);

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
        if (p < 0) F.pip_color = FG_NONE;
        else {
            mat4 view = mat4_look_at(R->sight.objective_pos, v3_add(R->sight.objective_pos, v3_norm(R->sight.fwd)), v3_norm(R->sight.up));
            mat4 proj = mat4_perspective(R->pip_fov * DG_DEG2RAD, 1.0f, 0.3f, R->cam.zfar);
            post_scope_sky(F.pip_color, F.pip_depth, mat4_inverse(mat4_mul(proj, view)), R->sight.objective_pos);
        }
    }
    post_set_sight(&R->sight, F.pip_color, F.pip_from_view, R->vm_proj, R->pip_fov);

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

void render_end_frame(void) {
    if (!R->frame_active) return;
    write_cookie_binding();
    write_weather_bindings();
    FrameResources *f = &R->frames[R->frame_slot];
    VkCommandBuffer cmd = f->cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    vkr_record_uploads(cmd);

    FrameData *fd = (FrameData *)f->ubo.mapped;
    compute_matrices(fd);
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
    static SkinPass skin_main, skin_shadow, skin_vm;
    skin_main.batch_count = skin_shadow.batch_count = skin_vm.batch_count = 0;
    F.skin_main = &skin_main;
    F.skin_vm = &skin_vm;
    for (u32 i = 0; i < R->skin_draw_count; i++) F.any_skin_vm |= (R->skin_draws[i].flags & MAT_VIEWMODEL) != 0;
    ArenaMark skin_mark = arena_mark(&g_engine.frame_arena);
    if (R->skin_draw_count) {
        if (cpu_skin) {
            F.cpu_skinned = ARENA_PUSH_N(&g_engine.frame_arena, CpuSkin, R->skin_draw_count);
            cpu_skin_all(f, F.cpu_skinned);
        } else {
            memcpy(f->bones.mapped, R->skin_bones, sizeof(mat4) * R->skin_bone_count);
            if (R->prev_bone_count && R->skin_bone_count + R->prev_bone_count <= MAX_SKIN_BONES * 2)
                memcpy((mat4 *)f->bones.mapped + R->skin_bone_count, R->prev_bones, sizeof(mat4) * R->prev_bone_count);
            u32 used = build_skin_pass(f, 0, 0, &skin_main);
            used += build_skin_pass(f, 1, used, &skin_shadow);
            build_skin_pass(f, 2, used, &skin_vm);
        }
    }

    // ---------------------------------------------------------- GPU particle simulation
    vkx_begin_frame_queries(cmd);
    vkx_record_compute(cmd);

    // ---------------------------------------------------------- shadow pass
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
        if (R->r_shadow_quality->i > 0) {
            // One atlas tile per cascade (the cascade index rides in the push constants). The
            // cascade matrices already map into their tile: full viewport, scissored to the tile.
            u32 tile = R->shadow.w / 2;
            for (int k = 0; k < 3; k++) {
                VkViewport vp = { 0.0f, 0.0f, (float)R->shadow.w, (float)R->shadow.h, 0.0f, 1.0f };
                VkRect2D sc = { { (int)((k & 1) * tile), (int)((k >> 1) * tile) }, { tile, tile } };
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
                vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
                g_shadow_cascade = (u32)k;
                draw_items(cmd, R->draws, R->draw_count, true, false, 0);
                if (F.cpu_skinned) draw_cpu_skinned(cmd, f, F.cpu_skinned, 1);
                else if (skin_shadow.batch_count) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SKIN_SHADOW]);
                    PushLit pc;
                    memset(&pc, 0, sizeof(pc));
                    pc.flags[3] = (u32)k;
                    vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
                    draw_skin_pass(cmd, &skin_shadow);
                }
            }
            g_shadow_cascade = 0;
        }
        vkCmdEndRendering(cmd);
    }
    vkr_image_barrier(cmd, R->shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);

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
        g_shadow_cascade = 3;
        draw_items(cmd, R->draws, R->draw_count, true, false, 0);
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
    float t = clampf(c->ads_t, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return hip + (c->fov_deg - hip) * t;
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
const char *render_device_name(void) { return R->gpu_props.deviceName; }
