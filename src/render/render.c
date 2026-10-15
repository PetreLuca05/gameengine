// Frame orchestration: shadow pass -> HDR scene -> post (tonemap/FXAA) + UI -> present.
#include "vk_internal.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include "stb_image_write.h"

static void on_render_cvar_changed(Cvar *cv) { (void)cv; }

void render_register_cvars(void) {
    Renderer *r = R;
    r->r_vsync = cvar_reg_bool("r_vsync", true, CVAR_ARCHIVE, "vertical sync");
    r->r_wireframe = cvar_reg_bool("r_wireframe", false, 0, "wireframe rendering");
    r->r_render_scale = cvar_reg_float("r_render_scale", 1.0f, 0.25f, 2.0f, CVAR_ARCHIVE, "3D resolution scale");
    r->r_shadow_quality = cvar_reg_int("r_shadow_quality", 2, 0, 3, CVAR_ARCHIVE, "0 off, 1 low, 2 medium, 3 high");
    r->r_aniso = cvar_reg_int("r_aniso", 8, 1, 16, CVAR_ARCHIVE, "anisotropic filtering level");
    r->r_texture_quality = cvar_reg_int("r_texture_quality", 2, 0, 2, CVAR_ARCHIVE, "0 low, 1 medium, 2 high");
    r->r_aa = cvar_reg_int("r_aa", 1, 0, 2, CVAR_ARCHIVE, "anti-aliasing: 0 none, 1 FXAA, 2 TAA (stub, uses FXAA)");
    r->r_brightness = cvar_reg_float("r_brightness", 1.0f, 0.3f, 3.0f, CVAR_ARCHIVE, "exposure multiplier");
    r->r_gamma = cvar_reg_float("r_gamma", 1.0f, 0.5f, 2.0f, CVAR_ARCHIVE, "gamma adjustment");
    r->r_grid = cvar_reg_bool("r_grid", true, CVAR_ARCHIVE, "draw unit grid on dev geometry");
    r->r_vignette = cvar_reg_float("r_vignette", 0.35f, 0.0f, 2.0f, CVAR_ARCHIVE, "vignette strength");
    r->r_shader_hotreload = cvar_reg_bool("r_shader_hotreload", true, 0, "watch shader sources and reload on change");
    r->r_culling = cvar_reg_bool("r_culling", true, 0, "frustum culling");
    r->r_vm_fov = cvar_reg_float("r_viewmodel_fov", 0.0f, 0.0f, 120.0f, CVAR_ARCHIVE, "viewmodel FOV (0 = same as world, keeps IK hands aligned)");
    r->r_cpu_skinning = cvar_reg_bool("r_cpu_skinning", false, 0, "skin characters on the CPU (fallback / debugging)");
    r->r_bloom = cvar_reg_bool("r_bloom", true, CVAR_ARCHIVE, "bloom (HDR mip chain)");
    r->r_bloom_intensity = cvar_reg_float("r_bloom_intensity", 0.06f, 0.0f, 1.0f, CVAR_ARCHIVE, "bloom strength");
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
        f->dyn = vkr_buffer_create(DYN_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        f->bones = vkr_buffer_create(sizeof(mat4) * MAX_SKIN_BONES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f->instances = vkr_buffer_create(sizeof(GpuSkinInstance) * MAX_SKIN_DRAWS * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);

        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = R->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &R->frame_layout;
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &f->frame_set));
        VkDescriptorBufferInfo bi = { f->ubo.buffer, 0, sizeof(FrameData) };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = f->frame_set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
        VkDescriptorBufferInfo sb[2] = { { f->bones.buffer, 0, VK_WHOLE_SIZE }, { f->instances.buffer, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet ws[2];
        for (int k = 0; k < 2; k++) {
            ws[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            ws[k].dstSet = f->frame_set;
            ws[k].dstBinding = (u32)(2 + k);
            ws[k].descriptorCount = 1;
            ws[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ws[k].pBufferInfo = &sb[k];
        }
        vkUpdateDescriptorSets(R->device, 2, ws, 0, NULL);
    }
}

static void destroy_frames(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        FrameResources *f = &R->frames[i];
        vkr_flush_deferred(f);
        vkr_buffer_destroy_now(&f->ubo);
        vkr_buffer_destroy_now(&f->dyn);
        vkr_buffer_destroy_now(&f->bones);
        vkr_buffer_destroy_now(&f->instances);
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
    vkd_create_targets();
    vkd_create_shadow_map(shadow_size_for_quality(R->r_shadow_quality->i));
    if (!vkp_create_pipelines()) { log_error("render: pipeline creation failed"); return false; }
    if (!vkx_init()) log_warn("render: VFX pipelines unavailable (particles disabled)");

    R->draws = (DrawItem *)malloc(sizeof(DrawItem) * MAX_DRAWS);
    R->vm_draws = (DrawItem *)malloc(sizeof(DrawItem) * MAX_VM_DRAWS);
    R->skin_draws = (SkinnedDraw *)malloc(sizeof(SkinnedDraw) * MAX_SKIN_DRAWS);
    R->skin_bones = (mat4 *)malloc(sizeof(mat4) * MAX_SKIN_BONES);
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
        vkx_shutdown();
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
    free(R->line_cpu[0]); free(R->line_cpu[1]); free(R->text_cpu);
    free(R->ui_verts); free(R->ui_indices); free(R->ui_cmds);
    free(R);
    g_engine.render = NULL;
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(R->device);
    vkd_destroy_targets();
    if (!vkd_create_swapchain()) { R->swap_dirty = true; return; }
    vkd_create_targets();
    vkp_create_pipelines(); // swapchain format can change
}

static void apply_setting_changes(void) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_engine.window, &w, &h);
    if (w <= 0 || h <= 0) return;
    if ((u32)w != R->swap_extent.width || (u32)h != R->swap_extent.height) R->swap_dirty = true;
    if ((R->r_vsync->i != 0) != R->vsync_applied) R->swap_dirty = true;
    if (R->swap_dirty) recreate_swapchain();
    if (fabsf(R->r_render_scale->f - R->render_scale_applied) > 1e-4f) {
        vkDeviceWaitIdle(R->device);
        vkd_destroy_targets();
        vkd_create_targets();
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
        vkr_write_tex_set(R->hdr_set, R->hdr.view, R->sampler_clamp);
        vkd_create_shadow_map(R->shadow_size);
    }
}

bool render_begin_frame(void) {
    R->frame_active = false;
    R->draw_count = R->vm_draw_count = R->light_count = 0;
    R->skin_draw_count = R->skin_bone_count = 0;
    R->line_count[0] = R->line_count[1] = 0;
    R->text_count = 0;
    R->ui_vcount = R->ui_icount = R->ui_ccount = 0;
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
    memcpy(R->skin_bones + R->skin_bone_count, d->palette, sizeof(mat4) * d->bone_count);
    // The palette pointer is replaced by its offset into this frame's bone buffer.
    s->palette = NULL;
    s->bone_count = R->skin_bone_count | ((u32)d->bone_count << 20);
    R->skin_bone_count += d->bone_count;
}

void render_draw_viewmodel(const DrawItem *item) {
    if (!item->mesh || R->vm_draw_count >= MAX_VM_DRAWS) return;
    DrawItem *d = &R->vm_draws[R->vm_draw_count++];
    *d = *item;
    d->mat.flags |= MAT_VIEWMODEL;
}

void render_add_light(const PointLight *l) {
    if (R->light_count >= MAX_POINT_LIGHTS) return;
    GpuPointLight *g = &R->lights[R->light_count++];
    g->pos_radius = v4(l->pos.x, l->pos.y, l->pos.z, l->radius);
    g->color_intensity = v4(l->color.x, l->color.y, l->color.z, l->intensity);
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
        pc.flags[1] = pc.flags[2] = pc.flags[3] = 0;
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3;
    }
    arena_pop_to(mark);
}


