// GPU scopes for the profiler (vk_prof.h, docs/PERF_AUDIT.md "GPU"). Its own buffers use the real
// allocator ((malloc)(n): not counted by prof_alloc.h, so profiles show the engine's allocations only).
#define VKP_NO_COUNT   // this file calls the real entry points
#include "vk_internal.h"
#include "vk_prof.h"
#include "core/profiler.h"
#include <stdio.h>
#include <stdlib.h>

#define VKP_MAX_SCOPES 160            // per frame
#define VKP_MAX_NAMES 192             // distinct scope names in one capture
#define VKP_STATS 4                   // vertex invocations, clipping primitives, fragment invocations, compute invocations

typedef struct VkpSeries {
    char name[32];
    float *ms, *stat[VKP_STATS];
    u32 *count[VKP_COUNT];
    u32 frames_seen;
} VkpSeries;

typedef struct VkpSlot {
    bool active;
    int frame;                        // capture frame the slot was recorded in
    int n;
    char names[VKP_MAX_SCOPES][32];
    bool stats[VKP_MAX_SCOPES];       // a pipeline statistics query was recorded
    u64 counts[VKP_MAX_SCOPES + 1][VKP_COUNT];   // [VKP_MAX_SCOPES]: commands outside any scope
} VkpSlot;

static struct {
    bool init, stats;
    VkQueryPool ts, ps;
    VkpSlot slot[FRAMES_IN_FLIGHT];
    u32 cur;
    bool rec;                         // this frame records scopes
    int open[2];                      // open scope per stream (-1 none): graphics, async compute (PF-0027)
    int stream;
    bool stream_stats;
    VkpSeries *series;
    int series_count, frames;
} K;

void vkp_init(bool pipeline_stats) {
    memset(&K, 0, sizeof(K));
    K.open[0] = K.open[1] = -1;
    K.stream_stats = true;
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = VKP_MAX_SCOPES * 2 * FRAMES_IN_FLIGHT;
    if (vkCreateQueryPool(R->device, &qi, NULL, &K.ts) != VK_SUCCESS) return;
    if (pipeline_stats) {
        VkQueryPoolCreateInfo pi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        pi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        pi.queryCount = VKP_MAX_SCOPES * FRAMES_IN_FLIGHT;
        pi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
        K.stats = vkCreateQueryPool(R->device, &pi, NULL, &K.ps) == VK_SUCCESS;
    }
    K.init = true;
}

void vkp_shutdown(void) {
    if (!K.init) return;
    vkDestroyQueryPool(R->device, K.ts, NULL);
    if (K.ps) vkDestroyQueryPool(R->device, K.ps, NULL);
    K.init = false;
}

static VkpSeries *series_for(const char *name) {
    for (int i = 0; i < K.series_count; i++) if (!strcmp(K.series[i].name, name)) return &K.series[i];
    if (!K.series || K.series_count >= VKP_MAX_NAMES) return NULL;
    VkpSeries *s = &K.series[K.series_count++];
    dg_strcpy(s->name, sizeof(s->name), name);
    size_t n = (size_t)K.frames;
    s->ms = (float *)(malloc)(sizeof(float) * n);
    for (int k = 0; k < VKP_STATS; k++) s->stat[k] = (float *)(malloc)(sizeof(float) * n);
    for (int k = 0; k < VKP_COUNT; k++) s->count[k] = (u32 *)(calloc)(n, sizeof(u32));
    for (size_t i = 0; i < n; i++) {
        s->ms[i] = -1.0f;   // -1: the scope did not run that frame (or its result was not read back)
        for (int k = 0; k < VKP_STATS; k++) s->stat[k][i] = -1.0f;
    }
    return s;
}

