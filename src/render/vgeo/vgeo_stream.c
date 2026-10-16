#include "vgeo_stream.h"
#include "core/log.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { VGSS_FREE, VGSS_LOADING, VGSS_RESIDENT };

u32 vg_stream_pool_slots(u32 pool_mb) {
    u64 n = (u64)pool_mb * 1024u * 1024u / VGEO_PAGE_SIZE;
    return n ? (u32)DG_MIN(n, (u64)(1u << (32 - VG_CLUSTER_BITS)) - 1) : 1;
}

// ------------------------------------------------------------------ file reader

static int worker_main(void *user) {
    VgStream *st = (VgStream *)user;
    for (;;) {
        SDL_WaitSemaphore(st->work_sem);
        if (SDL_GetAtomicInt(&st->quit)) break;
        SDL_LockMutex(st->work_mutex);
        u32 li = st->work_queue[st->work_head++ % st->work_cap];
        SDL_UnlockMutex(st->work_mutex);
        VgStreamLoad *l = &st->loads[li];
        l->ok = vgeo_scene_read_page(l->scene, l->page, l->dst);
        SDL_AddAtomicInt(&st->batch_left[l->batch], -1);
    }
    return 0;
}

static void queue_read(VgStream *st, u32 li) {
    if (!st->worker) {
        st->work_sem = SDL_CreateSemaphore(0);
        st->work_mutex = SDL_CreateMutex();
        st->worker = SDL_CreateThread(worker_main, "vgeo_stream", st);
    }
    SDL_LockMutex(st->work_mutex);
    st->work_queue[st->work_tail++ % st->work_cap] = li;
    SDL_UnlockMutex(st->work_mutex);
    SDL_SignalSemaphore(st->work_sem);
}

static void wait_batch(VgStream *st, u32 r) {
    while (SDL_GetAtomicInt(&st->batch_left[r]) > 0) SDL_Delay(0);
}

// ------------------------------------------------------------------ state

static void free_state(VgStream *st) {
    free(st->last_used);
    free(st->release);
    free(st->slot_page);
    free(st->slot_state);
    free(st->loading);
    free(st->dependents);
    free(st->parent_first);
    free(st->parents);
    free(st->req_mark);
    free(st->req_at);
    free(st->reqs);
    st->last_used = st->release = st->slot_page = st->dependents = st->parent_first = st->parents = st->req_mark = st->req_at = NULL;
    st->slot_state = st->loading = NULL;
    st->reqs = NULL;
    for (u32 f = 0; f < VG_MAX_FRAMES; f++) vgc_buffer_free(st->ctx, &st->readback[f]);
}

typedef struct DepEdge { u32 child, parent; } DepEdge;
static int edge_cmp(const void *a, const void *b) {
    const DepEdge *x = (const DepEdge *)a, *y = (const DepEdge *)b;
    if (x->child != y->child) return x->child < y->child ? -1 : 1;
    return x->parent < y->parent ? -1 : (x->parent > y->parent);
}

// Page dependencies: page P depends on page Q when a cluster of Q was simplified from a group of P
// (Q holds coarser parents of P's clusters). Residency is kept closed under it, so a missing group's
// whole subtree is missing and its parents stand in alone (no overlap with finer resident pages).
static void build_parents(VgStream *st) {
    const VgScene *s = st->scene;
    size_t n = 0, cap = 1024;
    DepEdge *e = (DepEdge *)malloc(sizeof(DepEdge) * cap);
    for (u32 q = 0; q < s->page_count; q++) {
        const VgMesh *m = &s->meshes[s->page_mesh[q]];
        u32 nc = vgeo_scene_page_header(s, q)->cluster_count;
        for (u32 k = 0; k < nc; k++) {
            u32 child = vgeo_scene_page_cluster(s, q, k)->child_group;
            if (child == ~0u) continue;
            u32 p = m->page_base + s->groups[m->group_base + child].page;
            if (p == q) continue;
            if (n == cap) e = (DepEdge *)realloc(e, sizeof(DepEdge) * (cap *= 2));
            e[n].child = p;
            e[n].parent = q;
            n++;
        }
    }
    qsort(e, n, sizeof(DepEdge), edge_cmp);
    st->parent_first = (u32 *)calloc(s->page_count + 1, sizeof(u32));
    st->parents = (u32 *)malloc(sizeof(u32) * (n + 1));
    u32 m = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && e[i].child == e[i - 1].child && e[i].parent == e[i - 1].parent) continue;
        st->parents[m++] = e[i].parent;
        st->parent_first[e[i].child + 1]++;
    }
    for (u32 p = 0; p < s->page_count; p++) st->parent_first[p + 1] += st->parent_first[p];
    free(e);
}

