#include "vgeo_raster.h"
#include "core/log.h"
#include <string.h>

static VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits s, VkShaderModule m) {
    VkPipelineShaderStageCreateInfo i = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    i.stage = s;
    i.module = m;
    i.pName = "main";
    return i;
}

// No attachments, no depth test: the fragment atomicMax is the depth test. CCW front faces, back faces
// culled (as the legacy pipelines in vk_pipelines.c), or no culling (two-sided materials).
static VkPipeline graphics_pipeline(VgRaster *r, const char *first, VkShaderStageFlagBits first_stage, const char *frag, bool two_sided) {
    const VgContext *c = r->ctx;
    VkShaderModule m0 = c->load_shader(first), m1 = c->load_shader(frag);
    VkPipeline p = VK_NULL_HANDLE;
    if (m0 && m1) {
        VkPipelineShaderStageCreateInfo st[2] = { stage(first_stage, m0), stage(VK_SHADER_STAGE_FRAGMENT_BIT, m1) };
        bool mesh = first_stage == VK_SHADER_STAGE_MESH_BIT_EXT;
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gi.pNext = &ri;
        gi.stageCount = 2;
        gi.pStages = st;
        gi.pVertexInputState = mesh ? NULL : &vi;
        gi.pInputAssemblyState = mesh ? NULL : &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pDynamicState = &ds;
        gi.layout = r->pl;
        if (vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &p) != VK_SUCCESS) {
            log_error("vgeo: pipeline %s failed", first);
            p = VK_NULL_HANDLE;
        }
    }
    if (m0) vkDestroyShaderModule(c->device, m0, NULL);
    if (m1) vkDestroyShaderModule(c->device, m1, NULL);
    return p;
}

bool vg_raster_init(VgRaster *r, const VgContext *ctx, VgCuller *cull) {
    memset(r, 0, sizeof(*r));
    r->ctx = ctx;
    r->cull = cull;
    if (!cull->ok) return false;
    r->push_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    if (ctx->mesh_shader) r->push_stages |= VK_SHADER_STAGE_MESH_BIT_EXT;
    VkPushConstantRange pr = { r->push_stages, 0, sizeof(VgPush) };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &cull->layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(ctx->device, &pi, NULL, &r->pl) != VK_SUCCESS) return false;
    r->p_args = vgc_compute_pipeline(ctx, r->pl, "vgeo/vg_raster_args.comp");
    r->p_sw = vgc_compute_pipeline(ctx, r->pl, "vgeo/vg_raster_sw.comp");
    r->p_vert = graphics_pipeline(r, "vgeo/vg_raster_hw.vert", VK_SHADER_STAGE_VERTEX_BIT, "vgeo/vg_raster_hw.frag", false);
    r->p_vert2 = graphics_pipeline(r, "vgeo/vg_raster_hw.vert", VK_SHADER_STAGE_VERTEX_BIT, "vgeo/vg_raster_hw.frag", true);
    if (ctx->mesh_shader) {
        r->p_mesh = graphics_pipeline(r, "vgeo/vg_raster_hw.mesh", VK_SHADER_STAGE_MESH_BIT_EXT, "vgeo/vg_raster_mesh.frag", false);
        r->p_mesh2 = graphics_pipeline(r, "vgeo/vg_raster_hw.mesh", VK_SHADER_STAGE_MESH_BIT_EXT, "vgeo/vg_raster_mesh.frag", true);
    }
    if (!r->p_args || !r->p_sw || !r->p_vert || !r->p_vert2 || (r->p_mesh && !r->p_mesh2)) { log_error("vgeo: raster pipelines failed"); return false; }
    r->use_mesh = r->p_mesh != VK_NULL_HANDLE;
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = VGR_COUNT * 2 * VG_MAX_VIEWS * ctx->frames;
    vkCreateQueryPool(ctx->device, &qi, NULL, &r->qp);
    r->ok = true;
    return true;
}

void vg_raster_shutdown(VgRaster *r) {
    if (!r->ctx) return;
    VkDevice d = r->ctx->device;
    vkDeviceWaitIdle(d);
    if (r->cull && r->cull->vis_buffer == r->vis.buffer) vg_cull_set_vis(r->cull, VK_NULL_HANDLE);
    vgc_buffer_free(r->ctx, &r->vis);
    VkPipeline ps[] = { r->p_args, r->p_sw, r->p_vert, r->p_mesh, r->p_vert2, r->p_mesh2 };
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) if (ps[i]) vkDestroyPipeline(d, ps[i], NULL);
    if (r->qp) vkDestroyQueryPool(d, r->qp, NULL);
    if (r->pl) vkDestroyPipelineLayout(d, r->pl, NULL);
    memset(r, 0, sizeof(*r));
}