// ------------------------------------------------------------------ skinned characters

typedef struct SkinBatch { u32 mesh, first, count; } SkinBatch;
typedef struct SkinPass { SkinBatch batches[MAX_SKIN_DRAWS]; u32 batch_count; } SkinPass;
typedef struct CpuSkin { VkDeviceSize offset; bool ok; } CpuSkin;

static int cmp_skin_mesh(const void *a, const void *b) {
    u32 ma = ((const SortKey *)a)->key, mb = ((const SortKey *)b)->key;
    return ma < mb ? -1 : (ma > mb ? 1 : 0);
}

// Skinned passes: 0 = main (world), 1 = shadow, 2 = viewmodel (first-person arms, after the depth clear).
static bool skin_in_pass(const SkinnedDraw *d, int pass) {
    bool vm = (d->flags & MAT_VIEWMODEL) != 0;
    if (pass == 1) return d->cast_shadow && !vm;
    if (pass == 2) return vm;
    return !vm && (d->flags & MAT_SHADOW_ONLY) == 0;
}

// Writes instance records for one pass (culled, sorted by mesh) starting at `base`.
static u32 build_skin_pass(FrameResources *f, int pass, u32 base, SkinPass *out) {
    bool shadow = pass == 1;
    ArenaMark mark = arena_mark(&g_engine.frame_arena);
    SortKey *keys = ARENA_PUSH_N(&g_engine.frame_arena, SortKey, R->skin_draw_count + 1);
    u32 n = 0;
    for (u32 i = 0; i < R->skin_draw_count; i++) {
        const SkinnedDraw *d = &R->skin_draws[i];
        if (!skin_in_pass(d, pass)) continue;
        if (pass == 0 && R->r_culling->i && d->radius > 0 && !sphere_visible(R->frustum, d->center, d->radius)) { R->stats.culled++; continue; }
        keys[n].key = d->mesh;
        keys[n].index = i;
        n++;
    }
    qsort(keys, n, sizeof(SortKey), cmp_skin_mesh);
    GpuSkinInstance *inst = (GpuSkinInstance *)f->instances.mapped;
    out->batch_count = 0;
    for (u32 k = 0; k < n; k++) {
        const SkinnedDraw *d = &R->skin_draws[keys[k].index];
        GpuSkinInstance *g = &inst[base + k];
        g->color = d->color;
        g->params = v4(d->roughness, d->metallic, d->emissive, d->fade);
        g->info[0] = d->bone_count & 0xFFFFFu;
        g->info[1] = d->flags;
        g->info[2] = g->info[3] = 0;
        if (out->batch_count && out->batches[out->batch_count - 1].mesh == d->mesh) out->batches[out->batch_count - 1].count++;
        else out->batches[out->batch_count++] = (SkinBatch){ d->mesh, base + k, 1 };
    }
    arena_pop_to(mark);
    return n;
}

