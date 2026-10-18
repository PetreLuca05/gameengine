#include "fg.h"
#include "fg_check.h"
#include "render/fidelity/fidelity.h"
#include "core/cvar.h"
#include "core/profiler.h"
#include <stdio.h>
#include <string.h>

typedef struct FgKey { VkFormat format; u32 w, h, d, mips; } FgKey;

typedef struct FgPhys {
    bool used;
    FgKey key;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    VkImageView mip_views[FG_MAX_MIPS];
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    bool last_write;
    u8 queue;                 // the queue of the last access (0 graphics, 1 async compute; PF-0027)
    int busy_until;           // last pass index using it this frame (-1 = free)
    u32 last_frame;           // frame number of the last use (eviction)
    char history[48];         // non-empty: persistent texture of that name
    char debug_name[48];      // current debug utils name (validation builds)
    bool valid;               // history content exists from a previous frame
    VkDeviceSize bytes;
} FgPhys;

typedef enum FgKind { FG_TRANSIENT, FG_HISTORY, FG_IMPORT } FgKind;

typedef struct FgResource {
    char name[48];
    FgKind kind;
    FgKey key;
    VkImageAspectFlags aspect;
    int phys;
    int first, last;
    bool needs_storage, needs_color;
    // imported
    VkImage image;
    VkImageView view;
    VkImageLayout *layout_io;
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    bool last_write;
    u8 queue;                 // imported: the queue of the last access this frame
    bool valid_at_begin;      // history: content was valid when the frame started
} FgResource;

typedef struct FgUse { FgRes res; FgAccess access; bool write, ref; } FgUse;

typedef struct FgPassRec {
    char name[32];
    FgExecFn exec;
    void *user;
    bool enabled;
    bool async;               // fg_async: may run on the async compute queue
    bool marker;              // fg_marker: the caller records its work
    FgUse uses[16];
    int use_count;
    int after[4];             // fg_after: passes it needs without a shared resource
    int after_n;
} FgPassRec;

#define FG_POOL_MAX 128

// Async compute (PF-0027): a batch is a run of one queue's passes between cross-queue sync points,
// recorded into its own command buffer. Batch 0 is the graphics work recorded before the graph
// (render_end_frame: uploads, particles, wind, rain occlusion), FG_PRE as a pass index.
#define FG_MAX_BATCHES 24
#define FG_PRE (-1)
#define FG_NO_WAIT (-2)
typedef struct FgBatch {
    int queue;                // 0 graphics, 1 async compute
    int first, last;          // pass range (FG_PRE: the pre-graph work)
    int wait;                 // the other queue's pass whose batch completes first (FG_PRE, FG_NO_WAIT)
    bool signal;              // signals its queue's timeline (the other queue waits for it)
    u64 value;
    VkCommandBuffer cmd;
} FgBatch;

static struct {
    bool init;
    FgPhys pool[FG_POOL_MAX];
    FgResource res[FG_MAX_RES];
    int res_count;
    FgPassRec passes[FG_MAX_PASSES];
    int pass_count;
    u32 render_w, render_h, output_w, output_h;
    u32 frame;
    VkQueryPool qp;
    float ms[FRAMES_IN_FLIGHT][FG_MAX_PASSES];
    char ms_names[FRAMES_IN_FLIGHT][FG_MAX_PASSES][32];
    int ms_count[FRAMES_IN_FLIGHT];
    bool ms_pending[FRAMES_IN_FLIGHT];
    float last_ms[FG_MAX_PASSES];
    char last_names[FG_MAX_PASSES][32];
    int last_count;
    Cvar *disable;
    int cur;                  // the executing pass (fg_store_op, fg_stages), -1 outside a pass exec
    FgReport report;          // the last executed graph (fg_check + pool state)
    bool report_valid;        // PF-0005: built on the first fg_last_report after fg_exec_begin
    // async compute (PF-0027)
    int async_kind;           // this frame: 0 off, 1 the compute family, 2 the second graphics queue
    bool async_graphics;      // the async queue's family supports graphics (stage masks, statistics)
    VkSemaphore timeline[2];  // graphics, async: the batches' signals
    u64 timeline_value[2];
    VkCommandPool cmd_pool[FRAMES_IN_FLIGHT][2];   // per family: [0] graphics (also the second graphics queue), [1] compute
    VkCommandBuffer cmd_buf[FRAMES_IN_FLIGHT][2][FG_MAX_BATCHES];
    int cmd_used[2];
    FgBatch batch[FG_MAX_BATCHES];
    int batch_n;
    u8 pass_queue[FG_MAX_PASSES];
    u8 pass_batch[FG_MAX_PASSES];
    signed char prev_user[FG_MAX_PASSES][16];   // per use: the pass that used the resource before (-1 none)
    int cursor;               // next pass to record
    int open_marker;          // the marker between fg_exec_until and fg_exec_rest (-1 none)
} G;

static void write_profile(FILE *f);

void fg_init(void) {
    memset(&G, 0, sizeof(G));
    G.cur = -1;
    G.init = true;
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = FG_MAX_PASSES * 2 * FRAMES_IN_FLIGHT;
    VK_CHECK(vkCreateQueryPool(R->device, &qi, NULL, &G.qp));
    G.disable = cvar_reg_string("r_post_disable", "", CVAR_LOCKED, "comma separated frame graph passes to skip (e.g. ssr,gtao); r_post_passes lists them");
    prof_add_section("fg", write_profile);
    G.open_marker = -1;
    if (R->compute_queue || R->gfx_queue2) {
        VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        si.pNext = &ti;
        for (int q = 0; q < 2; q++) VK_CHECK(vkCreateSemaphore(R->device, &si, NULL, &G.timeline[q]));
    }
    for (int f = 0; f < FRAMES_IN_FLIGHT; f++)
        for (int k = 0; k < 2; k++) {
            if (k == 1 && !R->compute_queue) continue;
            VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.queueFamilyIndex = k ? R->compute_family : R->queue_family;
            VK_CHECK(vkCreateCommandPool(R->device, &pci, NULL, &G.cmd_pool[f][k]));
        }
}