static void read_slot(u32 si) {
    VkpSlot *sl = &K.slot[si];
    if (!sl->active || !K.series || sl->frame < 0 || sl->frame >= K.frames) { sl->active = false; return; }
    int n = sl->n;
    // With availability: scopes on the async compute queue may have no statistics query (PF-0027).
    static u64 ts[VKP_MAX_SCOPES * 2 * 2], ps[VKP_MAX_SCOPES * (VKP_STATS + 1)];
    VkResult tr = n > 0 ? vkGetQueryPoolResults(R->device, K.ts, si * VKP_MAX_SCOPES * 2, (u32)n * 2, sizeof(u64) * 2 * (size_t)n * 2, ts, sizeof(u64) * 2,
                                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                        : VK_NOT_READY;
    VkResult pr = n > 0 && K.stats ? vkGetQueryPoolResults(R->device, K.ps, si * VKP_MAX_SCOPES, (u32)n, sizeof(u64) * (VKP_STATS + 1) * (size_t)n, ps,
                                                           sizeof(u64) * (VKP_STATS + 1), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                                   : VK_NOT_READY;
    bool ts_ok = tr == VK_SUCCESS || tr == VK_NOT_READY, ps_ok = n > 0 && K.stats && (pr == VK_SUCCESS || pr == VK_NOT_READY);
    float period = R->gpu_props.limits.timestampPeriod * 1e-6f;
    for (int i = 0; i <= n; i++) {
        VkpSeries *s = series_for(i < n ? sl->names[i] : "(unscoped)");
        if (!s) continue;
        int f = sl->frame;
        s->frames_seen++;
        if (i < n && ts_ok && ts[i * 4 + 1] && ts[i * 4 + 3])   // a name used twice adds up
            s->ms[f] += (s->ms[f] < 0 ? 1.0f : 0.0f) + (float)(ts[i * 4 + 2] - ts[i * 4]) * period;
        if (i < n && ps_ok && sl->stats[i] && ps[i * (VKP_STATS + 1) + VKP_STATS])
            for (int k = 0; k < VKP_STATS; k++) s->stat[k][f] += (s->stat[k][f] < 0 ? 1.0f : 0.0f) + (float)ps[i * (VKP_STATS + 1) + k];
        for (int c = 0; c < VKP_COUNT; c++) s->count[c][f] += (u32)DG_MIN(sl->counts[i < n ? i : VKP_MAX_SCOPES][c], 0xFFFFFFFFull);
    }
    sl->active = false;
}

void vkp_frame_begin(VkCommandBuffer cmd, u32 slot) {
    if (!K.init) return;
    K.cur = slot % FRAMES_IN_FLIGHT;
    read_slot(K.cur);   // the fence of this slot has signalled (render_begin_frame)
    VkpSlot *sl = &K.slot[K.cur];
    int frame = prof_capture_frame();
    K.rec = dg_prof_recording && frame >= 0 && K.series;
    K.open[0] = K.open[1] = -1;
    K.stream = 0;
    K.stream_stats = true;
    sl->n = 0;
    sl->frame = frame;
    memset(sl->counts, 0, sizeof(sl->counts));
    if (!K.rec) return;
    sl->active = true;
    (vkCmdResetQueryPool)(cmd, K.ts, K.cur * VKP_MAX_SCOPES * 2, VKP_MAX_SCOPES * 2);
    if (K.stats) (vkCmdResetQueryPool)(cmd, K.ps, K.cur * VKP_MAX_SCOPES, VKP_MAX_SCOPES);
    // VRAM residency vs budget (VMA heap budgets), once per captured frame.
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(R->vma, b);
    const VkPhysicalDeviceMemoryProperties *mp = NULL;
    vmaGetMemoryProperties(R->vma, &mp);
    double used = 0, budget = 0, alloc = 0;
    for (u32 h = 0; mp && h < mp->memoryHeapCount; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            used += (double)b[h].usage;
            budget += (double)b[h].budget;
            alloc += (double)b[h].statistics.allocationBytes;
        }
    prof_series("gpu_frame_ms", render_gpu_frame_ms());   // the whole command buffer, last completed frame
    prof_series("vram_usage_mb", used / (1024.0 * 1024.0));
    prof_series("vram_budget_mb", budget / (1024.0 * 1024.0));
    prof_series("vram_allocated_mb", alloc / (1024.0 * 1024.0));
}

void vkp_stream(int stream, bool stats) {
    K.stream = stream ? 1 : 0;
    K.stream_stats = stats;
}

void vkp_scope_end(VkCommandBuffer cmd) {
    int o = K.open[K.stream];
    if (!K.rec || o < 0) return;
    VkpSlot *sl = &K.slot[K.cur];
    u32 q = K.cur * VKP_MAX_SCOPES + (u32)o;
    if (sl->stats[o]) (vkCmdEndQuery)(cmd, K.ps, q);
    (vkCmdWriteTimestamp2)(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, K.ts, q * 2 + 1);
    K.open[K.stream] = -1;
}

void vkp_scope_begin(VkCommandBuffer cmd, const char *name) {
    if (!K.rec) return;
    if (K.open[K.stream] >= 0) vkp_scope_end(cmd);
    VkpSlot *sl = &K.slot[K.cur];
    if (sl->n >= VKP_MAX_SCOPES) return;
    int i = sl->n++;
    dg_strcpy(sl->names[i], sizeof(sl->names[i]), name);
    u32 q = K.cur * VKP_MAX_SCOPES + (u32)i;
    (vkCmdWriteTimestamp2)(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, K.ts, q * 2);
    sl->stats[i] = K.stats && K.stream_stats;
    if (sl->stats[i]) (vkCmdBeginQuery)(cmd, K.ps, q, 0);
    K.open[K.stream] = i;
}

void vkp_note_slow(VkpCount c, u64 n) {
    if (!K.rec) return;
    VkpSlot *sl = &K.slot[K.cur];
    int o = K.open[K.stream];
    sl->counts[o >= 0 ? o : VKP_MAX_SCOPES][c] += n;
}

// ------------------------------------------------------------------ counted entry points

void vkp_begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo *ri) {
    if (dg_prof_recording && K.rec) {
        u64 px = (u64)ri->renderArea.extent.width * ri->renderArea.extent.height * DG_MAX(ri->layerCount, 1u);
        vkp_note_slow(VKP_RENDERING, 1);
        for (u32 i = 0; i < ri->colorAttachmentCount; i++)
            if (ri->pColorAttachments[i].imageView && ri->pColorAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) { vkp_note_slow(VKP_CLEAR, 1); vkp_note_slow(VKP_CLEAR_PX, px); }
        if (ri->pDepthAttachment && ri->pDepthAttachment->imageView && ri->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            vkp_note_slow(VKP_CLEAR, 1);
            vkp_note_slow(VKP_CLEAR_PX, px);
        }
    }
    (vkCmdBeginRendering)(cmd, ri);
}

void vkp_pipeline_barrier2(VkCommandBuffer cmd, const VkDependencyInfo *dep) {
    if (dg_prof_recording && K.rec) {
        vkp_note_slow(VKP_BARRIER, 1);
        vkp_note_slow(VKP_IMAGE_BARRIER, dep->imageMemoryBarrierCount);
    }
    (vkCmdPipelineBarrier2)(cmd, dep);
}

void vkp_copy_buffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    if (dg_prof_recording && K.rec) {
        u64 bytes = 0;
        for (u32 i = 0; i < n; i++) bytes += r[i].size;
        vkp_note_slow(VKP_COPY, 1);
        vkp_note_slow(VKP_COPY_BYTES, bytes);
    }
    (vkCmdCopyBuffer)(cmd, src, dst, n, r);
}

