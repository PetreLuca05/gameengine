#include "vgeo_cull.h"
#include "core/log.h"
#include <stddef.h>
#include <string.h>

#define NBIND 18

static const VkBufferUsageFlags SSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

static bool create_layouts(VgCuller *c) {
    VkDevice d = c->ctx->device;
    VkDescriptorSetLayoutBinding b[NBIND];
    memset(b, 0, sizeof(b));
    for (u32 i = 0; i < NBIND; i++) {
        b[i].binding = i;
        b[i].descriptorType = i >= 16 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = NBIND;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &li, NULL, &c->layout) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding h[3];
    memset(h, 0, sizeof(h));
    for (u32 i = 0; i < 3; i++) {
        h[i].binding = i;
        h[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        h[i].descriptorCount = 1;
        h[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    li.bindingCount = 3;
    li.pBindings = h;
    if (vkCreateDescriptorSetLayout(d, &li, NULL, &c->hiz_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VgPush) };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &c->layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(d, &pi, NULL, &c->pl) != VK_SUCCESS) return false;
    pr.size = 4;
    pi.pSetLayouts = &c->hiz_layout;
    if (vkCreatePipelineLayout(d, &pi, NULL, &c->hiz_pl) != VK_SUCCESS) return false;
    u32 frames = c->ctx->frames;
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 * VG_MAX_VIEWS * frames },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * VG_MAX_VIEWS * frames + 16 * frames },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 16 * frames },
    };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = VG_MAX_VIEWS * frames + 16 * frames;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d, &dp, NULL, &c->pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = c->pool;
    ai.descriptorSetCount = 1;
    for (u32 f = 0; f < frames; f++) {
        ai.pSetLayouts = &c->layout;
        for (u32 v = 0; v < VG_MAX_VIEWS; v++)
            if (vkAllocateDescriptorSets(d, &ai, &c->v[v].set[f]) != VK_SUCCESS) return false;
        ai.pSetLayouts = &c->hiz_layout;
        for (u32 m = 0; m < 16; m++)
            if (vkAllocateDescriptorSets(d, &ai, &c->hiz_sets[f][m]) != VK_SUCCESS) return false;
    }
    return true;
}

static bool create_dummy(VgCuller *c) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32_SFLOAT;
    ii.extent.width = ii.extent.height = ii.extent.depth = 1;
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(c->ctx->vma, &ii, &ai, &c->dummy, &c->dummy_alloc, NULL) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = c->dummy;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(c->ctx->device, &vi, NULL, &c->dummy_view) != VK_SUCCESS) return false;
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(c->ctx, &pool);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = c->dummy;
    b.subresourceRange = vi.subresourceRange;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    vgc_submit_wait(c->ctx, pool, cmd);
    return true;
}

bool vg_cull_init(VgCuller *c, const VgContext *ctx, VgScene *scene, const VgCullCaps *caps) {
    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
    c->scene = scene;
    c->caps = *caps;
    if (ctx->frames > VG_MAX_FRAMES) { log_error("vgeo: %u frames in flight > %d", ctx->frames, VG_MAX_FRAMES); return false; }
    if (!create_layouts(c) || !create_dummy(c)) { log_error("vgeo: cull layouts failed"); return false; }
    c->p_args = vgc_compute_pipeline(ctx, c->pl, "vgeo/vg_args.comp");
    c->p_cells = vgc_compute_pipeline(ctx, c->pl, "vgeo/vg_cells.comp");
    c->p_inst = vgc_compute_pipeline(ctx, c->pl, "vgeo/vg_instances.comp");
    c->p_trav = vgc_compute_pipeline(ctx, c->pl, "vgeo/vg_traverse.comp");
    c->p_clus = vgc_compute_pipeline(ctx, c->pl, "vgeo/vg_clusters.comp");
    c->p_hiz = vgc_compute_pipeline(ctx, c->hiz_pl, "vgeo/vg_hiz.comp");
    if (!c->p_args || !c->p_cells || !c->p_inst || !c->p_trav || !c->p_clus || !c->p_hiz) return false;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 16.0f;
    if (vkCreateSampler(ctx->device, &si, NULL, &c->sampler) != VK_SUCCESS) return false;
    c->views = vgc_buffer(ctx, sizeof(VgViewGpu) * VG_MAX_VIEWS, SSBO, VGM_DEVICE);
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = VGT_COUNT * VG_MAX_VIEWS * ctx->frames;
    vkCreateQueryPool(ctx->device, &qi, NULL, &c->qp);
    c->ok = true;
    return true;
}