static void phys_destroy(FgPhys *p, bool now) {
    if (!p->used) return;
    for (int m = 0; m < FG_MAX_MIPS; m++)
        if (p->mip_views[m]) {
            if (now) vkDestroyImageView(R->device, p->mip_views[m], NULL);
            else vkr_defer_image(VK_NULL_HANDLE, p->mip_views[m], NULL);
        }
    if (now) {
        vkDestroyImageView(R->device, p->view, NULL);
        vmaDestroyImage(R->vma, p->image, p->alloc);
    } else {
        vkr_defer_image(p->image, p->view, p->alloc);
    }
    memset(p, 0, sizeof(*p));
}

void fg_shutdown(void) {
    if (!G.init) return;
    vkDeviceWaitIdle(R->device);
    for (int i = 0; i < FG_POOL_MAX; i++) phys_destroy(&G.pool[i], true);
    vkDestroyQueryPool(R->device, G.qp, NULL);
    for (int q = 0; q < 2; q++)
        if (G.timeline[q]) vkDestroySemaphore(R->device, G.timeline[q], NULL);
    for (int f = 0; f < FRAMES_IN_FLIGHT; f++)
        for (int k = 0; k < 2; k++)
            if (G.cmd_pool[f][k]) vkDestroyCommandPool(R->device, G.cmd_pool[f][k], NULL);
    G.init = false;
}

void fg_targets_changed(void) {
    if (!G.init) return;
    fg_last_report();   // the report describes the pool before it is dropped
    vkDeviceWaitIdle(R->device);
    for (int i = 0; i < FG_POOL_MAX; i++) phys_destroy(&G.pool[i], true);
}

static void read_timings(void) {
    int slot = (int)R->frame_slot;
    if (!G.ms_pending[slot]) return;
    int n = G.ms_count[slot];
    u64 ts[FG_MAX_PASSES * 2];
    if (n > 0 && vkGetQueryPoolResults(R->device, G.qp, (u32)(slot * FG_MAX_PASSES * 2), (u32)n * 2, sizeof(u64) * (size_t)n * 2, ts, sizeof(u64),
                                       VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        float k = R->gpu_props.limits.timestampPeriod * 1e-6f;
        for (int i = 0; i < n; i++) {
            G.last_ms[i] = (float)(ts[i * 2 + 1] - ts[i * 2]) * k;
            memcpy(G.last_names[i], G.ms_names[slot][i], 32);
        }
        G.last_count = n;
    }
    G.ms_pending[slot] = false;
}

void fg_begin(u32 rw, u32 rh, u32 ow, u32 oh) {
    read_timings();
    G.frame++;
    G.res_count = 0;
    G.pass_count = 0;
    G.render_w = rw;
    G.render_h = rh;
    G.output_w = ow;
    G.output_h = oh;
    for (int i = 0; i < FG_POOL_MAX; i++) G.pool[i].busy_until = -1;
    // Async compute this frame (PF-0027): r_async_compute 1 the compute-only family (it needs timestamps:
    // the passes are timed), 2 the second graphics queue (measured slower on an RTX 3060 Ti: the driver
    // time-slices the graphics family's queues); off when the device has not that queue.
    int want = R->r_async_compute ? R->r_async_compute->i : 0;
    bool c = R->compute_queue && R->compute_timestamps && G.cmd_pool[0][1], g2 = R->gfx_queue2 != VK_NULL_HANDLE;
    G.async_kind = 0;
    if (want > 0 && prof_fix_enabled(PFX_ASYNC_COMPUTE) && G.timeline[0]) G.async_kind = want == 1 ? (c ? 1 : 0) : (g2 ? 2 : 0);
    G.async_graphics = G.async_kind == 2;
}

bool fg_async_frame(void) { return G.async_kind != 0; }

static FgKey key_for(const FgTexDesc *d) {
    FgKey k;
    memset(&k, 0, sizeof(k));
    k.format = d->format;
    u32 bw = G.render_w, bh = G.render_h;
    switch (d->size) {
    case FG_SIZE_HALF: bw = (G.render_w + 1) / 2; bh = (G.render_h + 1) / 2; break;
    case FG_SIZE_QUARTER: bw = (G.render_w + 3) / 4; bh = (G.render_h + 3) / 4; break;
    case FG_SIZE_OUTPUT: bw = G.output_w; bh = G.output_h; break;
    case FG_SIZE_ABS: bw = d->w; bh = d->h; break;
    default: break;
    }
    if (d->scale > 0) { bw = (u32)((float)bw * d->scale + 0.5f); bh = (u32)((float)bh * d->scale + 0.5f); }
    k.w = DG_MAX(1u, bw);
    k.h = DG_MAX(1u, bh);
    k.d = DG_MAX(1u, d->d);
    u32 max_mips = 1;
    while ((k.w >> max_mips) > 0 || (k.h >> max_mips) > 0) max_mips++;
    k.mips = d->mips == 0 ? 1 : DG_MIN(DG_MIN(d->mips, (u32)FG_MAX_MIPS), max_mips);
    if (k.d > 1) k.mips = 1;
    return k;
}

static bool is_depth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM; }

static FgRes add_res(const char *name, FgKind kind) {
    if (G.res_count >= FG_MAX_RES) { log_error("fg: too many resources"); return FG_NONE; }
    FgResource *r = &G.res[G.res_count];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name ? name : "?");
    r->kind = kind;
    r->phys = -1;
    r->first = r->last = -1;
    return G.res_count++;
}

FgRes fg_texture(const FgTexDesc *d) {
    FgRes id = add_res(d->name, FG_TRANSIENT);
    if (id < 0) return id;
    G.res[id].key = key_for(d);
    G.res[id].aspect = is_depth(d->format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    return id;
}

FgRes fg_history(const FgTexDesc *d) {
    FgRes id = fg_texture(d);
    if (id >= 0) G.res[id].kind = FG_HISTORY;
    return id;
}

FgRes fg_import(const char *name, VkImage image, VkImageView view, VkFormat format, u32 w, u32 h, VkImageAspectFlags aspect, VkImageLayout *layout_io) {
    FgRes id = add_res(name, FG_IMPORT);
    if (id < 0) return id;
    FgResource *r = &G.res[id];
    r->image = image;
    r->view = view;
    r->key.format = format;
    r->key.w = w;
    r->key.h = h;
    r->key.d = 1;
    r->key.mips = 1;
    r->aspect = aspect;
    r->layout_io = layout_io;
    r->layout = layout_io ? *layout_io : VK_IMAGE_LAYOUT_UNDEFINED;
    r->stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    r->access = VK_ACCESS_2_MEMORY_WRITE_BIT;
    r->last_write = true;
    r->queue = 0;
    return id;
}

static bool name_in_list(const char *list, const char *name) {
    size_t n = strlen(name);
    const char *p = list;
    while (p && *p) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == n && !strncmp(p, name, n)) return true;
        p = e;
    }
    return false;
}