// ------------------------------------------------------------------ capture hooks / JSON

static void cap_begin(int frames) {
    K.frames = frames;
    K.series_count = 0;
    K.series = (VkpSeries *)(calloc)(VKP_MAX_NAMES, sizeof(VkpSeries));
    for (int s = 0; s < FRAMES_IN_FLIGHT; s++) K.slot[s].active = false;
}

static void free_series(void) {
    for (int i = 0; K.series && i < K.series_count; i++) {
        VkpSeries *s = &K.series[i];
        (free)(s->ms);
        for (int k = 0; k < VKP_STATS; k++) (free)(s->stat[k]);
        for (int k = 0; k < VKP_COUNT; k++) (free)(s->count[k]);
    }
    (free)(K.series);
    K.series = NULL;
    K.series_count = 0;
}

static void write_gpu(FILE *f) {
    static const char *cn[VKP_COUNT] = { "draws", "draws_indirect", "dispatches", "pipeline_binds", "descriptor_binds", "push_constants", "barriers",
                                         "image_barriers", "renderings", "clears", "clear_px", "copies", "copy_bytes" };
    static const char *sn[VKP_STATS] = { "vs_invocations", "clip_primitives", "fs_invocations", "cs_invocations" };
    u32 rw = 0, rh = 0;
    render_render_size(&rw, &rh);
    fprintf(f, "{\"device\":");
    prof_json_str(f, R->gpu_props.deviceName);
    fprintf(f, ",\"pipeline_stats\":%s,\"render_w\":%u,\"render_h\":%u,\"output_w\":%u,\"output_h\":%u,\"scopes\":[", K.stats ? "true" : "false", rw, rh,
            R->swap_extent.width, R->swap_extent.height);
    for (int i = 0; K.series && i < K.series_count; i++) {
        VkpSeries *s = &K.series[i];
        fprintf(f, "%s\n {\"name\":", i ? "," : "");
        prof_json_str(f, s->name);
        fprintf(f, ",\"ms\":");
        prof_json_floats(f, s->ms, K.frames);
        for (int k = 0; k < VKP_STATS; k++) {
            fprintf(f, ",\"%s\":", sn[k]);
            prof_json_floats(f, s->stat[k], K.frames);
        }
        for (int c = 0; c < VKP_COUNT; c++) {
            fprintf(f, ",\"%s\":[", cn[c]);
            for (int j = 0; j < K.frames; j++) fprintf(f, j ? ",%u" : "%u", s->count[c][j]);
            fputc(']', f);
        }
        fputc('}', f);
    }
    fprintf(f, "]}");
    free_series();
}

void vkp_register(void) {
    prof_add_capture_hook(cap_begin, NULL);
    prof_add_section("gpu", write_gpu);
}
