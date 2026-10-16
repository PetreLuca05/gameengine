#include "fg.h"
#include "fg_check.h"
#include "render/fidelity/fidelity.h"
#include "core/cvar.h"
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
    bool valid_at_begin;      // history: content was valid when the frame started
} FgResource;

typedef struct FgUse { FgRes res; FgAccess access; bool write, ref; } FgUse;

typedef struct FgPassRec {
    char name[32];
    FgExecFn exec;
    void *user;
    bool enabled;
    FgUse uses[16];
    int use_count;
} FgPassRec;

#define FG_POOL_MAX 128

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
    FgReport report;          // the last executed graph (fg_check + pool state)
} G;

void fg_init(void) {
    memset(&G, 0, sizeof(G));
    G.init = true;
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = FG_MAX_PASSES * 2 * FRAMES_IN_FLIGHT;
    VK_CHECK(vkCreateQueryPool(R->device, &qi, NULL, &G.qp));
    G.disable = cvar_reg_string("r_post_disable", "", 0, "comma separated frame graph passes to skip (e.g. ssr,gtao); r_post_passes lists them");
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
    G.init = false;
}

void fg_targets_changed(void) {
    if (!G.init) return;
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
}

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

int fg_pass(const char *name, FgExecFn exec, void *user) {
    if (G.pass_count >= FG_MAX_PASSES) { log_error("fg: too many passes"); return -1; }
    if (!fg_pass_enabled(name)) return -1;
    FgPassRec *p = &G.passes[G.pass_count];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->exec = exec;
    p->user = user;
    p->enabled = true;
    return G.pass_count++;
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

static void res_state(FgResource *r, VkImage *img, VkImageLayout **layout, VkPipelineStageFlags2 **stage, VkAccessFlags2 **access, bool **lw, u32 *mips) {
    if (r->kind == FG_IMPORT) {
        *img = r->image;
        *layout = &r->layout;
        *stage = &r->stage;
        *access = &r->access;
        *lw = &r->last_write;
        *mips = 1;
    } else {
        FgPhys *p = &G.pool[r->phys];
        *img = p->image;
        *layout = &p->layout;
        *stage = &p->stage;
        *access = &p->access;
        *lw = &p->last_write;
        *mips = p->key.mips;
    }
}

static void pass_barriers(VkCommandBuffer cmd, FgPassRec *p) {
    VkImageMemoryBarrier2 bars[16];
    int n = 0;
    for (int u = 0; u < p->use_count; u++) {
        FgResource *r = &G.res[p->uses[u].res];
        if (r->kind != FG_IMPORT && r->phys < 0) continue;
        // Read + storage write of the same resource in one pass: GENERAL wins.
        FgAccess a = p->uses[u].access;
        bool write = p->uses[u].write;
        for (int v = 0; v < p->use_count; v++)
            if (v != u && p->uses[v].res == p->uses[u].res) {
                if (p->uses[v].access == FG_STORAGE) a = FG_STORAGE;
                write |= p->uses[v].write;
                if (v < u) { a = -1; break; }   // handled by the first use
            }
        if ((int)a < 0) continue;
        State want = state_for(a, r->aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImage img;
        VkImageLayout *lay;
        VkPipelineStageFlags2 *st;
        VkAccessFlags2 *ac;
        bool *lw;
        u32 mips;
        res_state(r, &img, &lay, &st, &ac, &lw, &mips);
        bool hazard = *lay != want.layout || *lw || write;
        if (hazard) {
            VkImageMemoryBarrier2 *b = &bars[n++];
            memset(b, 0, sizeof(*b));
            b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b->srcStageMask = *st ? *st : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b->srcAccessMask = *lw ? *ac : 0;
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
}

const FgReport *fg_last_report(void) { return &G.report; }

// Drops pooled images the last frame didn't use (a settings change: Engine Fidelity level switch)
// instead of waiting for the 240-frame eviction. Deferred destruction (frames in flight).
void fg_trim(void) {
    if (!G.init) return;
    for (int k = 0; k < FG_POOL_MAX; k++)
        if (G.pool[k].used && G.pool[k].last_frame != G.frame) phys_destroy(&G.pool[k], false);
}

void fg_execute(VkCommandBuffer cmd) {
    allocate();
    check_graph();
    int slot = (int)R->frame_slot;
    u32 qbase = (u32)(slot * FG_MAX_PASSES * 2);
    vkCmdResetQueryPool(cmd, G.qp, qbase, FG_MAX_PASSES * 2);
    for (int i = 0; i < G.pass_count; i++) {
        FgPassRec *p = &G.passes[i];
        pass_barriers(cmd, p);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, G.qp, qbase + (u32)i * 2);
        bool label = R->validation && vkCmdBeginDebugUtilsLabelEXT;
        if (label) {
            VkDebugUtilsLabelEXT l = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
            l.pLabelName = p->name;
            vkCmdBeginDebugUtilsLabelEXT(cmd, &l);
        }
        if (p->exec) p->exec(cmd, p->user);
        if (label) vkCmdEndDebugUtilsLabelEXT(cmd);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, G.qp, qbase + (u32)i * 2 + 1);
        memcpy(G.ms_names[slot][i], p->name, 32);
    }
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
}

// ------------------------------------------------------------------ accessors

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