bool fg_pass_enabled(const char *name) { return !(G.disable && G.disable->str[0] && name_in_list(G.disable->str, name)); }

static int add_pass(const char *name, FgExecFn exec, void *user) {
    if (G.pass_count >= FG_MAX_PASSES) { log_error("fg: too many passes"); return -1; }
    FgPassRec *p = &G.passes[G.pass_count];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->exec = exec;
    p->user = user;
    p->enabled = true;
    return G.pass_count++;
}

int fg_pass(const char *name, FgExecFn exec, void *user) {
    if (!fg_pass_enabled(name)) return -1;
    return add_pass(name, exec, user);
}

int fg_marker(const char *name) {
    int p = add_pass(name, NULL, NULL);
    if (p >= 0) G.passes[p].marker = true;
    return p;
}

void fg_async(int pass) {
    if (pass >= 0 && pass < G.pass_count && G.passes[pass].exec) G.passes[pass].async = true;
}

void fg_after(int pass, int dep) {
    if (pass < 0 || pass >= G.pass_count || dep < 0 || dep >= pass) return;
    FgPassRec *p = &G.passes[pass];
    if (p->after_n < 4) p->after[p->after_n++] = dep;
}

int fg_find_pass(const char *name) {
    for (int i = 0; i < G.pass_count; i++)
        if (!strcmp(G.passes[i].name, name)) return i;
    return -1;
}

static void use(int pass, FgRes r, FgAccess a, bool write) {
    if (pass < 0 || r < 0 || pass >= G.pass_count) return;
    FgPassRec *p = &G.passes[pass];
    if (p->use_count >= 16) { log_error("fg: pass %s uses too many resources", p->name); return; }
    p->uses[p->use_count++] = (FgUse){ r, a, write, false };
    FgResource *res = &G.res[r];
    if (res->first < 0) res->first = pass;
    res->last = pass;
    if (a == FG_STORAGE) res->needs_storage = true;
    if (a == FG_COLOR) res->needs_color = true;
}
void fg_read(int pass, FgRes r, FgAccess a) { use(pass, r, a, false); }
void fg_write(int pass, FgRes r, FgAccess a) { use(pass, r, a, true); }
void fg_reference(int pass, FgRes r, FgAccess a) {
    use(pass, r, a, false);
    if (pass >= 0 && r >= 0 && pass < G.pass_count && G.passes[pass].use_count > 0) G.passes[pass].uses[G.passes[pass].use_count - 1].ref = true;
}

// ------------------------------------------------------------------ physical images

static VkImageUsageFlags usage_for(VkFormat f) {
    if (is_depth(f)) return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(R->gpu, f, &fp);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return u;
}

static int phys_create(const FgKey *k, VkImageAspectFlags aspect, const char *history) {
    int slot = -1;
    for (int i = 0; i < FG_POOL_MAX; i++) if (!G.pool[i].used) { slot = i; break; }
    if (slot < 0) { log_error("fg: physical image pool full"); return -1; }
    FgPhys *p = &G.pool[slot];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->key = *k;
    p->aspect = aspect;
    p->usage = usage_for(k->format);
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = k->d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = k->format;
    ici.extent.width = k->w;
    ici.extent.height = k->h;
    ici.extent.depth = k->d;
    ici.mipLevels = k->mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = p->usage;
    vkd_share_image(&ici);   // any pooled image may serve an async compute pass (PF-0027)
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationInfo ai;
    VK_CHECK(vmaCreateImage(R->vma, &ici, &aci, &p->image, &p->alloc, &ai));
    p->bytes = ai.size;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = p->image;
    vci.viewType = k->d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = k->format;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = k->mips;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &p->view));
    if (k->mips > 1)
        for (u32 m = 0; m < k->mips; m++) {
            vci.subresourceRange.baseMipLevel = m;
            vci.subresourceRange.levelCount = 1;
            VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &p->mip_views[m]));
        }
    p->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    p->stage = VK_PIPELINE_STAGE_2_NONE;
    p->busy_until = -1;
    if (history) snprintf(p->history, sizeof(p->history), "%s", history);
    return slot;
}

static bool key_eq(const FgKey *a, const FgKey *b) { return a->format == b->format && a->w == b->w && a->h == b->h && a->d == b->d && a->mips == b->mips; }

// Validation messages name the frame graph images (debug utils, validation builds only).
static void name_image(FgPhys *p, const char *name) {
    if (!R->validation || !vkSetDebugUtilsObjectNameEXT || !strcmp(p->debug_name, name)) return;
    snprintf(p->debug_name, sizeof(p->debug_name), "%s", name);
    VkDebugUtilsObjectNameInfoEXT ni = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    ni.objectType = VK_OBJECT_TYPE_IMAGE;
    ni.objectHandle = (u64)p->image;
    ni.pObjectName = p->debug_name;
    vkSetDebugUtilsObjectNameEXT(R->device, &ni);
}