static void add_dependents(VgStream *st, u32 page, int d) {
    for (u32 k = st->parent_first[page]; k < st->parent_first[page + 1]; k++) st->dependents[st->parents[k]] += (u32)d;
}

// (Re)builds the state from a freshly committed scene (root pages resident).
static void sync(VgStream *st) {
    VgScene *s = st->scene;
    const VgContext *c = st->ctx;
    vkDeviceWaitIdle(c->device);
    for (u32 r = 0; r < VG_STREAM_RING; r++) { wait_batch(st, r); st->batch_count[r] = 0; }
    free_state(st);
    u32 slots = s->pool_slots, pages = s->page_count;
    st->slots = slots;
    st->pages = pages;
    st->last_used = (u32 *)calloc(slots + 1, sizeof(u32));
    st->release = (u32 *)calloc(slots + 1, sizeof(u32));
    st->slot_page = (u32 *)malloc(sizeof(u32) * (slots + 1));
    st->slot_state = (u8 *)calloc(slots + 1, 1);
    st->loading = (u8 *)calloc(pages + 1, 1);
    st->dependents = (u32 *)calloc(pages + 1, sizeof(u32));
    st->req_mark = (u32 *)calloc(pages + 1, sizeof(u32));
    st->req_at = (u32 *)calloc(pages + 1, sizeof(u32));
    st->req_cap = 1024;
    st->reqs = (VgStreamReq *)malloc(sizeof(VgStreamReq) * st->req_cap);
    build_parents(st);
    for (u32 i = 0; i < slots; i++) {
        st->slot_page[i] = s->slot_page[i];
        st->slot_state[i] = s->slot_page[i] == ~0u ? VGSS_FREE : VGSS_RESIDENT;
        if (s->slot_page[i] != ~0u) add_dependents(st, s->slot_page[i], 1);
    }
    u32 caps = st->cull->caps.requests;
    st->rb_view_offset = ((VkDeviceSize)slots * 4 + 15) & ~(VkDeviceSize)15;
    st->rb_view_size = 16 + 8ull * caps;
    for (u32 f = 0; f < c->frames; f++) {
        st->readback[f] = vgc_buffer(c, st->rb_view_offset + VG_MAX_VIEWS * st->rb_view_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
        st->readback_valid[f] = false;
    }
    VkDescriptorBufferInfo bi[2] = { { st->staging.buffer, 0, VK_WHOLE_SIZE }, { s->b_page_table.buffer, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet w[2];
    memset(w, 0, sizeof(w));
    for (u32 i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = st->set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(c->device, 2, w, 0, NULL);
    st->generation = s->generation;
}

// ------------------------------------------------------------------ init

bool vg_stream_init(VgStream *st, VgScene *scene, VgCuller *cull, const VgStreamConfig *cfg) {
    memset(st, 0, sizeof(*st));
    st->scene = scene;
    st->cull = cull;
    st->ctx = scene->ctx;
    st->cfg = *cfg;
    st->ppf_cap = DG_MAX(1u, cfg->pages_per_frame);
    st->generation = ~0u;
    const VgContext *c = st->ctx;
    VkDevice d = c->device;
    VkDescriptorSetLayoutBinding b[2];
    memset(b, 0, sizeof(b));
    for (u32 i = 0; i < 2; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &li, NULL, &st->layout) != VK_SUCCESS) return false;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &st->layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(d, &pi, NULL, &st->pl) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dp, NULL, &st->dpool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = st->dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &st->layout;
    if (vkAllocateDescriptorSets(d, &ai, &st->set) != VK_SUCCESS) return false;
    st->p_patch = vgc_compute_pipeline(c, st->pl, "vgeo/vg_stream_patch.comp");
    if (!st->p_patch) return false;
    // Staging ring: per region the page payloads, then the patch entries (loads + evictions +
    // the previous frame's file reads).
    st->patch_cap = 4 * st->ppf_cap + 16;
    st->patch_offset = (VkDeviceSize)st->ppf_cap * VGEO_PAGE_SIZE;
    st->region_size = st->patch_offset + (((VkDeviceSize)st->patch_cap * 8 + 255) & ~(VkDeviceSize)255);
    st->staging = vgc_buffer(c, st->region_size * VG_STREAM_RING, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_UPLOAD);
    if (!st->staging.buffer || !st->staging.mapped) return false;
    st->loads = (VgStreamLoad *)calloc((size_t)VG_STREAM_RING * st->ppf_cap, sizeof(VgStreamLoad));
    st->work_cap = VG_STREAM_RING * st->ppf_cap;
    st->work_queue = (u32 *)malloc(sizeof(u32) * st->work_cap);
    if (c->timestamp_period > 0) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * VG_MAX_FRAMES;
        vkCreateQueryPool(d, &qi, NULL, &st->qp);
    }
    vgeo_scene_set_pool(scene, vg_stream_pool_slots(cfg->pool_mb));
    if (scene->generation && !vgeo_scene_commit(scene)) return false;
    st->ok = true;
    return true;
}

void vg_stream_shutdown(VgStream *st) {
    if (!st->ctx) return;
    VkDevice d = st->ctx->device;
    vkDeviceWaitIdle(d);
    if (st->worker) {
        for (u32 r = 0; r < VG_STREAM_RING; r++) wait_batch(st, r);
        SDL_SetAtomicInt(&st->quit, 1);
        SDL_SignalSemaphore(st->work_sem);
        SDL_WaitThread(st->worker, NULL);
        SDL_DestroySemaphore(st->work_sem);
        SDL_DestroyMutex(st->work_mutex);
    }
    free_state(st);
    free(st->loads);
    free(st->work_queue);
    vgc_buffer_free(st->ctx, &st->staging);
    if (st->qp) vkDestroyQueryPool(d, st->qp, NULL);
    if (st->p_patch) vkDestroyPipeline(d, st->p_patch, NULL);
    if (st->dpool) vkDestroyDescriptorPool(d, st->dpool, NULL);
    if (st->pl) vkDestroyPipelineLayout(d, st->pl, NULL);
    if (st->layout) vkDestroyDescriptorSetLayout(d, st->layout, NULL);
    vgeo_scene_set_pool(st->scene, 0);
    memset(st, 0, sizeof(*st));
}

void vg_stream_set_config(VgStream *st, const VgStreamConfig *cfg) {
    st->cfg = *cfg;
    vgeo_scene_set_pool(st->scene, vg_stream_pool_slots(cfg->pool_mb));
}

const VgStreamStats *vg_stream_stats(const VgStream *st) { return &st->stats; }

// ------------------------------------------------------------------ update

typedef struct Upd {
    u32 cur, region, issued, budget, scan;
    VkBufferCopy *copies;
    u32 ncopies;
    u32 *patch;                  // (page, slot) pairs in the staging region
    u32 npatch;
} Upd;

static void add_patch(VgStream *st, Upd *x, u32 page, u32 slot) {
    if (x->npatch >= st->patch_cap) return;   // cannot happen: loads + evictions + completions <= 3 ppf_cap
    x->patch[2 * x->npatch] = page;
    x->patch[2 * x->npatch + 1] = slot;
    x->npatch++;
}

// A read finished: map the page (copy + patch), or give the slot back.
static void complete(VgStream *st, Upd *x, const VgStreamLoad *l, VkDeviceSize src) {
    VgScene *s = st->scene;
    st->loading[l->page] = 0;
    if (!l->ok) {
        if (!st->logged_fail) log_error("vgeo: cannot read page %u", l->page);
        st->logged_fail = true;
        st->slot_state[l->slot] = VGSS_FREE;
        st->slot_page[l->slot] = ~0u;
        st->release[l->slot] = x->cur;
        add_dependents(st, l->page, -1);
        return;
    }
    st->slot_state[l->slot] = VGSS_RESIDENT;
    s->slot_page[l->slot] = l->page;
    s->page_table[l->page] = l->slot;
    VkBufferCopy bc = { src, (VkDeviceSize)l->slot * VGEO_PAGE_SIZE, l->size };
    x->copies[x->ncopies++] = bc;
    add_patch(st, x, l->page, l->slot);
    st->stats.loaded++;
    st->stats.loaded_total++;
}

static u32 take_slot(VgStream *st, Upd *x) {
    for (; x->scan < st->slots; x->scan++)
        if (st->slot_state[x->scan] == VGSS_FREE && st->release[x->scan] <= x->cur) return x->scan++;
    return ~0u;
}

// Issues the load of a page, its missing parents first (they are coarser: the recursion ends at
// the root pages). False when the budget or the free slots run out.
static bool issue(VgStream *st, Upd *x, u32 page, int depth) {
    VgScene *s = st->scene;
    if (x->issued >= x->budget || depth > 64) return false;
    for (u32 k = st->parent_first[page]; k < st->parent_first[page + 1]; k++) {
        u32 q = st->parents[k];
        if (s->page_table[q] != ~0u || st->loading[q]) continue;
        if (!issue(st, x, q, depth + 1)) return false;
    }
    if (x->issued >= x->budget) return false;
    u32 slot = take_slot(st, x);
    if (slot == ~0u) return false;
    u32 li = x->region * st->ppf_cap + x->issued;
    VgStreamLoad *l = &st->loads[li];
    l->scene = s;
    l->page = page;
    l->slot = slot;
    l->size = s->page_desc[page].size;
    l->batch = x->region;
    l->dst = (u8 *)st->staging.mapped + x->region * st->region_size + (VkDeviceSize)x->issued * VGEO_PAGE_SIZE;
    l->async = s->sources[s->page_mesh[page]].data == NULL;
    x->issued++;
    st->batch_count[x->region] = x->issued;
    st->slot_state[slot] = VGSS_LOADING;
    st->slot_page[slot] = page;
    st->last_used[slot] = x->cur;
    st->loading[page] = 1;
    add_dependents(st, page, 1);
    if (l->async) {
        SDL_AddAtomicInt(&st->batch_left[x->region], 1);
        queue_read(st, li);
    } else {
        l->ok = vgeo_scene_read_page(s, page, l->dst);
        complete(st, x, l, (VkDeviceSize)((u8 *)l->dst - (u8 *)st->staging.mapped));
    }
    return true;
}

typedef struct Victim { u32 last_used, slot; } Victim;
static int victim_cmp(const void *a, const void *b) {
    const Victim *x = (const Victim *)a, *y = (const Victim *)b;
    if (x->last_used != y->last_used) return x->last_used < y->last_used ? -1 : 1;
    return x->slot < y->slot ? -1 : (x->slot > y->slot);
}

// LRU: unmaps up to `need` slots unused for frames + 1 frames (the CPU mirror is frames + 1 frames
// old, so "unused in the newest frame read back"), never root pages or pages others depend on. The
// slots are reusable `frames` frames later: every frame that could still see the page has completed.
static void evict(VgStream *st, Upd *x, u32 need) {
    VgScene *s = st->scene;
    u32 lag = st->ctx->frames + 1;
    if (x->cur <= lag) return;
    Victim *v = (Victim *)malloc(sizeof(Victim) * (st->slots + 1));
    u32 n = 0;
    for (u32 i = 0; i < st->slots; i++) {
        if (st->slot_state[i] != VGSS_RESIDENT) continue;
        u32 p = st->slot_page[i];
        if (s->page_root[p] || st->dependents[p] || st->last_used[i] >= x->cur - lag) continue;
        v[n].last_used = st->last_used[i];
        v[n].slot = i;
        n++;
    }
    qsort(v, n, sizeof(Victim), victim_cmp);
    u32 done = 0;
    for (u32 k = 0; k < n && done < need; k++) {
        u32 i = v[k].slot, p = st->slot_page[i];
        if (st->dependents[p]) continue;   // a child evicted above does not free its parent this frame
        s->page_table[p] = ~0u;
        s->slot_page[i] = ~0u;
        st->slot_page[i] = ~0u;
        st->slot_state[i] = VGSS_FREE;
        st->release[i] = x->cur + st->ctx->frames;
        add_dependents(st, p, -1);
        add_patch(st, x, p, ~0u);
        done++;
    }
    st->stats.evicted += done;
    st->stats.evicted_total += done;
    free(v);
}

static int req_cmp(const void *a, const void *b) {
    const VgStreamReq *x = (const VgStreamReq *)a, *y = (const VgStreamReq *)b;
    if (x->prio != y->prio) return x->prio > y->prio ? -1 : 1;
    return x->page < y->page ? -1 : (x->page > y->page);
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

void vg_stream_update(VgStream *st, VkCommandBuffer cmd, u32 frame) {
    VgScene *s = st->scene;
    if (!st->ok || !s->b_page_table.buffer) return;
    u64 t0 = SDL_GetPerformanceCounter();
    if (s->generation != st->generation) sync(st);
    const VgContext *c = st->ctx;
    Upd x;
    memset(&x, 0, sizeof(x));
    x.cur = ++st->frame;
    s->stamp_frame = x.cur;
    x.region = x.cur % VG_STREAM_RING;
    x.budget = DG_MIN(DG_MAX(1u, st->cfg.pages_per_frame), st->ppf_cap);
    x.patch = (u32 *)((u8 *)st->staging.mapped + x.region * st->region_size + st->patch_offset);
    x.copies = (VkBufferCopy *)malloc(sizeof(VkBufferCopy) * 2 * st->ppf_cap);
    st->stats.loaded = st->stats.evicted = st->stats.requested = st->stats.waiting = 0;
    u32 prev = (x.cur + VG_STREAM_RING - 1) % VG_STREAM_RING;
    st->batch_count[x.region] = 0;

    // 1. The readback of this frame slot (its fence has signalled): GPU ms, stamps, requests.
    u32 nreq = 0;
    if (frame < c->frames && st->readback_valid[frame]) {
        if (st->qp && st->qp_written[frame]) {
            u64 ts[2];
            if (vkGetQueryPoolResults(c->device, st->qp, frame * 2, 2, sizeof(ts), ts, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                st->stats.gpu_ms = (float)(ts[1] - ts[0]) * c->timestamp_period * 1e-6f;
        }
        const VgBuffer *rb = &st->readback[frame];
        vmaInvalidateAllocation(c->vma, rb->alloc, 0, VK_WHOLE_SIZE);
        const u32 *stamps = (const u32 *)rb->mapped;
        for (u32 i = 0; i < st->slots; i++)
            if (st->slot_state[i] == VGSS_RESIDENT && stamps[i] > st->last_used[i]) st->last_used[i] = stamps[i];
        for (u32 v = 0; v < VG_MAX_VIEWS; v++) {
            if (!(st->rb_views[frame] & (1u << v))) continue;
            const u32 *e = (const u32 *)((const u8 *)rb->mapped + st->rb_view_offset + v * st->rb_view_size);
            u32 n = DG_MIN(e[0], st->cull->caps.requests);
            for (u32 k = 0; k < n; k++) {
                u32 page = e[4 + 2 * k];
                float prio;
                memcpy(&prio, &e[4 + 2 * k + 1], 4);
                if (page >= st->pages || s->page_table[page] != ~0u || st->loading[page]) continue;
                if (st->req_mark[page] == x.cur) {   // duplicate: keep the highest priority
                    VgStreamReq *r = &st->reqs[st->req_at[page]];
                    if (prio > r->prio) r->prio = prio;
                    continue;
                }
                st->req_mark[page] = x.cur;
                st->req_at[page] = nreq;
                if (nreq == st->req_cap) st->reqs = (VgStreamReq *)realloc(st->reqs, sizeof(VgStreamReq) * (st->req_cap *= 2));
                st->reqs[nreq].prio = prio;
                st->reqs[nreq].page = page;
                nreq++;
            }
        }
        st->readback_valid[frame] = false;
    }

    // 2. Last frame's file reads (waited for if still running: a batch lands within one frame).
    if (st->batch_count[prev]) {
        wait_batch(st, prev);
        for (u32 k = 0; k < st->batch_count[prev]; k++) {
            VgStreamLoad *l = &st->loads[prev * st->ppf_cap + k];
            if (l->async) complete(st, &x, l, (VkDeviceSize)((u8 *)l->dst - (u8 *)st->staging.mapped));
        }
        st->batch_count[prev] = 0;
    }

    // 3. New loads by priority, 4. eviction when the pool is short.
    qsort(st->reqs, nreq, sizeof(VgStreamReq), req_cmp);
    st->stats.requested = nreq;
    u32 waiting = 0;
    for (u32 k = 0; k < nreq; k++) {
        u32 p = st->reqs[k].page;
        if (s->page_table[p] != ~0u || st->loading[p]) continue;   // loaded as a parent above
        if (!issue(st, &x, p, 0)) waiting++;
    }
    st->stats.waiting = waiting;
    if (waiting) {
        u32 free_now = 0, quarantined = 0;
        for (u32 i = 0; i < st->slots; i++)
            if (st->slot_state[i] == VGSS_FREE) {
                if (st->release[i] <= x.cur) free_now++;
                else quarantined++;
            }
        u32 want = DG_MIN(waiting, x.budget);
        if (want > free_now + quarantined) evict(st, &x, want - free_now - quarantined);
    }

    // 5. Record: readback copies of what the previous frames wrote, page copies, page-table scatter.
    if (st->qp) {
        vkCmdResetQueryPool(cmd, st->qp, frame * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, st->qp, frame * 2);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (frame < c->frames) {
        const VgBuffer *rb = &st->readback[frame];
        VkBufferCopy sc = { 0, 0, (VkDeviceSize)st->slots * 4 };
        vkCmdCopyBuffer(cmd, s->b_stamps.buffer, rb->buffer, 1, &sc);
        st->rb_views[frame] = 0;
        for (u32 v = 0; v < VG_MAX_VIEWS; v++) {
            const VgViewRes *vr = &st->cull->v[v];
            if (!vr->allocated) continue;
            VkDeviceSize o = st->rb_view_offset + v * st->rb_view_size;
            VkBufferCopy cc = { offsetof(VgCounters, n) + VGN_PAGE_REQ * 4, o, 4 };
            VkBufferCopy rc = { 0, o + 16, 8ull * st->cull->caps.requests };
            vkCmdCopyBuffer(cmd, vr->counters.buffer, rb->buffer, 1, &cc);
            vkCmdCopyBuffer(cmd, vr->requests.buffer, rb->buffer, 1, &rc);
            st->rb_views[frame] |= 1u << v;
        }
        st->readback_valid[frame] = true;
        if (st->qp) st->qp_written[frame] = true;
    }
    if (x.ncopies) vkCmdCopyBuffer(cmd, st->staging.buffer, s->b_pool.buffer, x.ncopies, x.copies);
    if (x.npatch) {
        vmaFlushAllocation(c->vma, st->staging.alloc, x.region * st->region_size, st->region_size);
        u32 pc[2] = { (u32)((x.region * st->region_size + st->patch_offset) / 8), x.npatch };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, st->p_patch);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, st->pl, 0, 1, &st->set, 0, NULL);
        vkCmdPushConstants(cmd, st->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        vkCmdDispatch(cmd, (x.npatch + 63) / 64, 1, 1);
    } else if (x.ncopies) {
        vmaFlushAllocation(c->vma, st->staging.alloc, x.region * st->region_size, st->region_size);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
    if (st->qp) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, st->qp, frame * 2 + 1);
    free(x.copies);

    u32 resident = 0, free_slots = 0, pending = 0;
    for (u32 i = 0; i < st->slots; i++) {
        resident += st->slot_state[i] == VGSS_RESIDENT;
        free_slots += st->slot_state[i] == VGSS_FREE;
    }
    for (u32 k = 0; k < st->batch_count[x.region]; k++) pending += st->loads[x.region * st->ppf_cap + k].async;
    st->stats.pool_slots = st->slots;
    st->stats.resident = resident;
    st->stats.free = free_slots;
    st->stats.pending = pending;
    st->stats.cpu_ms = (float)((double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
}