void vg_raster_resize(VgRaster *r, u32 w, u32 h) {
    if (!r->ok || (r->vis.buffer && r->w == w && r->h == h)) return;
    if (r->vis.buffer) {
        vkDeviceWaitIdle(r->ctx->device);
        vgc_buffer_free(r->ctx, &r->vis);
    }
    r->w = w;
    r->h = h;
    r->vis = vgc_buffer(r->ctx, 8ull * w * h, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_DEVICE);
    vg_cull_set_vis(r->cull, r->vis.buffer);
}

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 src_a, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_a) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = src;
    mb.srcAccessMask = src_a;
    mb.dstStageMask = dst;
    mb.dstAccessMask = dst_a;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static VkPipelineStageFlags2 raster_stages(const VgRaster *r) {
    VkPipelineStageFlags2 s = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (r->p_mesh) s |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    return s;
}

void vg_raster_clear(VgRaster *r, VkCommandBuffer cmd) {
    if (!r->ok || !r->vis.buffer) return;
    vkCmdFillBuffer(cmd, r->vis.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, raster_stages(r),
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

static void stamp(VgRaster *r, VkCommandBuffer cmd, u32 base, u32 i) {
    if (r->qp) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r->qp, base + i);
}

void vg_raster_record(VgRaster *r, VkCommandBuffer cmd, u32 frame, u32 slot, u32 pass) {
    VgCuller *c = r->cull;
    if (!r->ok || !r->vis.buffer || slot >= VG_MAX_VIEWS || !c->v[slot].allocated || pass < 1 || pass > 2) return;
    VkBuffer counters = c->v[slot].counters.buffer;
    VkDescriptorSet set = vg_cull_set(c, frame, slot);
    u32 q = ((frame * VG_MAX_VIEWS + slot) * 2 + pass - 1) * VGR_COUNT;
    if (r->qp) vkCmdResetQueryPool(cmd, r->qp, q, VGR_COUNT);
    VgPush p;
    memset(&p, 0, sizeof(p));
    p.pass = pass;
    p.view = slot;

    // Args from the counters of this pass's range.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->pl, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, r->pl, r->push_stages, 0, sizeof(p), &p);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->p_args);
    vkCmdDispatch(cmd, 1, 1, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | raster_stages(r),
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    stamp(r, cmd, q, VGR_BEGIN);

    // HW
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = r->w;
    ri.renderArea.extent.height = r->h;
    ri.layerCount = 1;
    vkCmdBeginRendering(cmd, &ri);
    bool mesh = r->use_mesh && r->p_mesh;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh ? r->p_mesh : r->p_vert);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pl, 0, 1, &set, 0, NULL);
    VkViewport vp = { 0, 0, (float)r->w, (float)r->h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { r->w, r->h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    if (mesh) vkCmdDrawMeshTasksIndirectEXT(cmd, counters, (VkDeviceSize)VGA_MESH * 16, 1, 16);
    else vkCmdDrawIndirect(cmd, counters, (VkDeviceSize)VGA_DRAW_HW * 16, 1, 16);
    if (c->scene->has_two_sided) {   // the same list again without culling: only two-sided clusters draw
        p.a[0] = 1;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh ? r->p_mesh2 : r->p_vert2);
        vkCmdPushConstants(cmd, r->pl, r->push_stages, 0, sizeof(p), &p);
        if (mesh) vkCmdDrawMeshTasksIndirectEXT(cmd, counters, (VkDeviceSize)VGA_MESH * 16, 1, 16);
        else vkCmdDrawIndirect(cmd, counters, (VkDeviceSize)VGA_DRAW_HW * 16, 1, 16);
        p.a[0] = 0;
        vkCmdPushConstants(cmd, r->pl, r->push_stages, 0, sizeof(p), &p);
    }
    vkCmdEndRendering(cmd);
    stamp(r, cmd, q, VGR_HW);
    if (r->serialize)
        barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // SW
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->p_sw);
    vkCmdDispatchIndirect(cmd, counters, (VkDeviceSize)VGA_SW * 16);
    stamp(r, cmd, q, VGR_SW);
    if (r->qp) r->qp_written[frame][slot][pass - 1] = true;
    // Visibility buffer to its readers; indirect args / list reads before the next writers (WAR).
    barrier(cmd, raster_stages(r) | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            raster_stages(r) | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
}

bool vg_raster_timings(const VgRaster *r, u32 frame, u32 slot, u32 pass, bool wait, float ms[2]) {
    if (!r->qp || pass < 1 || pass > 2 || !r->qp_written[frame][slot][pass - 1]) return false;
    u64 ts[VGR_COUNT];
    u32 q = ((frame * VG_MAX_VIEWS + slot) * 2 + pass - 1) * VGR_COUNT;
    VkQueryResultFlags fl = VK_QUERY_RESULT_64_BIT | (wait ? VK_QUERY_RESULT_WAIT_BIT : 0);
    if (vkGetQueryPoolResults(r->ctx->device, r->qp, q, VGR_COUNT, sizeof(ts), ts, sizeof(u64), fl) != VK_SUCCESS) return false;
    float k = r->ctx->timestamp_period * 1e-6f;
    ms[0] = (float)(ts[VGR_HW] - ts[VGR_BEGIN]) * k;
    ms[1] = (float)(ts[VGR_SW] - ts[VGR_HW]) * k;
    return true;
}
