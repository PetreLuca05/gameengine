// UI renderer extras (docs/UI.md "Rendering"):
//   * backdrop: the final image blurred at a quarter resolution (persistent target), sampled by
//     the UI in screen space under modal panels (render_ui_backdrop, shaders/ui.frag uv.x < 0);
//   * item views: offscreen RGBA8 targets with their own camera and studio lighting, drawn from a
//     list of parts (boxes / cylinders) - the inventory preview and the icon bakes. A view can be
//     blitted into another texture (the icon atlas) and / or read back into a PNG after the frame.
#include "vk_internal.h"
#include "render.h"
#include "mesh_gen.h"
#include "graph/fg.h"
#include "post/post_internal.h"
#include "core/log.h"
#include "core/engine.h"
#include "stb_image_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI3D_MAX_TARGETS 16
#define UI3D_MAX_VIEWS 16
#define UI3D_MAX_PARTS 4096
#define UI3D_MAX_READBACKS 8

typedef struct Ui3dTarget {
    bool used;
    RenderTarget color;
    TextureId tex;
    FgRes res;
} Ui3dTarget;

typedef struct Ui3dJob {
    RenderUi3dView v;
    int first_part;
    int target;
    char png_path[256];
} Ui3dJob;

typedef struct Readback {
    GpuBuffer buf;
    int size;   // the PNG's size
    int src;    // the read-back target's size (a multiple of size: box-filtered down on the CPU)
    char path[256];
} Readback;

// Box filter of a square RGBA8 image by an integer factor, in premultiplied alpha (no dark fringes
// where the transparent background is averaged in). Icons bake at 4x and come out antialiased.
static void downsample_box(const u8 *src, int sw, u8 *dst, int dw) {
    int f = sw / dw;
    for (int y = 0; y < dw; y++)
        for (int x = 0; x < dw; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < f; j++)
                for (int i = 0; i < f; i++) {
                    const u8 *p = src + ((size_t)(y * f + j) * sw + (size_t)(x * f + i)) * 4;
                    float pa = p[3] / 255.0f;
                    r += p[0] * pa;
                    g += p[1] * pa;
                    b += p[2] * pa;
                    a += pa;
                }
            u8 *o = dst + ((size_t)y * dw + x) * 4;
            float inv = a > 0 ? 1.0f / a : 0.0f;
            o[0] = (u8)DG_CLAMP((int)(r * inv + 0.5f), 0, 255);
            o[1] = (u8)DG_CLAMP((int)(g * inv + 0.5f), 0, 255);
            o[2] = (u8)DG_CLAMP((int)(b * inv + 0.5f), 0, 255);
            o[3] = (u8)DG_CLAMP((int)(a / (float)(f * f) * 255.0f + 0.5f), 0, 255);
        }
}

static struct {
    // backdrop
    RenderTarget blur;
    TextureId blur_tex;
    bool blur_wanted;
    FgRes blur_res, blur_tmp, ldr;
    // item views
    Ui3dTarget targets[UI3D_MAX_TARGETS];
    RenderTarget depth;
    FgRes depth_res;
    RenderTarget bake;                  // readback downscale (PNG bakes)
    MeshId cube, cyl;
    Ui3dJob jobs[UI3D_MAX_VIEWS];
    int job_count;
    RenderUi3dPart parts[UI3D_MAX_PARTS];
    int part_count;
    Readback rb[UI3D_MAX_READBACKS];
    int rb_count;
} UR;

// ------------------------------------------------------------------ backdrop

TextureId render_ui_backdrop(void) {
    UR.blur_wanted = true;
    if (!UR.blur_tex) {
        // Created on the first request (any size: resized before the first use in render_ui_build).
        UR.blur = vkr_target_create(16, 16, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
        UR.blur_tex = vkr_texture_wrap(UR.blur.image, UR.blur.view, UR.blur.w, UR.blur.h, UR.blur.format);
    }
    return UR.blur_tex;
}

static void exec_blur(VkCommandBuffer cmd, void *u) {
    (void)u;
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    u32 w = UR.blur.w, h = UR.blur.h;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, UR.ldr, P.linear);
    pb_img(&b, 8, UR.blur.view);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    if (post_direct()) {   // Engine Fidelity Off: the source is the lit HDR (the same output transform)
        pc.u[0] = 1;
        pc.p[0] = post_direct_exposure();
    }
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "ui_blur_down", w, h, 1, 8);
    pc.u[0] = 0;
    pc.p[0] = 0.0f;
    // Two separable Gaussian iterations: blur -> tmp (H) -> blur (V), twice.
    for (int it = 0; it < 4; it++) {
        bool horiz = (it & 1) == 0;
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        PostBind c = { 0 };
        pb_ubo(&c);
        // Both images stay in GENERAL inside this pass (storage), sampled through GENERAL views.
        pb_tex_general(&c, 0, horiz ? UR.blur.view : fg_view(UR.blur_tmp), P.linear);
        pb_img(&c, 8, horiz ? fg_view(UR.blur_tmp) : UR.blur.view);
        pb_push(cmd, &c, VK_PIPELINE_BIND_POINT_COMPUTE);
        pc.u[0] = horiz ? 0u : 1u;
        post_push_constants(cmd, &pc, 128);
        post_dispatch(cmd, "ui_blur", w, h, 1, 8);
    }
}