static void allocate(void) {
    // Histories first (stable images by name).
    for (int i = 0; i < G.res_count; i++) {
        FgResource *r = &G.res[i];
        if (r->kind != FG_HISTORY || r->first < 0) continue;
        int found = -1;
        for (int k = 0; k < FG_POOL_MAX; k++)
            if (G.pool[k].used && G.pool[k].history[0] && !strcmp(G.pool[k].history, r->name)) { found = k; break; }
        if (found >= 0 && !key_eq(&G.pool[found].key, &r->key)) { phys_destroy(&G.pool[found], false); found = -1; }
        if (found < 0) found = phys_create(&r->key, r->aspect, r->name);
        r->phys = found;
        if (found >= 0) name_image(&G.pool[found], r->name);
        if (found >= 0) {
            r->valid_at_begin = G.pool[found].valid && G.pool[found].last_frame + 1 == G.frame;
            G.pool[found].busy_until = FG_MAX_PASSES;
        }
    }
    // Transients in order of first use: reuse a free compatible image (aliasing).
    for (int pass = 0; pass < G.pass_count; pass++)
        for (int i = 0; i < G.res_count; i++) {
            FgResource *r = &G.res[i];
            if (r->kind != FG_TRANSIENT || r->first != pass) continue;
            int found = -1;
            for (int k = 0; k < FG_POOL_MAX; k++) {
                FgPhys *p = &G.pool[k];
                if (!p->used || p->history[0] || !key_eq(&p->key, &r->key)) continue;
                if (p->busy_until < r->first) { found = k; break; }
            }
            if (found < 0) found = phys_create(&r->key, r->aspect, NULL);
            r->phys = found;
            if (found >= 0) G.pool[found].busy_until = r->last;
            if (found >= 0) name_image(&G.pool[found], r->name);
        }
    // Sanity: two different resources of one pass must never share a physical image.
    for (int pi = 0; pi < G.pass_count; pi++) {
        FgPassRec *p = &G.passes[pi];
        for (int u = 0; u < p->use_count; u++)
            for (int v = u + 1; v < p->use_count; v++) {
                FgResource *a = &G.res[p->uses[u].res], *b = &G.res[p->uses[v].res];
                if (a == b || a->kind == FG_IMPORT || b->kind == FG_IMPORT || a->phys < 0 || a->phys != b->phys) continue;
                static int reported;
                if (reported++ < 8)
                    log_error("fg: pass %s: %s [%d..%d] and %s [%d..%d] alias one image", p->name, a->name, a->first, a->last, b->name, b->first, b->last);
            }
    }
    // Evict images unused for a while.
    for (int k = 0; k < FG_POOL_MAX; k++)
        if (G.pool[k].used && G.pool[k].busy_until < 0 && G.frame - G.pool[k].last_frame > 240) phys_destroy(&G.pool[k], false);
}

// ------------------------------------------------------------------ barriers

typedef struct State { VkImageLayout layout; VkPipelineStageFlags2 stage; VkAccessFlags2 access; } State;