static void draw_skin_pass(VkCommandBuffer cmd, const SkinPass *p) {
    for (u32 b = 0; b < p->batch_count; b++) {
        const SkinBatch *sb = &p->batches[b];
        GpuMesh *m = &R->meshes[sb->mesh];
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
        pc.flags[1] = pc.flags[2] = pc.flags[3] = 0;
        vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
        R->stats.draw_calls++;
        R->stats.triangles += m->index_count / 3;
    }
}

static void compute_matrices(FrameData *fd) {
    float aspect = (float)R->swap_extent.width / (float)DG_MAX(1u, R->swap_extent.height);
    RenderCamera *c = &R->cam;
    R->view = mat4_fps_view(c->pos, c->yaw, c->pitch, c->roll);
    R->proj = mat4_perspective(c->fov_deg * DG_DEG2RAD, aspect, c->znear, c->zfar);
    R->view_proj = mat4_mul(R->proj, R->view);
    float vm_fov = R->r_vm_fov->f > 1.0f ? R->r_vm_fov->f : c->fov_deg;
    R->vm_proj = mat4_perspective(vm_fov * DG_DEG2RAD, aspect, 0.01f, 100.0f);
    extract_frustum(R->view_proj, R->frustum);

    // Directional shadow: ortho box around shadow_center, snapped to texels to avoid shimmer.
    RenderEnvironment *e = &R->env;
    float ext = e->shadow_extent > 1.0f ? e->shadow_extent : 40.0f;
    vec3 L = v3_norm(e->sun_dir);
    vec3 up = fabsf(L.y) > 0.99f ? v3(0, 0, 1) : v3(0, 1, 0);
    mat4 lview = mat4_look_at(v3(0, 0, 0), v3_neg(L), up);
    vec3 cl = mat4_mul_point(lview, e->shadow_center);
    float texel = (2.0f * ext) / (float)R->shadow_size;
    cl.x = floorf(cl.x / texel) * texel;
    cl.y = floorf(cl.y / texel) * texel;
    float depth_range = ext * 4.0f;
    mat4 lproj = mat4_ortho(cl.x - ext, cl.x + ext, cl.y - ext, cl.y + ext, -cl.z - depth_range, -cl.z + depth_range);
    R->shadow_vp = mat4_mul(lproj, lview);

    fd->view_proj = R->view_proj;
    fd->view = R->view;
    fd->proj = R->proj;
    fd->vm_proj = R->vm_proj;
    fd->shadow_vp = R->shadow_vp;
    fd->camera_pos = v4(c->pos.x, c->pos.y, c->pos.z, (float)g_engine.real_time);
    fd->sun_dir = v4(L.x, L.y, L.z, e->sun_intensity);
    fd->sun_color = v4(e->sun_color.x, e->sun_color.y, e->sun_color.z, R->r_shadow_quality->i > 0 ? 1.0f : 0.0f);
    fd->sky_ambient = v4(e->sky_ambient.x, e->sky_ambient.y, e->sky_ambient.z, e->ambient_intensity);
    fd->ground_ambient = v4(e->ground_ambient.x, e->ground_ambient.y, e->ground_ambient.z, (float)R->light_count);
    fd->fog = v4(e->fog_color.x, e->fog_color.y, e->fog_color.z, e->fog_density);
    int pcf = R->r_shadow_quality->i >= 3 ? 2 : 1;
    fd->params = v4(R->r_grid->i ? 1.0f : 0.0f, 1.0f / (float)R->shadow_size, (float)pcf, 0.0f);
    fd->screen = v4((float)R->hdr.w, (float)R->hdr.h, 1.0f / (float)R->hdr.w, 1.0f / (float)R->hdr.h);
    memcpy(fd->lights, R->lights, sizeof(GpuPointLight) * R->light_count);
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

void render_end_frame(void) {
    if (!R->frame_active) return;
    FrameResources *f = &R->frames[R->frame_slot];
    VkCommandBuffer cmd = f->cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    vkr_record_uploads(cmd);

    FrameData *fd = (FrameData *)f->ubo.mapped;
    compute_matrices(fd);

    VkDeviceSize line_off[2] = { 0, 0 };
    for (int k = 0; k < 2; k++)
        if (R->line_count[k]) line_off[k] = dyn_push(f, R->line_cpu[k], sizeof(LineVertex) * R->line_count[k]);
    VkDeviceSize text_off = R->text_count ? dyn_push(f, R->text_cpu, sizeof(TextVertex) * R->text_count) : 0;
    {
        static int warned;
        if (text_off == (VkDeviceSize)-1 && !warned++)
            log_warn("render: dynamic buffer full (%llu used of %llu, lines %u/%u, text %u verts) - 3D text dropped", (unsigned long long)f->dyn_used,
                     (unsigned long long)f->dyn.size, R->line_count[0], R->line_count[1], R->text_count);
    }
    VkDeviceSize ui_voff = R->ui_vcount ? dyn_push(f, R->ui_verts, sizeof(UiVertex) * R->ui_vcount) : 0;
    VkDeviceSize ui_ioff = R->ui_icount ? dyn_push(f, R->ui_indices, sizeof(u32) * R->ui_icount) : 0;

    // Skinned characters: palettes + per-pass instance lists (GPU) or CPU-skinned vertices.
    bool cpu_skin = R->r_cpu_skinning->i != 0;
    static SkinPass skin_main, skin_shadow, skin_vm;
    skin_main.batch_count = skin_shadow.batch_count = skin_vm.batch_count = 0;
    bool any_skin_vm = false;
    for (u32 i = 0; i < R->skin_draw_count; i++) any_skin_vm |= (R->skin_draws[i].flags & MAT_VIEWMODEL) != 0;
    CpuSkin *cpu_skinned = NULL;
    ArenaMark skin_mark = arena_mark(&g_engine.frame_arena);
    if (R->skin_draw_count) {
        if (cpu_skin) {
            cpu_skinned = ARENA_PUSH_N(&g_engine.frame_arena, CpuSkin, R->skin_draw_count);
            cpu_skin_all(f, cpu_skinned);
        } else {
            memcpy(f->bones.mapped, R->skin_bones, sizeof(mat4) * R->skin_bone_count);
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
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
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
            set_viewport(cmd, R->shadow.w, R->shadow.h);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SHADOW]);
            vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
            draw_items(cmd, R->draws, R->draw_count, true, false, 0);
            if (cpu_skinned) draw_cpu_skinned(cmd, f, cpu_skinned, 1);
            else if (skin_shadow.batch_count) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_SKIN_SHADOW]);
                draw_skin_pass(cmd, &skin_shadow);
            }
        }
        vkCmdEndRendering(cmd);
    }
    vkr_image_barrier(cmd, R->shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);

    // ---------------------------------------------------------- scene passes (HDR)
    // 1 static opaque (clears) | 2 decals (depth read-only) | 3 characters + blended items |
    // scene copy (distortion) | 4 particles, lines, text (depth read-only, sampled) |
    // 5 viewmodel on its own depth (world depth survives for next frame's particle collision).
    const VkPipelineStageFlags2 DEPTH_STAGES = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags2 DEPTH_RW = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    vkr_image_barrier(cmd, R->hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1);
    // Last frame's depth was read by this frame's particle compute: WAR on compute before clearing.
    vkr_image_barrier(cmd, R->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                      DEPTH_STAGES, DEPTH_RW, 1);
    {
        VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = R->hdr.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.float32[0] = R->env.fog_color.x;
        ca.clearValue.color.float32[1] = R->env.fog_color.y;
        ca.clearValue.color.float32[2] = R->env.fog_color.z;
        ca.clearValue.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = R->depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea.extent.width = R->hdr.w;
        ri.renderArea.extent.height = R->hdr.h;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;

        // ---- 1: static opaque
        vkCmdBeginRendering(cmd, &ri);
        set_viewport(cmd, R->hdr.w, R->hdr.h);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT]);
        draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 0);
        vkCmdEndRendering(cmd);

        // ---- 2: decals (box projected onto the static scene; characters come after so they never receive them)
        vkr_image_barrier(cmd, R->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          DEPTH_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, DEPTH_STAGES | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        vkCmdBeginRendering(cmd, &ri);
        set_viewport(cmd, R->hdr.w, R->hdr.h);
        vkx_draw_decals(cmd);
        vkCmdEndRendering(cmd);
        vkr_image_barrier(cmd, R->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          DEPTH_STAGES | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, DEPTH_STAGES, DEPTH_RW, 1);

        // ---- 3: characters + blended items
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        vkCmdBeginRendering(cmd, &ri);
        set_viewport(cmd, R->hdr.w, R->hdr.h);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
        if (cpu_skinned || skin_main.batch_count) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
            if (cpu_skinned) draw_cpu_skinned(cmd, f, cpu_skinned, 0);
            else {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_SKIN_WIRE : PIPE_SKIN]);
                draw_skin_pass(cmd, &skin_main);
            }
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LIT_BLEND]);
        draw_items(cmd, R->draws, R->draw_count, false, R->r_culling->i != 0, 1);
        vkCmdEndRendering(cmd);

        // ---- scene copy for distortion, depth -> read-only (sampled by soft particles)
        vkx_copy_scene(cmd);
        vkr_image_barrier(cmd, R->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          DEPTH_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          DEPTH_STAGES | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);

        // ---- 4: particles + world lines / text
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        vkCmdBeginRendering(cmd, &ri);
        set_viewport(cmd, R->hdr.w, R->hdr.h);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
        if (R->line_count[0] && line_off[0] != (VkDeviceSize)-1) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LINES]);
            vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &line_off[0]);
            vkCmdDraw(cmd, R->line_count[0], 1, 0, 0);
            R->stats.draw_calls++;
        }
        if (R->text_count && text_off != (VkDeviceSize)-1) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_TEXT]);
            TextureId atlas = R->textures[R->text_atlas].ready ? R->text_atlas : R->white_tex;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[atlas].set, 0, NULL);
            vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &text_off);
            vkCmdDraw(cmd, R->text_count, 1, 0, 0);
            R->stats.draw_calls++;
        }
        vkx_draw_particles(cmd);
        vkCmdEndRendering(cmd);

        // ---- 5: viewmodel (own depth) + overlay lines
        VkRenderingAttachmentInfo vda = da;
        vda.imageView = R->vm_depth.view;
        vda.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        vda.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        vda.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        vkr_image_barrier(cmd, R->vm_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, DEPTH_STAGES,
                          0, DEPTH_STAGES, DEPTH_RW, 1);
        ri.pDepthAttachment = &vda;
        vkCmdBeginRendering(cmd, &ri);
        set_viewport(cmd, R->hdr.w, R->hdr.h);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 0, 1, &f->frame_set, 0, NULL);
        if (R->vm_draw_count || any_skin_vm) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_LIT_WIRE : PIPE_LIT]);
            draw_items(cmd, R->vm_draws, R->vm_draw_count, false, false, 2);
            // First-person arms (body model), same depth as the viewmodel weapon.
            if (cpu_skinned || skin_vm.batch_count) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_lit, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
                if (cpu_skinned) draw_cpu_skinned(cmd, f, cpu_skinned, 2);
                else {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[R->r_wireframe->i ? PIPE_SKIN_WIRE : PIPE_SKIN]);
                    draw_skin_pass(cmd, &skin_vm);
                }
            }
        }
        if (R->line_count[1] && line_off[1] != (VkDeviceSize)-1) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_LINES_OVERLAY]);
            vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &line_off[1]);
            vkCmdDraw(cmd, R->line_count[1], 1, 0, 0);
            R->stats.draw_calls++;
        }
        vkCmdEndRendering(cmd);
    }
    vkr_image_barrier(cmd, R->hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
    vkx_bloom(cmd);

    // ---------------------------------------------------------- post + UI (swapchain)
    VkImage swap = R->swap_images[R->image_index];
    vkr_image_barrier(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1);
    {
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

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_POST]);
        VkDescriptorSet post_sets[2] = { R->hdr_set, vkx_bloom_set() };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->layout_post2, 0, 2, post_sets, 0, NULL);
        float post[8] = { R->r_brightness->f, R->r_gamma->f, R->r_aa->i > 0 ? 1.0f : 0.0f, R->r_vignette->f,
                          1.0f / (float)R->hdr.w, 1.0f / (float)R->hdr.h, vkx_bloom_intensity(), R->exposure_boost };
        vkCmdPushConstants(cmd, R->layout_post2, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(post), post);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        if (R->ui_ccount && ui_voff != (VkDeviceSize)-1 && ui_ioff != (VkDeviceSize)-1) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_UI]);
            float scr[4] = { 2.0f / (float)R->swap_extent.width, 2.0f / (float)R->swap_extent.height, 0, 0 };
            vkCmdPushConstants(cmd, R->layout_ui, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(scr), scr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buffer, &ui_voff);
            vkCmdBindIndexBuffer(cmd, f->dyn.buffer, ui_ioff, VK_INDEX_TYPE_UINT32);
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

    GpuBuffer shot = { 0 };
    bool want_shot = R->screenshot_path[0] != 0;
    if (want_shot) {
        u32 w = R->swap_extent.width, h = R->swap_extent.height;
        shot = vkr_buffer_create_readback((VkDeviceSize)w * h * 4);
        vkr_image_barrier(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
        vkr_image_barrier(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
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

    if (want_shot) {
        VK_CHECK(vkWaitForFences(R->device, 1, &f->fence, VK_TRUE, UINT64_MAX));
        vmaInvalidateAllocation(R->vma, shot.alloc, 0, VK_WHOLE_SIZE);
        save_screenshot(&shot, R->swap_extent.width, R->swap_extent.height);
        vkr_buffer_destroy_now(&shot);
        R->screenshot_path[0] = 0;
    }

    R->frame_slot = (R->frame_slot + 1) % FRAMES_IN_FLIGHT;
    R->frame_active = false;
}

void render_screenshot(const char *path) { dg_strcpy(R->screenshot_path, sizeof(R->screenshot_path), path); }

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
vec3 render_viewmodel_to_world(vec3 p) {
    float vm_fov = R->r_vm_fov->f > 1.0f ? R->r_vm_fov->f : R->cam.fov_deg;
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