// ------------------------------------------------------------------ item views

static Ui3dTarget *target_of(TextureId t) {
    for (int i = 0; i < UI3D_MAX_TARGETS; i++)
        if (UR.targets[i].used && UR.targets[i].tex == t) return &UR.targets[i];
    return NULL;
}

TextureId render_ui3d_target_create(int w, int h) {
    for (int i = 0; i < UI3D_MAX_TARGETS; i++) {
        Ui3dTarget *t = &UR.targets[i];
        if (t->used) continue;
        t->color = vkr_target_create((u32)w, (u32)h, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
        t->tex = vkr_texture_wrap(t->color.image, t->color.view, t->color.w, t->color.h, t->color.format);
        t->used = t->tex != 0;
        if (!t->used) { vkr_target_destroy(&t->color); return 0; }
        return t->tex;
    }
    log_warn("render: out of UI item view targets");
    return 0;
}

void render_ui3d_target_destroy(TextureId tex) {
    Ui3dTarget *t = target_of(tex);
    if (!t) return;
    vkr_texture_unwrap(t->tex);
    vkr_defer_image(t->color.image, t->color.view, t->color.alloc);
    memset(t, 0, sizeof(*t));
}

void render_ui3d(const RenderUi3dView *v) {
    Ui3dTarget *t = target_of(v->target);
    if (!R->frame_active) return;   // no frame to record into (minimized)
    if (!t || UR.job_count >= UI3D_MAX_VIEWS || UR.part_count + v->part_count > UI3D_MAX_PARTS) return;
    Ui3dJob *j = &UR.jobs[UR.job_count++];
    j->v = *v;
    j->target = (int)(t - UR.targets);
    j->first_part = UR.part_count;
    memcpy(UR.parts + UR.part_count, v->parts, sizeof(RenderUi3dPart) * (size_t)v->part_count);
    UR.part_count += v->part_count;
    j->png_path[0] = 0;
    if (v->png_path && v->png_path[0]) dg_strcpy(j->png_path, sizeof(j->png_path), v->png_path);
    j->v.parts = NULL;
    j->v.png_path = NULL;
}

static void ensure_meshes(void) {
    if (UR.cube) return;
    MeshBuilder mb;
    mb_init(&mb);
    mb_box(&mb, v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 0xffffffffu);
    UR.cube = mb_upload(&mb);
    mb_reset(&mb);
    mb_cylinder(&mb, v3(0, 0, 0), 0.5f, 0.5f, 24, 0xffffffffu);
    UR.cyl = mb_upload(&mb);
    mb_free(&mb);
}

static void ensure_depth(u32 w, u32 h) {
    if (UR.depth.image && UR.depth.w >= w && UR.depth.h >= h) return;
    if (UR.depth.image) vkr_defer_image(UR.depth.image, UR.depth.view, UR.depth.alloc);
    u32 nw = DG_MAX(w, UR.depth.w), nh = DG_MAX(h, UR.depth.h);
    UR.depth = vkr_target_create(nw, nh, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

static void ensure_bake(void) {
    if (UR.bake.image) return;
    UR.bake = vkr_target_create(512, 512, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
}

typedef struct Ui3dPush { mat4 mvp; vec4 color; vec4 r0, r1, r2; } Ui3dPush;

static void blit(VkCommandBuffer cmd, VkImage src, int sw, int sh, VkImage dst, int dx, int dy, int dw, int dh) {
    VkImageBlit bl = { 0 };
    bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = 1;
    bl.srcOffsets[1] = (VkOffset3D){ sw, sh, 1 };
    bl.dstOffsets[0] = (VkOffset3D){ dx, dy, 0 };
    bl.dstOffsets[1] = (VkOffset3D){ dx + dw, dy + dh, 1 };
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
}

static void exec_ui3d(VkCommandBuffer cmd, void *u) {
    (void)u;
    for (int ji = 0; ji < UR.job_count; ji++) {
        Ui3dJob *j = &UR.jobs[ji];
        Ui3dTarget *t = &UR.targets[j->target];
        if (!t->used) continue;
        VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = t->color.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = (VkClearColorValue){ { j->v.clear.x, j->v.clear.y, j->v.clear.z, j->v.clear.w } };
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = UR.depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea.extent.width = t->color.w;
        ri.renderArea.extent.height = t->color.h;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp = { 0, 0, (float)t->color.w, (float)t->color.h, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { t->color.w, t->color.h } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R->pipes[PIPE_UI3D]);
        mat4 vpm = mat4_mul(j->v.proj, j->v.view);
        MeshId bound = 0;
        for (int k = 0; k < j->v.part_count; k++) {
            const RenderUi3dPart *p = &UR.parts[j->first_part + k];
            MeshId mid = p->shape == 1 ? UR.cyl : UR.cube;
            GpuMesh *m = &R->meshes[mid];
            if (!m->used) continue;
            if (mid != bound) {
                VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m->vb.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                bound = mid;
            }
            mat4 model = mat4_trs(p->center, p->rot, v3_scale(p->half, 2.0f));
            mat4 mv = mat4_mul(j->v.view, mat4_trs(p->center, p->rot, v3(1, 1, 1)));
            Ui3dPush pc;
            pc.mvp = mat4_mul(vpm, model);
            pc.color = v4(p->color.x, p->color.y, p->color.z, p->emissive);
            pc.r0 = v4(mv.m[0], mv.m[1], mv.m[2], p->highlight);
            pc.r1 = v4(mv.m[4], mv.m[5], mv.m[6], p->metallic);
            pc.r2 = v4(mv.m[8], mv.m[9], mv.m[10], 0);
            vkCmdPushConstants(cmd, R->layout_ui3d, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);
            R->stats.draw_calls++;
        }
        vkCmdEndRendering(cmd);

        bool copy = j->v.copy_to && j->v.copy_to < MAX_TEXTURES && R->textures[j->v.copy_to].used && R->textures[j->v.copy_to].ready;
        bool png = j->png_path[0] && UR.rb_count < UI3D_MAX_READBACKS;
        if (!copy && !png) continue;
        vkr_image_barrier(cmd, t->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT, 1);
        if (copy) {
            GpuTexture *dst = &R->textures[j->v.copy_to];
            vkr_image_barrier(cmd, dst->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
            blit(cmd, t->color.image, (int)t->color.w, (int)t->color.h, dst->image, j->v.copy_x, j->v.copy_y, j->v.copy_w, j->v.copy_h);
            vkr_image_barrier(cmd, dst->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
        }
        int full = (int)t->color.w;
        int ps = DG_CLAMP(j->v.png_size > 0 ? j->v.png_size : full, 8, 512);
        if (png && full > ps && full % ps == 0 && full == (int)t->color.h) {
            // Supersampled bake: the whole target to the CPU, box-filtered there (render_ui_frame_done).
            Readback *rb = &UR.rb[UR.rb_count++];
            rb->size = ps;
            rb->src = full;
            dg_strcpy(rb->path, sizeof(rb->path), j->png_path);
            rb->buf = vkr_buffer_create_readback((VkDeviceSize)full * full * 4);
            VkBufferImageCopy region = { 0 };
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = (u32)full;
            region.imageExtent.height = (u32)full;
            region.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cmd, t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb->buf.buffer, 1, &region);
        } else if (png) {
            ensure_bake();
            int s = ps;
            vkr_image_barrier(cmd, UR.bake.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
            blit(cmd, t->color.image, (int)t->color.w, (int)t->color.h, UR.bake.image, 0, 0, s, s);
            vkr_image_barrier(cmd, UR.bake.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT, 1);
            Readback *rb = &UR.rb[UR.rb_count++];
            rb->size = s;
            rb->src = s;
            dg_strcpy(rb->path, sizeof(rb->path), j->png_path);
            rb->buf = vkr_buffer_create_readback((VkDeviceSize)s * s * 4);
            VkBufferImageCopy region = { 0 };
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = (u32)s;
            region.imageExtent.height = (u32)s;
            region.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cmd, UR.bake.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb->buf.buffer, 1, &region);
        }
        vkr_image_barrier(cmd, t->color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1);
    }
}

// ------------------------------------------------------------------ frame graph hooks (render.c)

void render_ui_build(FgRes ldr, u32 out_w, u32 out_h) {
    UR.blur_res = UR.blur_tmp = FG_NONE;
    UR.ldr = ldr;
    if (UR.blur_wanted && UR.blur_tex && ldr != FG_NONE) {   // (fg_valid is false for transients until execution)
        u32 bw = DG_MAX(1u, out_w / 4), bh = DG_MAX(1u, out_h / 4);
        if (UR.blur.w != bw || UR.blur.h != bh) {
            vkr_defer_image(UR.blur.image, UR.blur.view, UR.blur.alloc);
            UR.blur = vkr_target_create(bw, bh, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT);
            vkr_texture_rewrap(UR.blur_tex, UR.blur.image, UR.blur.view, bw, bh);
        }
        UR.blur_res = fg_import("ui_blur", UR.blur.image, UR.blur.view, UR.blur.format, bw, bh, VK_IMAGE_ASPECT_COLOR_BIT, &UR.blur.layout);
        FgTexDesc td = { "ui_blur_tmp", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, bw, bh, 1, 1, 0 };
        UR.blur_tmp = fg_texture(&td);
        int p = fg_pass("ui_blur", exec_blur, NULL);
        fg_read(p, ldr, FG_SAMPLED);
        fg_write(p, UR.blur_res, FG_STORAGE);
        fg_write(p, UR.blur_tmp, FG_STORAGE);
        if (p < 0) UR.blur_res = FG_NONE;
    }
    UR.blur_wanted = false;

    for (int i = 0; i < UI3D_MAX_TARGETS; i++) {
        Ui3dTarget *t = &UR.targets[i];
        t->res = t->used ? fg_import("ui_view", t->color.image, t->color.view, t->color.format, t->color.w, t->color.h, VK_IMAGE_ASPECT_COLOR_BIT,
                                     &t->color.layout)
                         : FG_NONE;
    }
    UR.depth_res = FG_NONE;
    if (UR.job_count) {
        ensure_meshes();
        u32 mw = 1, mh = 1;
        for (int j = 0; j < UR.job_count; j++) {
            Ui3dTarget *t = &UR.targets[UR.jobs[j].target];
            mw = DG_MAX(mw, t->color.w);
            mh = DG_MAX(mh, t->color.h);
        }
        ensure_depth(mw, mh);
        UR.depth_res = fg_import("ui_view_depth", UR.depth.image, UR.depth.view, UR.depth.format, UR.depth.w, UR.depth.h, VK_IMAGE_ASPECT_DEPTH_BIT,
                                 &UR.depth.layout);
        int p = fg_pass("ui_views", exec_ui3d, NULL);
        fg_write(p, UR.depth_res, FG_DEPTH);
        bool written[UI3D_MAX_TARGETS] = { 0 };
        for (int j = 0; j < UR.job_count; j++) {
            int ti = UR.jobs[j].target;
            if (!written[ti]) fg_write(p, UR.targets[ti].res, FG_COLOR);
            written[ti] = true;
        }
    }
}

// The final (UI) pass samples the backdrop and every item view.
void render_ui_final_reads(int pass) {
    if (UR.blur_res != FG_NONE) fg_read(pass, UR.blur_res, FG_SAMPLED);
    for (int i = 0; i < UI3D_MAX_TARGETS; i++)
        if (UR.targets[i].used && UR.targets[i].res != FG_NONE) fg_read(pass, UR.targets[i].res, FG_SAMPLED);
}

bool render_ui_wants_wait(void) { return UR.rb_count > 0; }

// After the frame's fence: write the PNG readbacks; reset the per-frame lists.
void render_ui_frame_done(void) {
    for (int i = 0; i < UR.rb_count; i++) {
        Readback *rb = &UR.rb[i];
        vmaInvalidateAllocation(R->vma, rb->buf.alloc, 0, VK_WHOLE_SIZE);
        const u8 *px = (const u8 *)rb->buf.mapped;
        u8 *small = NULL;
        if (rb->src > rb->size) {
            small = (u8 *)malloc((size_t)rb->size * rb->size * 4);
            downsample_box(px, rb->src, small, rb->size);
            px = small;
        }
        bool ok = stbi_write_png(rb->path, rb->size, rb->size, 4, px, rb->size * 4) != 0;
        free(small);
        if (ok) log_debug("ui: wrote %s", rb->path);
        else log_error("ui: failed to write %s", rb->path);
        vkr_buffer_destroy_now(&rb->buf);
    }
    UR.rb_count = 0;
    UR.job_count = 0;
    UR.part_count = 0;
}

void render_ui_shutdown(void) {
    for (int i = 0; i < UR.rb_count; i++) vkr_buffer_destroy_now(&UR.rb[i].buf);
    UR.rb_count = 0;
    for (int i = 0; i < UI3D_MAX_TARGETS; i++)
        if (UR.targets[i].used) vkr_target_destroy(&UR.targets[i].color);
    vkr_target_destroy(&UR.depth);
    vkr_target_destroy(&UR.bake);
    vkr_target_destroy(&UR.blur);
    memset(&UR, 0, sizeof(UR));
}