static void free_view(VgCuller *c, VgViewRes *v) {
    const VgContext *x = c->ctx;
    vgc_buffer_free(x, &v->counters);
    vgc_buffer_free(x, &v->cell_list);
    vgc_buffer_free(x, &v->inst_list);
    vgc_buffer_free(x, &v->node_queue);
    vgc_buffer_free(x, &v->group_items);
    vgc_buffer_free(x, &v->visible);
    vgc_buffer_free(x, &v->occluded);
    vgc_buffer_free(x, &v->requests);
    for (u32 f = 0; f < VG_MAX_FRAMES; f++) vgc_buffer_free(x, &v->readback[f]);
    v->allocated = false;
}

void vg_cull_shutdown(VgCuller *c) {
    if (!c->ctx) return;
    VkDevice d = c->ctx->device;
    vkDeviceWaitIdle(d);
    for (u32 i = 0; i < VG_MAX_VIEWS; i++) free_view(c, &c->v[i]);
    vgc_buffer_free(c->ctx, &c->views);
    VkPipeline ps[] = { c->p_args, c->p_cells, c->p_inst, c->p_trav, c->p_clus, c->p_hiz };
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) if (ps[i]) vkDestroyPipeline(d, ps[i], NULL);
    if (c->qp) vkDestroyQueryPool(d, c->qp, NULL);
    if (c->sampler) vkDestroySampler(d, c->sampler, NULL);
    if (c->dummy_view) vkDestroyImageView(d, c->dummy_view, NULL);
    if (c->dummy) vmaDestroyImage(c->ctx->vma, c->dummy, c->dummy_alloc);
    if (c->pool) vkDestroyDescriptorPool(d, c->pool, NULL);
    if (c->pl) vkDestroyPipelineLayout(d, c->pl, NULL);
    if (c->hiz_pl) vkDestroyPipelineLayout(d, c->hiz_pl, NULL);
    if (c->layout) vkDestroyDescriptorSetLayout(d, c->layout, NULL);
    if (c->hiz_layout) vkDestroyDescriptorSetLayout(d, c->hiz_layout, NULL);
    memset(c, 0, sizeof(*c));
}