static State state_for(FgAccess a, bool depth) {
    const VkPipelineStageFlags2 SH = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    const VkPipelineStageFlags2 DS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    switch (a) {
    case FG_SAMPLED: return (State){ depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, SH, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
    case FG_STORAGE: return (State){ VK_IMAGE_LAYOUT_GENERAL, SH, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
    case FG_COLOR: return (State){ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
    case FG_DEPTH: return (State){ VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, DS, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    case FG_DEPTH_READ: return (State){ VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, DS | SH, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
    case FG_TRANSFER_SRC: return (State){ VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
    case FG_TRANSFER_DST: return (State){ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
    }
    return (State){ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
}

static void res_state(FgResource *r, VkImage *img, VkImageLayout **layout, VkPipelineStageFlags2 **stage, VkAccessFlags2 **access, bool **lw, u32 *mips, u8 **queue) {
    if (r->kind == FG_IMPORT) {
        *img = r->image;
        *layout = &r->layout;
        *stage = &r->stage;
        *access = &r->access;
        *lw = &r->last_write;
        *mips = 1;
        *queue = &r->queue;
    } else {
        FgPhys *p = &G.pool[r->phys];
        *img = p->image;
        *layout = &p->layout;
        *stage = &p->stage;
        *access = &p->access;
        *lw = &p->last_write;
        *mips = p->key.mips;
        *queue = &p->queue;
    }
}

// Stages a queue supports: the compute-only family drops the graphics stages (PF-0027).
static VkPipelineStageFlags2 queue_stages(VkPipelineStageFlags2 s, int q) {
    if (q == 0 || G.async_graphics) return s;
    const VkPipelineStageFlags2 ok = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                     VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    s &= ok;
    return s ? s : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
}

VkPipelineStageFlags2 fg_stages(VkPipelineStageFlags2 s) { return queue_stages(s, G.cur >= 0 ? G.pass_queue[G.cur] : 0); }

// A pass's access to a resource it uses more than once: storage wins (GENERAL), writes add up. Returns
// false for the uses after the first (handled by the first).
static bool merged_use(const FgPassRec *p, int u, FgAccess *a, bool *write) {
    *a = p->uses[u].access;
    *write = p->uses[u].write;
    for (int v = 0; v < p->use_count; v++)
        if (v != u && p->uses[v].res == p->uses[u].res) {
            if (v < u) return false;
            if (p->uses[v].access == FG_STORAGE) *a = FG_STORAGE;
            *write |= p->uses[v].write;
        }
    return true;
}

static void pass_barriers(VkCommandBuffer cmd, FgPassRec *p) {
    VkImageMemoryBarrier2 bars[16];
    int n = 0;
    int q = G.pass_queue[p - G.passes];
    for (int u = 0; u < p->use_count; u++) {
        FgResource *r = &G.res[p->uses[u].res];
        if (r->kind != FG_IMPORT && r->phys < 0) continue;
        // Read + storage write of the same resource in one pass: GENERAL wins.
        FgAccess a;
        bool write;
        if (!merged_use(p, u, &a, &write)) continue;   // handled by the first use
        State want = state_for(a, r->aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
        want.stage = queue_stages(want.stage, q);
        VkImage img;
        VkImageLayout *lay;
        VkPipelineStageFlags2 *st;
        VkAccessFlags2 *ac;
        bool *lw;
        u32 mips;
        u8 *rq;
        res_state(r, &img, &lay, &st, &ac, &lw, &mips, &rq);
        // Last used on the other queue (PF-0027): the timeline wait at this batch's start ordered those
        // accesses and made them visible; only a layout change remains (chained to the wait).
        bool cross = *rq != q;
        bool hazard = cross ? *lay != want.layout : (*lay != want.layout || *lw || write);
        if (hazard) {
            VkImageMemoryBarrier2 *b = &bars[n++];
            memset(b, 0, sizeof(*b));
            b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b->srcStageMask = cross ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : (*st ? *st : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
            b->srcAccessMask = !cross && *lw ? *ac : 0;
            b->dstStageMask = want.stage;
            b->dstAccessMask = want.access;
            // Transients are re-written every frame: discard the old content (aliasing).
            bool discard = r->kind == FG_TRANSIENT && r->first == (int)(p - G.passes) && write && a != FG_SAMPLED;
            b->oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : *lay;
            b->newLayout = want.layout;
            b->srcQueueFamilyIndex = b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b->image = img;
            b->subresourceRange.aspectMask = r->aspect;
            b->subresourceRange.levelCount = mips;
            b->subresourceRange.layerCount = 1;
        }
        *lay = want.layout;
        *st = want.stage;
        *ac = want.access;
        *lw = write;
        *rq = (u8)q;
    }
    if (n) {
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = (u32)n;
        dep.pImageMemoryBarriers = bars;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

// Validation of the graph just allocated (fg_check.h): dead passes, unused resources, undefined
// reads, post-processing passes (Engine Fidelity Off schedules none), pool images this frame doesn't use.
static void check_graph(void) {
    PROF_BEGIN("fg:validate");
    static FgcUse uses[FG_MAX_PASSES][16];
    FgcPass passes[FG_MAX_PASSES];
    FgcRes res[FG_MAX_RES];
    for (int i = 0; i < G.res_count; i++) res[i] = (FgcRes){ G.res[i].name, G.res[i].kind == FG_TRANSIENT ? FGC_TRANSIENT : (G.res[i].kind == FG_HISTORY ? FGC_HISTORY : FGC_IMPORT) };
    FgReport *r = &G.report;
    memset(r, 0, sizeof(*r));
    for (int p = 0; p < G.pass_count; p++) {
        FgPassRec *pr = &G.passes[p];
        int nu = 0;
        for (int u = 0; u < pr->use_count; u++)
            if (!pr->uses[u].ref) uses[p][nu++] = (FgcUse){ pr->uses[u].res, pr->uses[u].write };
        passes[p] = (FgcPass){ pr->name, uses[p], nu };
        if (fid_pass_is_post(pr->name)) {
            if (r->post_passes < FG_REPORT_NAMES) snprintf(r->post[r->post_passes], 32, "%s", pr->name);
            r->post_passes++;
        }
        if (p < FG_MAX_PASSES) snprintf(r->names[p], 32, "%s", pr->name);
    }
    FgcReport c;
    fgc_analyze(passes, G.pass_count, res, G.res_count, &c);
    r->passes = G.pass_count;
    r->dead_passes = c.dead_passes;
    r->unused_res = c.unused_res;
    r->undefined_reads = c.undefined_reads;
    memcpy(r->dead, c.dead, sizeof(r->dead));
    memcpy(r->unused, c.unused, sizeof(r->unused));
    memcpy(r->undefined, c.undefined, sizeof(r->undefined));
    for (int k = 0; k < FG_POOL_MAX; k++)
        if (G.pool[k].used) {
            r->pool_images++;
            r->pool_bytes += G.pool[k].bytes;
            if (G.pool[k].busy_until < 0) { r->stale_images++; r->stale_bytes += G.pool[k].bytes; }
        }
    PROF_END();
}

// The validation report of the last executed graph. It used to be built inside every execution
// (pass name copies, the dead pass / unused resource analysis) although only the fidelity checks and
// benches read it: now it is built on the first request after the frame (docs/perf/FINDINGS.md
// PF-0005; perf_fix_disable PF-0005 builds it every frame again). Same content: between
// fg_exec_begin and the next fg_begin the graph and the pool state it reads are unchanged.
const FgReport *fg_last_report(void) {
    if (!G.report_valid && G.init) {
        check_graph();
        G.report_valid = true;
    }
    return &G.report;
}

// Drops pooled images the last frame didn't use (a settings change: Engine Fidelity level switch)
// instead of waiting for the 240-frame eviction. Deferred destruction (frames in flight).
void fg_trim(void) {
    if (!G.init) return;
    fg_last_report();
    for (int k = 0; k < FG_POOL_MAX; k++)
        if (G.pool[k].used && G.pool[k].last_frame != G.frame) phys_destroy(&G.pool[k], false);
}

// ------------------------------------------------------------------ execution (two queues, PF-0027)

// Each pass's queue, the cross-queue sync points and the batches between them. The declared order
// is a valid serial order: a pass waits for the latest pass of the other queue it depends on (the
// previous user of each of its resources, fg_after) unless an earlier batch of its queue already
// waited for that pass or a later one, and that pass's batch ends with a signal. An async pass also
// waits for the graphics work declared before it (only what is declared after it overlaps it: fewer
// batches, and the pre-graph work, whose signal follows the previous frame's graphics work too, is
// always behind: the persistent images the async passes rewrite are no longer read). Without async
// passes: one batch.
static void schedule(void) {
    static int last_user[FG_MAX_RES], need[FG_MAX_PASSES];
    static bool cut[FG_MAX_PASSES], sig[FG_MAX_PASSES];
    int n = G.pass_count;
    for (int i = 0; i < G.res_count; i++) last_user[i] = -1;
    for (int p = 0; p < n; p++) G.pass_queue[p] = (u8)(G.async_kind && G.passes[p].async ? 1 : 0);
    int last_gfx = FG_PRE;
    for (int p = 0; p < n; p++) {
        FgPassRec *pr = &G.passes[p];
        int q = G.pass_queue[p];
        int w = q ? last_gfx : FG_NO_WAIT;
        if (!q) last_gfx = p;
        for (int u = 0; u < pr->use_count; u++) {
            int i = last_user[pr->uses[u].res];
            G.prev_user[p][u] = (signed char)i;
            if (i >= 0 && G.pass_queue[i] != q && i > w) w = i;
        }
        for (int a = 0; a < pr->after_n; a++)
            if (G.pass_queue[pr->after[a]] != q && pr->after[a] > w) w = pr->after[a];
        for (int u = 0; u < pr->use_count; u++) last_user[pr->uses[u].res] = p;
        need[p] = w;
    }
    int waited[2] = { FG_NO_WAIT, FG_NO_WAIT };
    bool pre_signal = false;
    for (int p = 0; p < n; p++) {
        int q = G.pass_queue[p];
        cut[p] = sig[p] = false;
        if (need[p] > waited[q]) {
            cut[p] = true;
            waited[q] = need[p];
            if (need[p] == FG_PRE) pre_signal = true;
            else sig[need[p]] = true;
        }
    }
    G.batch_n = 0;
    FgBatch *pre = &G.batch[G.batch_n++];
    memset(pre, 0, sizeof(*pre));
    pre->first = pre->last = FG_PRE;
    pre->wait = FG_NO_WAIT;
    pre->signal = pre_signal;
    int open[2] = { pre_signal ? -1 : 0, -1 };
    for (int p = 0; p < n; p++) {
        int q = G.pass_queue[p];
        if (open[q] < 0 || cut[p]) {
            if (G.batch_n >= FG_MAX_BATCHES) {
                static int warned;
                if (!warned++) log_warn("fg: more than %d queue batches: this frame runs on the graphics queue", FG_MAX_BATCHES);
                G.async_kind = 0;
                G.async_graphics = false;
                schedule();
                return;
            }
            FgBatch *b = &G.batch[G.batch_n];
            memset(b, 0, sizeof(*b));
            b->queue = q;
            b->first = p;
            b->wait = cut[p] ? need[p] : FG_NO_WAIT;
            open[q] = G.batch_n++;
        }
        G.batch[open[q]].last = p;
        G.pass_batch[p] = (u8)open[q];
        if (sig[p]) {
            G.batch[open[q]].signal = true;
            open[q] = -1;
        }
    }
    // The frame's last async batch signals: the last graphics batch (fence) waits for it (fg_submit).
    for (int b = G.batch_n - 1; b > 0; b--)
        if (G.batch[b].queue == 1) { G.batch[b].signal = true; break; }
}

// A batch's command buffer, begun on first use (per frame slot and queue family; the pools are reset
// when the slot comes round again: its fence covered both queues).
static VkCommandBuffer batch_cmd(int b) {
    FgBatch *bt = &G.batch[b];
    if (bt->cmd) return bt->cmd;
    int fam = bt->queue == 1 && G.async_kind == 1 ? 1 : 0, slot = (int)R->frame_slot;
    VkCommandBuffer *c = &G.cmd_buf[slot][fam][G.cmd_used[fam]++];
    if (!*c) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = G.cmd_pool[slot][fam];
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(R->device, &ai, c));
    }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(*c, &bi));
    bt->cmd = *c;
    return *c;
}

// End of a graphics batch: what an async pass uses next (its previous user is in this batch) moves
// to that pass's layout here, so the async queue never changes a layout (depth images included); the
// batch's timeline signal orders and publishes it.
static void batch_handoff(VkCommandBuffer cmd, int b) {
    VkImageMemoryBarrier2 bars[32];
    int n = 0;
    for (int j = G.batch[b].last + 1; j < G.pass_count; j++) {
        if (G.pass_queue[j] != 1) continue;
        FgPassRec *pj = &G.passes[j];
        for (int u = 0; u < pj->use_count && n < 32; u++) {
            int i = G.prev_user[j][u];
            FgAccess a;
            bool write;
            if (i < 0 || G.pass_batch[i] != b || !merged_use(pj, u, &a, &write)) continue;
            FgResource *r = &G.res[pj->uses[u].res];
            if (r->kind != FG_IMPORT && r->phys < 0) continue;
            VkImage img;
            VkImageLayout *lay;
            VkPipelineStageFlags2 *st;
            VkAccessFlags2 *ac;
            bool *lw;
            u32 mips;
            u8 *rq;
            res_state(r, &img, &lay, &st, &ac, &lw, &mips, &rq);
            if (*rq != 0) continue;
            State want = state_for(a, r->aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
            if (*lay != want.layout) {
                VkImageMemoryBarrier2 *bb = &bars[n++];
                memset(bb, 0, sizeof(*bb));
                bb->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                bb->srcStageMask = *st ? *st : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                bb->srcAccessMask = *lw ? *ac : 0;
                bb->dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                bb->oldLayout = *lay;
                bb->newLayout = want.layout;
                bb->srcQueueFamilyIndex = bb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb->image = img;
                bb->subresourceRange.aspectMask = r->aspect;
                bb->subresourceRange.levelCount = mips;
                bb->subresourceRange.layerCount = 1;
            }
            *lay = want.layout;
            *st = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            *ac = 0;
            *lw = false;
            *rq = 1;
        }
    }
    if (n) {
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = (u32)n;
        dep.pImageMemoryBarriers = bars;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

static void pass_begin(int i, VkCommandBuffer cmd) {
    FgPassRec *p = &G.passes[i];
    int q = G.pass_queue[i];
    u32 qbase = (u32)((int)R->frame_slot * FG_MAX_PASSES * 2);
    vkp_stream(q, q == 0 || G.async_graphics);
    vkp_scope_begin(cmd, p->name);
    pass_barriers(cmd, p);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, G.qp, qbase + (u32)i * 2);
    if (R->validation && vkCmdBeginDebugUtilsLabelEXT) {
        VkDebugUtilsLabelEXT l = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
        l.pLabelName = p->name;
        vkCmdBeginDebugUtilsLabelEXT(cmd, &l);
    }
    G.cur = i;
}

static void pass_end(int i, VkCommandBuffer cmd) {
    FgPassRec *p = &G.passes[i];
    int slot = (int)R->frame_slot;
    u32 qbase = (u32)(slot * FG_MAX_PASSES * 2);
    G.cur = -1;
    if (R->validation && vkCmdEndDebugUtilsLabelEXT) vkCmdEndDebugUtilsLabelEXT(cmd);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, G.qp, qbase + (u32)i * 2 + 1);
    vkp_scope_end(cmd);
    vkp_stream(0, true);
    memcpy(G.ms_names[slot][i], p->name, 32);
    int b = G.pass_batch[i];
    if (G.batch[b].last == i && G.batch[b].queue == 0) batch_handoff(cmd, b);
}

static void run_to(int end) {
    for (; G.cursor < end; G.cursor++) {
        int i = G.cursor;
        VkCommandBuffer cmd = batch_cmd(G.pass_batch[i]);
        pass_begin(i, cmd);
        if (G.passes[i].exec) G.passes[i].exec(cmd, G.passes[i].user);
        pass_end(i, cmd);
    }
}

// The batches of this frame, one line ("G[pre] C[sky_luts..sky_env] G[prepass..shadows] ..."): logged
// when the shape changes (a few times), printed by r_async_info.
static void batches_line(char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
    for (int b = 0; b < G.batch_n && o + 80 < n; b++) {
        const FgBatch *bt = &G.batch[b];
        const char *f = bt->first == FG_PRE ? "pre" : G.passes[bt->first].name;
        const char *l = bt->last == FG_PRE ? "pre" : G.passes[bt->last].name;
        o += (size_t)snprintf(out + o, n - o, "%s%c[%s%s%s]%s", b ? " " : "", bt->queue ? 'C' : 'G', f, bt->first == bt->last ? "" : "..",
                              bt->first == bt->last ? "" : l, bt->signal ? "+" : "");
    }
}

void fg_async_info(void) {
    char line[1024];
    batches_line(line, sizeof(line));
    log_info("fg: async compute %s (r_async_compute %d), %d batches: %s", G.async_kind == 1 ? "on the compute family" : (G.async_kind == 2 ? "on a second graphics queue" : "off"),
             R->r_async_compute ? R->r_async_compute->i : 0, G.batch_n, line);
}

VkCommandBuffer fg_exec_begin(VkCommandBuffer cmd) {
    allocate();
    G.report_valid = false;
    if (!prof_fix_enabled(PFX_FG_REPORT_LAZY)) fg_last_report();
    int slot = (int)R->frame_slot;
    vkCmdResetQueryPool(cmd, G.qp, (u32)(slot * FG_MAX_PASSES * 2), FG_MAX_PASSES * 2);
    for (int k = 0; k < 2; k++)
        if (G.cmd_pool[slot][k]) VK_CHECK(vkResetCommandPool(R->device, G.cmd_pool[slot][k], 0));
    G.cmd_used[0] = G.cmd_used[1] = 0;
    PROF_BEGIN("fg:schedule");
    schedule();
    PROF_END();
    {
        static char last[1024];
        static int logged;
        char line[1024];
        batches_line(line, sizeof(line));
        if (strcmp(line, last) && logged < 6) {
            logged++;
            dg_strcpy(last, sizeof(last), line);
            fg_async_info();
        }
    }
    G.batch[0].cmd = cmd;
    G.cursor = 0;
    G.open_marker = -1;
    return cmd;
}

VkCommandBuffer fg_exec_until(int marker) {
    if (marker < G.cursor || marker >= G.pass_count || !G.passes[marker].marker) return batch_cmd(0);
    run_to(marker);
    VkCommandBuffer cmd = batch_cmd(G.pass_batch[marker]);
    pass_begin(marker, cmd);
    G.cur = -1;   // the caller's commands are graphics work outside any pass exec (fg_stages)
    G.open_marker = marker;
    return cmd;
}

VkCommandBuffer fg_exec_rest(void) {
    if (G.open_marker >= 0) {
        int m = G.open_marker;
        pass_end(m, G.batch[G.pass_batch[m]].cmd);
        G.cursor = m + 1;
        G.open_marker = -1;
    }
    run_to(G.pass_count);
    int slot = (int)R->frame_slot;
    G.ms_count[slot] = G.pass_count;
    G.ms_pending[slot] = true;
    // Bookkeeping: history content now valid, imported layouts written back.
    for (int i = 0; i < G.res_count; i++) {
        FgResource *r = &G.res[i];
        if (r->kind == FG_IMPORT) {
            if (r->layout_io) *r->layout_io = r->layout;
            continue;
        }
        if (r->phys < 0) continue;
        FgPhys *p = &G.pool[r->phys];
        p->last_frame = G.frame;
        if (r->kind == FG_HISTORY && r->first >= 0) p->valid = true;
    }
    for (int b = G.batch_n - 1; b > 0; b--)
        if (G.batch[b].queue == 0) return batch_cmd(b);
    return G.batch[0].cmd;
}

void fg_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal, VkFence fence) {
    PROF_BEGIN("fg:submit");
    int lg = 0, la = -1;
    for (int b = 0; b < G.batch_n; b++) {
        FgBatch *bt = &G.batch[b];
        if (bt->queue == 0) lg = b;
        else la = b;
        VK_CHECK(vkEndCommandBuffer(batch_cmd(b)));
        if (bt->signal) bt->value = ++G.timeline_value[bt->queue];
    }
    // One vkQueueSubmit2 per queue, each queue's batches in order (timeline semaphores allow a wait
    // submitted before its signal: the graphics queue goes first, its pre batch starts at once).
    VkSubmitInfo2 si[2][FG_MAX_BATCHES];
    VkSemaphoreSubmitInfo ws[FG_MAX_BATCHES][3], ss[FG_MAX_BATCHES][2];
    VkCommandBufferSubmitInfo cb[FG_MAX_BATCHES];
    u32 nsi[2] = { 0, 0 };
    for (int b = 0; b < G.batch_n; b++) {
        FgBatch *bt = &G.batch[b];
        int nw = 0, ns = 0;
        if (bt->wait != FG_NO_WAIT) {
            const FgBatch *wb = &G.batch[bt->wait == FG_PRE ? 0 : G.pass_batch[bt->wait]];
            ws[b][nw++] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, G.timeline[wb->queue], wb->value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
        }
        if (b == lg) {
            if (la >= 0)
                ws[b][nw++] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, G.timeline[1], G.batch[la].value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
            if (wait) ws[b][nw++] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, wait, 0, wait_stage, 0 };
        }
        if (bt->signal)
            ss[b][ns++] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, G.timeline[bt->queue], bt->value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
        if (b == lg && signal) ss[b][ns++] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, signal, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
        cb[b] = (VkCommandBufferSubmitInfo){ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, bt->cmd, 0 };
        VkSubmitInfo2 *i = &si[bt->queue][nsi[bt->queue]++];
        *i = (VkSubmitInfo2){ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        i->waitSemaphoreInfoCount = (u32)nw;
        i->pWaitSemaphoreInfos = ws[b];
        i->commandBufferInfoCount = 1;
        i->pCommandBufferInfos = &cb[b];
        i->signalSemaphoreInfoCount = (u32)ns;
        i->pSignalSemaphoreInfos = ss[b];
    }
    VK_CHECK(vkQueueSubmit2(R->queue, nsi[0], si[0], fence));
    if (nsi[1]) VK_CHECK(vkQueueSubmit2(G.async_kind == 1 ? R->compute_queue : R->gfx_queue2, nsi[1], si[1], VK_NULL_HANDLE));
    PROF_END();
}

// ------------------------------------------------------------------ accessors

VkAttachmentStoreOp fg_store_op(FgRes r) {
    if (r < 0 || r >= G.res_count || G.res[r].kind != FG_TRANSIENT || G.cur < 0) return VK_ATTACHMENT_STORE_OP_STORE;
    for (int i = G.cur + 1; i < G.pass_count; i++)
        for (int u = 0; u < G.passes[i].use_count; u++)
            if (G.passes[i].uses[u].res == r) return VK_ATTACHMENT_STORE_OP_STORE;
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

bool fg_valid(FgRes r) { return r >= 0 && r < G.res_count && (G.res[r].kind == FG_IMPORT || G.res[r].phys >= 0); }
VkImageView fg_view(FgRes r) {
    if (!fg_valid(r)) return VK_NULL_HANDLE;
    return G.res[r].kind == FG_IMPORT ? G.res[r].view : G.pool[G.res[r].phys].view;
}
VkImageView fg_mip_view(FgRes r, u32 mip) {
    if (!fg_valid(r) || G.res[r].kind == FG_IMPORT) return fg_view(r);
    FgPhys *p = &G.pool[G.res[r].phys];
    if (p->key.mips <= 1) return p->view;
    return p->mip_views[DG_MIN(mip, p->key.mips - 1)];
}
VkImage fg_image(FgRes r) {
    if (!fg_valid(r)) return VK_NULL_HANDLE;
    return G.res[r].kind == FG_IMPORT ? G.res[r].image : G.pool[G.res[r].phys].image;
}
VkFormat fg_format(FgRes r) { return r >= 0 && r < G.res_count ? G.res[r].key.format : VK_FORMAT_UNDEFINED; }
void fg_extent(FgRes r, u32 *w, u32 *h) {
    *w = *h = 1;
    if (r < 0 || r >= G.res_count) return;
    *w = G.res[r].key.w;
    *h = G.res[r].key.h;
}
u32 fg_mips(FgRes r) { return r >= 0 && r < G.res_count ? G.res[r].key.mips : 1; }
// Valid both at build time (pool lookup by name: written last frame with the same description)
// and inside exec callbacks.
bool fg_history_valid(FgRes r) {
    if (r < 0 || r >= G.res_count || G.res[r].kind != FG_HISTORY) return false;
    FgResource *res = &G.res[r];
    if (res->phys >= 0) return res->valid_at_begin;
    for (int k = 0; k < FG_POOL_MAX; k++) {
        FgPhys *p = &G.pool[k];
        if (p->used && p->history[0] && !strcmp(p->history, res->name))
            return p->valid && p->last_frame + 1 == G.frame && key_eq(&p->key, &res->key);
    }
    return false;
}

void fg_mip_barrier(VkCommandBuffer cmd, FgRes r) {
    (void)r;
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

FgRes fg_find(const char *name) {
    for (int i = G.res_count - 1; i >= 0; i--) if (!strcmp(G.res[i].name, name) && fg_valid(i)) return i;
    return FG_NONE;
}
int fg_pass_count(void) { return G.last_count; }
const char *fg_pass_name(int i) { return i >= 0 && i < G.last_count ? G.last_names[i] : ""; }
float fg_pass_ms(int i) { return i >= 0 && i < G.last_count ? G.last_ms[i] : 0.0f; }
u64 fg_memory_bytes(void) {
    u64 t = 0;
    for (int k = 0; k < FG_POOL_MAX; k++) if (G.pool[k].used) t += G.pool[k].bytes;
    return t;
}
int fg_pool_images(void) {
    int n = 0;
    for (int k = 0; k < FG_POOL_MAX; k++) n += G.pool[k].used;
    return n;
}

// ------------------------------------------------------------------ profile (docs/PERF_AUDIT.md)

// Bytes per texel of the formats the graph uses (bandwidth / over-allocation estimates).
static u32 format_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
    default: return 4;
    }
}

// The last executed graph: passes with their accesses, resources with size / format / lifetime and
// the validation report. Written by the profiler between frames (the graph is still the last one).
static void write_profile(FILE *f) {
    static const char *acc[] = { "sampled", "storage", "color", "depth", "depth_read", "transfer_src", "transfer_dst" };
    static const char *kinds[] = { "transient", "history", "import" };
    const FgReport *rep = fg_last_report();
    fprintf(f, "{\"render_w\":%u,\"render_h\":%u,\"output_w\":%u,\"output_h\":%u,\"pool_images\":%d,\"pool_mb\":%.2f,\"stale_images\":%d,\"stale_mb\":%.2f,", G.render_w,
            G.render_h, G.output_w, G.output_h, rep->pool_images, (double)rep->pool_bytes / 1048576.0, rep->stale_images, (double)rep->stale_bytes / 1048576.0);
    fprintf(f, "\"dead_passes\":[");
    for (int i = 0; i < DG_MIN(rep->dead_passes, 12); i++) { if (i) fputc(',', f); prof_json_str(f, rep->dead[i]); }
    fprintf(f, "],\"unused_res\":[");
    for (int i = 0; i < DG_MIN(rep->unused_res, 12); i++) { if (i) fputc(',', f); prof_json_str(f, rep->unused[i]); }
    fprintf(f, "],\"resources\":[");
    for (int i = 0; i < G.res_count; i++) {
        const FgResource *r = &G.res[i];
        fprintf(f, "%s\n {\"name\":", i ? "," : "");
        prof_json_str(f, r->name);
        u64 bytes = r->kind != FG_IMPORT && r->phys >= 0 ? G.pool[r->phys].bytes : (u64)r->key.w * r->key.h * format_bytes(r->key.format);
        fprintf(f, ",\"kind\":\"%s\",\"format\":%d,\"bpp\":%u,\"w\":%u,\"h\":%u,\"d\":%u,\"mips\":%u,\"bytes\":%llu,\"first\":%d,\"last\":%d,\"phys\":%d}", kinds[r->kind],
                (int)r->key.format, format_bytes(r->key.format), r->key.w, r->key.h, r->key.d, r->key.mips, (unsigned long long)bytes, r->first, r->last, r->phys);
    }
    fprintf(f, "],\"passes\":[");
    for (int p = 0; p < G.pass_count; p++) {
        const FgPassRec *pr = &G.passes[p];
        fprintf(f, "%s\n {\"name\":", p ? "," : "");
        prof_json_str(f, pr->name);
        fprintf(f, ",\"post\":%s,\"uses\":[", fid_pass_is_post(pr->name) ? "true" : "false");
        for (int u = 0; u < pr->use_count; u++)
            fprintf(f, "%s{\"res\":%d,\"access\":\"%s\",\"write\":%s,\"ref\":%s}", u ? "," : "", pr->uses[u].res, acc[pr->uses[u].access], pr->uses[u].write ? "true" : "false",
                    pr->uses[u].ref ? "true" : "false");
        fprintf(f, "]}");
    }
    fprintf(f, "]}");
}