// Per-view buffers, sized by the caps and the scene (cells / instances); re-made when the scene grows.
static void ensure_view(VgCuller *c, VgViewRes *v) {
    const VgScene *s = c->scene;
    u32 cells = s->cell_count ? s->cell_count : 1, insts = s->instance_count ? s->instance_count : 1;
    if (v->allocated && v->cells_cap >= cells && v->inst_cap >= insts) return;
    if (v->allocated) { vkDeviceWaitIdle(c->ctx->device); free_view(c, v); }
    const VgContext *x = c->ctx;
    v->cells_cap = cells;
    v->inst_cap = insts;
    v->counters = vgc_buffer(x, sizeof(VgCounters), SSBO | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VGM_DEVICE);
    v->cell_list = vgc_buffer(x, sizeof(u32) * 2ull * cells, SSBO, VGM_DEVICE);
    v->inst_list = vgc_buffer(x, sizeof(u32) * (VkDeviceSize)insts, SSBO, VGM_DEVICE);
    v->node_queue = vgc_buffer(x, 8ull * 3 * c->caps.nodes, SSBO, VGM_DEVICE);
    v->group_items = vgc_buffer(x, 8ull * 2 * c->caps.groups, SSBO, VGM_DEVICE);
    v->visible = vgc_buffer(x, 8ull * 2 * c->caps.visible, SSBO, VGM_DEVICE);
    v->occluded = vgc_buffer(x, 8ull * c->caps.occluded, SSBO, VGM_DEVICE);
    v->requests = vgc_buffer(x, 8ull * c->caps.requests, SSBO, VGM_DEVICE);
    for (u32 f = 0; f < x->frames; f++) {
        v->readback[f] = vgc_buffer(x, sizeof(VgCounters), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
        v->readback_valid[f] = false;
        v->set_gen[f] = ~0u;
    }
    v->allocated = true;
}

static void write_set(VgCuller *c, VgViewRes *v, u32 f, VkImageView prev, VkImageLayout prev_l, VkImageView cur, VkImageLayout cur_l) {
    const VgScene *s = c->scene;
    if (v->set_gen[f] == s->generation && v->set_prev[f] == prev && v->set_cur[f] == cur) return;
    const VgBuffer *bufs[16] = { &c->views, &s->b_meshes, &s->b_nodes, &s->b_groups, &s->b_page_table, &s->b_pool, &s->b_instances, &s->b_cells,
                                 &v->counters, &v->cell_list, &v->inst_list, &v->node_queue, &v->group_items, &v->visible, &v->occluded, &v->requests };
    VkDescriptorBufferInfo bi[16];
    VkDescriptorImageInfo ii[2] = { { c->sampler, prev, prev_l }, { c->sampler, cur, cur_l } };
    VkWriteDescriptorSet w[NBIND];
    memset(w, 0, sizeof(w));
    for (u32 i = 0; i < NBIND; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = v->set[f];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        if (i < 16) {
            bi[i].buffer = bufs[i]->buffer;
            bi[i].offset = 0;
            bi[i].range = VK_WHOLE_SIZE;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        } else {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &ii[i - 16];
        }
    }
    vkUpdateDescriptorSets(c->ctx->device, NBIND, w, 0, NULL);
    v->set_gen[f] = s->generation;
    v->set_prev[f] = prev;
    v->set_cur[f] = cur;
}

void vg_cull_collect(VgCuller *c, u32 frame) {
    for (u32 i = 0; i < VG_MAX_VIEWS; i++) {
        VgViewRes *v = &c->v[i];
        if (!v->allocated || !v->readback_valid[frame]) continue;
        vmaInvalidateAllocation(c->ctx->vma, v->readback[frame].alloc, 0, VK_WHOLE_SIZE);
        memcpy(&v->last, v->readback[frame].mapped, sizeof(VgCounters));
        v->last_valid = true;
        v->readback_valid[frame] = false;
    }
}

bool vg_cull_counters(const VgCuller *c, u32 slot, VgCounters *out) {
    if (slot >= VG_MAX_VIEWS || !c->v[slot].last_valid) return false;
    *out = c->v[slot].last;
    return true;
}

bool vg_cull_timings(const VgCuller *c, u32 frame, u32 slot, bool wait, float ms[4]) {
    if (!c->qp || !c->qp_written[frame][slot]) return false;
    u64 ts[VGT_COUNT];
    u32 first = (frame * VG_MAX_VIEWS + slot) * VGT_COUNT;
    VkQueryResultFlags fl = VK_QUERY_RESULT_64_BIT | (wait ? VK_QUERY_RESULT_WAIT_BIT : 0);
    if (vkGetQueryPoolResults(c->ctx->device, c->qp, first, VGT_COUNT, sizeof(ts), ts, sizeof(u64), fl) != VK_SUCCESS) return false;
    float p = c->ctx->timestamp_period * 1e-6f;
    for (int i = 0; i < 4; i++) ms[i] = (float)(ts[i + 1] - ts[i]) * p;
    return true;
}

typedef struct Rec { VgCuller *c; VkCommandBuffer cmd; VgViewRes *v; u32 slot; } Rec;

static void push(Rec *r, u32 pass, u32 in, u32 out, const u32 *a, int na) {
    VgPush p;
    memset(&p, 0, sizeof(p));
    p.pass = pass;
    p.in_region = in;
    p.out_region = out;
    p.view = r->slot;
    for (int i = 0; i < na; i++) p.a[i] = a[i];
    vkCmdPushConstants(r->cmd, r->c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
}

static void dispatch(Rec *r, VkPipeline p, u32 x) {
    vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdDispatch(r->cmd, x ? x : 1, 1, 1);
    vgc_barrier(r->cmd);
    r->c->dispatches++;
}

static void dispatch_indirect(Rec *r, VkPipeline p, u32 args) {
    vkCmdBindPipeline(r->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdDispatchIndirect(r->cmd, r->v->counters.buffer, (VkDeviceSize)args * 16);
    vgc_barrier(r->cmd);
    r->c->dispatches++;
}

// args[dst] from n[src] / div; reset: counter cleared after (+1 encoded), cap on the source count.
static void args(Rec *r, u32 src, u32 dst, u32 div, int reset, u32 cap) {
    u32 a[5] = { div, reset >= 0 ? (u32)reset + 1 : 0, 0, 0, cap };
    push(r, 0, src, dst, a, 5);
    dispatch(r, r->c->p_args, 1);
}

static void stamp(Rec *r, u32 frame, u32 i) {
    if (!r->c->qp) return;
    vkCmdWriteTimestamp2(r->cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, r->c->qp, (frame * VG_MAX_VIEWS + r->slot) * VGT_COUNT + i);
}

static void traverse(Rec *r, u32 pass, u32 first_in, u32 depth) {
    u32 in = first_in, out = 0;
    for (u32 l = 0; l < depth; l++) {
        out = in == 0 ? 1 : 0;
        args(r, VGN_NODEQ0 + in, VGA_NODE, 64, (int)(VGN_NODEQ0 + out), r->c->caps.nodes);
        push(r, pass, in, out, NULL, 0);
        dispatch_indirect(r, r->c->p_trav, VGA_NODE);
        in = out;
    }
}

void vg_cull_record(VgCuller *c, VkCommandBuffer cmd, u32 frame, u32 slot, const VgViewGpu *view, const VgHizRef *prev, const VgHizRef *cur) {
    VgScene *s = c->scene;
    c->dispatches = 0;
    if (!c->ok || !s->instance_count || slot >= VG_MAX_VIEWS) return;
    VgViewRes *v = &c->v[slot];
    ensure_view(c, v);
    bool prev_ok = prev && prev->view, cur_ok = cur && cur->view;
    write_set(c, v, frame, prev_ok ? prev->view : c->dummy_view, prev_ok ? prev->layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              cur_ok ? cur->view : c->dummy_view, cur_ok ? cur->layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VgViewGpu g = *view;
    g.cell_count = s->cell_count;
    g.instance_count = s->instance_count;
    g.max_depth = s->max_depth;
    g.cap_nodes = c->caps.nodes;
    g.cap_groups = c->caps.groups;
    g.cap_visible = c->caps.visible;
    g.cap_occluded = c->caps.occluded;
    g.cap_cells = v->cells_cap;
    g.cap_instances = v->inst_cap;
    g.cap_requests = c->caps.requests;
    if (prev_ok) { g.hiz_prev[0] = (float)prev->w; g.hiz_prev[1] = (float)prev->h; g.hiz_prev[2] = (float)prev->mips; g.hiz_prev[3] = 1.0f; }
    if (cur_ok) { g.hiz_cur[0] = (float)cur->w; g.hiz_cur[1] = (float)cur->h; g.hiz_cur[2] = (float)cur->mips; g.hiz_cur[3] = 1.0f; }
    bool two_pass = (g.flags & VGV_TWO_PASS) && cur_ok && !(g.flags & VGV_NO_OCCLUSION);
    Rec r = { c, cmd, v, slot };
    if (c->qp) vkCmdResetQueryPool(cmd, c->qp, (frame * VG_MAX_VIEWS + slot) * VGT_COUNT, VGT_COUNT);
    vkCmdUpdateBuffer(cmd, c->views.buffer, sizeof(VgViewGpu) * slot, sizeof(VgViewGpu), &g);
    vkCmdFillBuffer(cmd, v->counters.buffer, 0, VK_WHOLE_SIZE, 0);
    vgc_barrier(cmd);
    stamp(&r, frame, VGT_BEGIN);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &v->set[frame], 0, NULL);

    // ---- pass 1
    push(&r, 1, 0, 0, NULL, 0);
    dispatch(&r, c->p_cells, (s->cell_count + 63) / 64);
    args(&r, VGN_CELLS_VIS, VGA_INST, 1, -1, v->cells_cap);
    u32 a0[1] = { 0 }, big1[2] = { 0, VGN_CELLS_VIS };
    push(&r, 1, 0, 0, big1, 2);
    dispatch_indirect(&r, c->p_inst, VGA_INST);
    stamp(&r, frame, VGT_INSTANCES);
    u32 depth = s->max_depth > 1 ? s->max_depth - 1 : 0;   // the instance step expands the roots
    traverse(&r, 1, 0, depth);
    stamp(&r, frame, VGT_TRAVERSE);
    args(&r, VGN_GROUPS0, VGA_GROUP, 64, -1, c->caps.groups);
    push(&r, 1, 0, 0, a0, 1);
    dispatch_indirect(&r, c->p_clus, VGA_GROUP);
    // pass 1 list ends (HW, SW -> HW_P1, SW_P1)
    VkBufferCopy bc = { offsetof(VgCounters, n) + VGN_HW * 4, offsetof(VgCounters, n) + VGN_HW_P1 * 4, 8 };
    vkCmdCopyBuffer(cmd, v->counters.buffer, v->counters.buffer, 1, &bc);
    vgc_barrier(cmd);
    stamp(&r, frame, VGT_CLUSTERS);

    // ---- pass 2: pass 1's occluders against this frame's Hi-Z
    if (two_pass) {
        u32 a1[1] = { 1 }, big2[2] = { 0, VGN_CELLS_P2 };
        args(&r, VGN_CELLS_OCC, VGA_INST, 64, -1, v->cells_cap);
        push(&r, 2, 0, 2, a1, 1);
        dispatch_indirect(&r, c->p_cells, VGA_INST);
        args(&r, VGN_CELLS_P2, VGA_INST, 1, -1, v->cells_cap);
        push(&r, 2, 0, 2, big2, 2);
        dispatch_indirect(&r, c->p_inst, VGA_INST);
        args(&r, VGN_INST_OCC, VGA_INSTLIST, 64, -1, v->inst_cap);
        push(&r, 2, 0, 2, a1, 1);
        dispatch_indirect(&r, c->p_inst, VGA_INSTLIST);
        traverse(&r, 2, 2, depth);
        args(&r, VGN_GROUPS0 + 1, VGA_GROUP, 64, -1, c->caps.groups);
        push(&r, 2, 1, 0, a0, 1);
        dispatch_indirect(&r, c->p_clus, VGA_GROUP);
        args(&r, VGN_CL_OCC, VGA_OCCL, 64, -1, c->caps.occluded);
        push(&r, 2, 0, 0, a1, 1);
        dispatch_indirect(&r, c->p_clus, VGA_OCCL);
    }
    stamp(&r, frame, VGT_PASS2);
    if (c->qp) c->qp_written[frame][slot] = true;
    VkBufferCopy rc = { 0, 0, sizeof(VgCounters) };
    vkCmdCopyBuffer(cmd, v->counters.buffer, v->readback[frame].buffer, 1, &rc);
    VkMemoryBarrier2 hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    hb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &hb;
    vkCmdPipelineBarrier2(cmd, &dep);
    v->readback_valid[frame] = true;
}

void vg_hiz_record(VgCuller *c, VkCommandBuffer cmd, u32 frame, VkImageView depth, VkImageLayout depth_layout, const VkImageView *mips, u32 mip_count,
                   u32 w, u32 h) {
    if (!c->ok) return;
    if (mip_count > 16) mip_count = 16;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->p_hiz);
    for (u32 m = 0; m < mip_count; m++) {
        VkDescriptorSet set = c->hiz_sets[frame][m];
        VkDescriptorImageInfo ii[3] = { { c->sampler, depth, depth_layout }, { VK_NULL_HANDLE, mips[m ? m - 1 : 0], VK_IMAGE_LAYOUT_GENERAL },
                                        { VK_NULL_HANDLE, mips[m], VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet wr[3];
        memset(wr, 0, sizeof(wr));
        for (int i = 0; i < 3; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = set;
            wr[i].dstBinding = (u32)i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wr[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(c->ctx->device, 3, wr, 0, NULL);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->hiz_pl, 0, 1, &set, 0, NULL);
        u32 mip = m;
        vkCmdPushConstants(cmd, c->hiz_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &mip);
        u32 mw = (w + 1) / 2, mh = (h + 1) / 2;
        for (u32 k = 0; k < m; k++) { mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; }
        vkCmdDispatch(cmd, (mw + 7) / 8, (mh + 7) / 8, 1);
        vgc_barrier(cmd);
    }
}
