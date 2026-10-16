#include "foliage_support.h"
#include "vgeo_impostor.h"
#include "core/log.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.2831853f

// ------------------------------------------------------------------ WPO (mirror of shaders/vgeo/wpo.glsl)

static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

float vgeo_wpo_max(u32 wpo, const float prm[4]) {
    float m = 0;
    if ((wpo & 0xFFu) == WPO_GRASS) m = 1.41421356f * fabsf(prm[0]);
    else if ((wpo & 0xFFu) == WPO_TREE) m = fabsf(prm[0]) + fabsf(prm[1]) + 1.5f * fabsf(prm[2]);
    if (wpo & WPO_DISPLACE) m += 1.1180340f * VG_DISPLACE_MAX;
    return m > 0 ? m * 1.0001f + 1e-5f : 0.0f;   // f32 slack of the GPU evaluation
}

void vgeo_wpo_eval(u32 wpo, const float prm[4], const float p[3], const float ip[3], const float col[4], float phase, const float wind[4], float t,
                   const float disp[2], float out[3]) {
    out[0] = out[1] = out[2] = 0;
    float s = clamp01(wind[2] / fmaxf(prm[3], 1e-3f));
    u32 id = wpo & 0xFFu;
    if (id == WPO_GRASS) {
        float osc = 0.65f + 0.35f * sinf(TAU * (prm[2] * t + phase) + ip[0] * 0.37f + ip[2] * 0.21f);
        float b = prm[0] * clamp01(col[0]) * s * osc;
        out[0] = wind[0] * b;
        out[2] = wind[1] * b;
        out[1] = -fminf(0.5f * b * b / fmaxf(prm[1], 0.05f), b);
    } else if (id == WPO_TREE) {
        float ph = TAU * phase;
        float dir[3] = { wind[0], 0, wind[1] }, perp[3] = { -wind[1], 0, wind[0] };
        float sway = prm[0] * clamp01(col[0]) * s * (0.6f + 0.4f * sinf(t * 0.9f + ph));
        float branch = prm[1] * clamp01(col[1]) * s * sinf(t * 2.3f + ph * 1.7f + p[0] * 0.5f + p[1] * 0.3f + p[2] * 0.4f);
        float fl[3] = { sinf(t * 7.1f + p[0] * 3.1f + ph), 0.5f * sinf(t * 8.3f + p[1] * 2.7f + ph), sinf(t * 6.7f + p[2] * 3.3f + ph) };
        float flutter = prm[2] * clamp01(col[2]) * (0.25f + 0.75f * s);
        for (int k = 0; k < 3; k++) out[k] = dir[k] * sway + (dir[k] * 0.6f + perp[k] * 0.8f) * branch + fl[k] * flutter;
    }
    if ((wpo & WPO_DISPLACE) && disp) {
        float d[2] = { disp[0], disp[1] }, l = sqrtf(d[0] * d[0] + d[1] * d[1]);
        if (l > VG_DISPLACE_MAX) { d[0] *= VG_DISPLACE_MAX / l; d[1] *= VG_DISPLACE_MAX / l; }
        float w = clamp01(col[0]);
        out[0] += d[0] * w;
        out[1] += -0.5f * fminf(l, VG_DISPLACE_MAX) * w;
        out[2] += d[1] * w;
    }
}

// ------------------------------------------------------------------ instances

float vgeo_foliage_alpha(const VgMesh *m, u32 fp, float dist, float density) {
    if (!(m->fflags & VGMF_FOLIAGE)) return 1.0f;
    float t = density;
    if (m->fade_end > m->fade_start) t *= clamp01((m->fade_end - dist) / (m->fade_end - m->fade_start));
    float h = ((float)VG_INST_HASH(fp) + 0.5f) * (0.9375f / 256.0f);
    return clamp01((t - h) * 16.0f);
}

float vgeo_project_size(const VgViewGpu *v, const float s[4]) {
    if (v->flags & VGV_ORTHO) return 2.0f * s[3] * v->proj_scale;
    float dx = s[0] - v->cam_pos[0], dy = s[1] - v->cam_pos[1], dz = s[2] - v->cam_pos[2];
    float d = fmaxf(sqrtf(dx * dx + dy * dy + dz * dz) - s[3], v->znear);
    return 2.0f * s[3] * v->proj_scale / d;
}

bool vgeo_use_impostor(const VgViewGpu *v, const VgMesh *m, const float s[4]) {
    return m->impostor != ~0u && v->foliage[3] > 0.0f && !(v->flags & VGV_ORTHO) && vgeo_project_size(v, s) < v->foliage[3];
}

void vgeo_foliage_instance(VgInstance *out, u32 mesh, const float pos[3], float yaw, float scale) {
    memset(out, 0, sizeof(*out));
    memcpy(out->pos, pos, 12);
    float q[4] = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
    out->rot = vg_quat_encode(q);
    out->scale_h = vgeo_half(scale);
    out->mesh = (u16)mesh;
    u32 h = vgeo_fnv1a(pos, 12, 2166136261u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    out->flags_phase = (h & 0xFFu) | (((h >> 8) & 0xFFu) << 8);
}

// ------------------------------------------------------------------ placement chunks

bool vgp_parse(const u8 *data, size_t size, VgpView *out) {
    memset(out, 0, sizeof(*out));
    if (size < sizeof(VgpHeader)) return false;
    const VgpHeader *h = (const VgpHeader *)data;
    if (h->magic != VGP_MAGIC || h->version != VGP_VERSION || h->cell_count > 16) return false;
    size_t need = sizeof(VgpHeader) + 48ull * h->mesh_count + sizeof(VgCell) * (size_t)h->cell_count + sizeof(VgInstance) * (size_t)h->instance_count;
    if (size < need) return false;
    out->hdr = h;
    out->names = (const char (*)[48])(data + sizeof(VgpHeader));
    out->cells = (const VgCell *)(data + sizeof(VgpHeader) + 48ull * h->mesh_count);
    out->instances = (const VgInstance *)((const u8 *)out->cells + sizeof(VgCell) * h->cell_count);
    for (u32 c = 0; c < h->cell_count; c++)
        if ((u64)out->cells[c].first + out->cells[c].count > h->instance_count) return false;
    for (u32 i = 0; i < h->instance_count; i++)
        if (out->instances[i].mesh >= h->mesh_count) return false;
    return true;
}

u8 *vgp_write(i32 cx, i32 cz, const char (*names)[48], u32 mesh_count, const VgInstance *inst, u32 n, size_t *size) {
    float ox = (float)cx * VGP_CHUNK_SIZE, oz = (float)cz * VGP_CHUNK_SIZE;
    u32 count[16] = { 0 }, at[16];
    u8 *cell_of = (u8 *)malloc(n + 1);
    for (u32 i = 0; i < n; i++) {
        float fx = (inst[i].pos[0] - ox) / VGEO_CELL_SIZE, fz = (inst[i].pos[2] - oz) / VGEO_CELL_SIZE;
        if (fx < -1e-3f || fz < -1e-3f || fx > 4.001f || fz > 4.001f) { free(cell_of); return NULL; }
        int x = DG_CLAMP((int)floorf(fx), 0, 3), z = DG_CLAMP((int)floorf(fz), 0, 3);
        cell_of[i] = (u8)(z * 4 + x);
        count[cell_of[i]]++;
    }
    u32 ncell = 0, acc = 0;
    VgCell cells[16];
    for (u32 c = 0; c < 16; c++) {
        at[c] = acc;
        if (!count[c]) continue;
        VgCell *cl = &cells[ncell++];
        cl->first = acc;
        cl->count = count[c];
        float x0 = ox + (float)(c % 4) * VGEO_CELL_SIZE, z0 = oz + (float)(c / 4) * VGEO_CELL_SIZE;
        cl->min[0] = x0;
        cl->min[2] = z0;
        cl->max[0] = x0 + VGEO_CELL_SIZE;
        cl->max[2] = z0 + VGEO_CELL_SIZE;
        cl->min[1] = FLT_MAX;
        cl->max[1] = -FLT_MAX;
        acc += count[c];
    }
    *size = sizeof(VgpHeader) + 48ull * mesh_count + sizeof(VgCell) * ncell + sizeof(VgInstance) * (size_t)n;
    u8 *out = (u8 *)calloc(1, *size);
    VgpHeader *h = (VgpHeader *)out;
    h->magic = VGP_MAGIC;
    h->version = VGP_VERSION;
    h->mesh_count = mesh_count;
    h->instance_count = n;
    h->chunk_x = cx;
    h->chunk_z = cz;
    h->cell_count = ncell;
    h->origin[0] = ox;
    h->origin[2] = oz;
    h->size = VGP_CHUNK_SIZE;
    memcpy(out + sizeof(VgpHeader), names, 48ull * mesh_count);
    VgInstance *dst = (VgInstance *)(out + sizeof(VgpHeader) + 48ull * mesh_count + sizeof(VgCell) * ncell);
    for (u32 i = 0; i < n; i++) dst[at[cell_of[i]]++] = inst[i];   // stable within a cell
    // cells' y bounds from the instance positions (informative)
    for (u32 c = 0; c < ncell; c++)
        for (u32 i = cells[c].first; i < cells[c].first + cells[c].count; i++) {
            cells[c].min[1] = fminf(cells[c].min[1], dst[i].pos[1]);
            cells[c].max[1] = fmaxf(cells[c].max[1], dst[i].pos[1]);
        }
    memcpy(out + sizeof(VgpHeader) + 48ull * mesh_count, cells, sizeof(VgCell) * ncell);
    free(cell_of);
    return out;
}

u32 vgeo_foliage_chunk_load(VgScene *s, const VgpView *v, u32 (*resolve)(const char *name, void *user), void *user, u32 tag) {
    const VgpHeader *h = v->hdr;
    u32 *map = (u32 *)malloc(sizeof(u32) * (h->mesh_count + 1));
    for (u32 m = 0; m < h->mesh_count; m++) {
        char nm[48];
        memcpy(nm, v->names[m], 48);
        nm[47] = 0;
        map[m] = resolve(nm, user);
        if (map[m] != ~0u && map[m] >= s->mesh_count) map[m] = ~0u;
    }
    VgInstance *inst = (VgInstance *)malloc(sizeof(VgInstance) * (h->instance_count + 1));
    VgCell cells[16];
    u32 n = 0, nc = 0;
    for (u32 c = 0; c < h->cell_count; c++) {
        u32 first = n;
        for (u32 i = v->cells[c].first; i < v->cells[c].first + v->cells[c].count; i++) {
            u32 mm = map[v->instances[i].mesh];
            if (mm == ~0u) continue;
            inst[n] = v->instances[i];
            inst[n].mesh = (u16)mm;
            n++;
        }
        if (n > first) {
            cells[nc] = v->cells[c];
            cells[nc].first = first;
            cells[nc].count = n - first;
            nc++;
        }
    }
    u32 r = n ? vgeo_scene_placement_add(s, cells, nc, inst, n, tag) : ~0u;
    free(inst);
    free(map);
    return r;
}

u32 vgeo_foliage_query(const VgScene *s, const float pos[3], float radius, VgFoliageHit *out, u32 max) {
    const VgPlacements *p = &s->pl;
    u32 n = 0;
    for (u32 c = 0; c < p->cell_cap && max; c++) {
        const VgCell *cl = &p->cells[c];
        if (!cl->count) continue;
        float qx = fmaxf(cl->min[0], fminf(pos[0], cl->max[0])), qz = fmaxf(cl->min[2], fminf(pos[2], cl->max[2]));
        if ((qx - pos[0]) * (qx - pos[0]) + (qz - pos[2]) * (qz - pos[2]) > radius * radius) continue;
        for (u32 i = cl->first; i < cl->first + cl->count; i++) {
            const VgInstance *in = &p->inst[i];
            float dx = in->pos[0] - pos[0], dz = in->pos[2] - pos[2], d = sqrtf(dx * dx + dz * dz);
            if (d > radius) continue;
            // insert nearest first (keep the max nearest)
            u32 at = n;
            while (at > 0 && out[at - 1].dist > d) at--;
            if (at >= max) continue;
            u32 last = n < max ? n : max - 1;
            memmove(&out[at + 1], &out[at], sizeof(VgFoliageHit) * (last - at));
            VgFoliageHit *hh = &out[at];
            hh->instance = s->instance_count + i;
            hh->mesh = in->mesh;
            memcpy(hh->pos, in->pos, 12);
            hh->scale = vgeo_half_to_float(in->scale_h);
            hh->dist = d;
            if (n < max) n++;
        }
    }
    return n;
}

// ------------------------------------------------------------------ impostors on the GPU

bool vgeo_foliage_impostor_upload(VgScene *s, VgTextures *t, u32 mesh, const VgBuilt *b) {
    const u8 *alb, *nrm;
    const VgImpostorHeader *h = vgeo_impostor(b, &alb, &nrm);
    if (!h || mesh >= s->mesh_count) return false;
    u32 levels;
    size_t bytes = vg_tex_mip_bytes(h->size, h->size, &levels);
    // coarse mips of a 8 x 8 atlas would mix frames: stop at 4 px tiles
    u32 keep = 1;
    while (keep < levels && (h->size / h->grid) >> keep >= 4) keep++;
    u8 *a = (u8 *)malloc(bytes), *n = (u8 *)malloc(bytes);
    vg_tex_build_mips(alb, h->size, h->size, a);
    vg_tex_build_mips(nrm, h->size, h->size, n);
    u32 ta = vg_tex_create(t, h->size, h->size, keep, a, true, false);
    u32 tn = vg_tex_create(t, h->size, h->size, keep, n, false, false);
    free(a);
    free(n);
    if (!ta || !tn) {
        if (ta) vg_tex_release(t, ta);
        if (tn) vg_tex_release(t, tn);
        return false;
    }
    VgMesh *m = &s->meshes[mesh];
    m->impostor = ta | tn << 16;
    m->impostor_grid = h->grid;
    m->impostor_radius = h->radius;
    return true;
}

// ------------------------------------------------------------------ displacement buffer

typedef struct DispPush { i32 ox, oz, pox, poz; float decay, dt, texel; u32 npush, valid, tex, pad[2]; } DispPush;

bool vg_displace_init(VgDisplace *d, const VgContext *ctx, VgTextures *t) {
    memset(d, 0, sizeof(*d));
    d->ctx = ctx;
    d->tex = t;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16_SFLOAT;
    ii.extent.width = ii.extent.height = VG_DISP_SIZE;
    ii.extent.depth = 1;
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(ctx->vma, &ii, &ai, &d->image, &d->alloc, NULL) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = d->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(ctx->device, &vi, NULL, &d->view) != VK_SUCCESS) return false;
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(ctx, &pool);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = d->image;
    b.subresourceRange = vi.subresourceRange;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkClearColorValue zero = { { 0, 0, 0, 0 } };
    vkCmdClearColorImage(cmd, d->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &vi.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    vgc_submit_wait(ctx, pool, cmd);
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                           { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = lb;
    if (vkCreateDescriptorSetLayout(ctx->device, &li, NULL, &d->layout) != VK_SUCCESS) return false;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DispPush) };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &d->layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(ctx->device, &pi, NULL, &d->pl) != VK_SUCCESS) return false;
    d->pipe = vgc_compute_pipeline(ctx, d->pl, "vgeo/vg_displace.comp");
    if (!d->pipe) return false;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VG_MAX_FRAMES }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VG_MAX_FRAMES } };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = VG_MAX_FRAMES;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    if (vkCreateDescriptorPool(ctx->device, &dp, NULL, &d->pool) != VK_SUCCESS) return false;
    for (u32 f = 0; f < ctx->frames; f++) {
        d->pushes[f] = vgc_buffer(ctx, sizeof(float) * 4 * VG_DISP_PUSHES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VGM_UPLOAD);
        VkDescriptorSetAllocateInfo sa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        sa.descriptorPool = d->pool;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &d->layout;
        if (vkAllocateDescriptorSets(ctx->device, &sa, &d->set[f]) != VK_SUCCESS) return false;
        VkDescriptorImageInfo im = { VK_NULL_HANDLE, d->view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo bi = { d->pushes[f].buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[2];
        memset(w, 0, sizeof(w));
        for (int k = 0; k < 2; k++) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = d->set[f];
            w[k].dstBinding = (u32)k;
            w[k].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[0].pImageInfo = &im;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx->device, 2, w, 0, NULL);
    }
    d->tex_index = vg_tex_alloc(t);
    if (!d->tex_index) return false;
    vg_tex_set(t, d->tex_index, d->view, t->repeat, VK_IMAGE_LAYOUT_GENERAL);
    d->ok = true;
    return true;
}

void vg_displace_shutdown(VgDisplace *d) {
    if (!d->ctx) return;
    VkDevice dev = d->ctx->device;
    vkDeviceWaitIdle(dev);
    if (d->tex_index) vg_tex_release(d->tex, d->tex_index);
    for (u32 f = 0; f < VG_MAX_FRAMES; f++) vgc_buffer_free(d->ctx, &d->pushes[f]);
    if (d->pipe) vkDestroyPipeline(dev, d->pipe, NULL);
    if (d->pl) vkDestroyPipelineLayout(dev, d->pl, NULL);
    if (d->layout) vkDestroyDescriptorSetLayout(dev, d->layout, NULL);
    if (d->pool) vkDestroyDescriptorPool(dev, d->pool, NULL);
    if (d->view) vkDestroyImageView(dev, d->view, NULL);
    if (d->image) vmaDestroyImage(d->ctx->vma, d->image, d->alloc);
    memset(d, 0, sizeof(*d));
}

void vg_displace_push(VgDisplace *d, const float pos[3], float radius, float strength) {
    if (d->queued >= VG_DISP_PUSHES || radius <= 0) return;
    float *q = d->queue[d->queued++];
    q[0] = pos[0];
    q[1] = pos[2];
    q[2] = radius;
    q[3] = strength;
}

void vg_displace_record(VgDisplace *d, VkCommandBuffer cmd, u32 frame, const float cam[3], float dt) {
    if (!d->ok || frame >= d->ctx->frames) return;
    DispPush pc;
    memset(&pc, 0, sizeof(pc));
    pc.pox = d->ox;
    pc.poz = d->oz;
    d->ox = (i32)floorf(cam[0] / VG_DISP_TEXEL) - VG_DISP_SIZE / 2;
    d->oz = (i32)floorf(cam[2] / VG_DISP_TEXEL) - VG_DISP_SIZE / 2;
    pc.ox = d->ox;
    pc.oz = d->oz;
    pc.dt = fmaxf(dt, 0.0f);
    pc.decay = expf(-pc.dt / 1.5f);   // pushed grass springs back over ~1.5 s
    pc.texel = VG_DISP_TEXEL;
    pc.npush = d->queued;
    pc.valid = d->valid ? 1u : 0u;
    memcpy(d->pushes[frame].mapped, d->queue, sizeof(float) * 4 * d->queued);
    vmaFlushAllocation(d->ctx->vma, d->pushes[frame].alloc, 0, VK_WHOLE_SIZE);
    d->queued = 0;
    // after every earlier read (WPO sampling of the frames before), then to every reader
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pl, 0, 1, &d->set[frame], 0, NULL);
    vkCmdPushConstants(cmd, d->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, VG_DISP_SIZE / 8, VG_DISP_SIZE / 8, 1);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
    d->valid = true;
}

void vg_displace_view(const VgDisplace *d, VgViewGpu *v) {
    if (!d->ok || !d->valid) return;
    v->disp[0] = (float)d->ox * VG_DISP_TEXEL;
    v->disp[1] = (float)d->oz * VG_DISP_TEXEL;
    v->disp[2] = VG_DISP_TEXEL;
    v->disp[3] = 1.0f;
    v->disp_tex = d->tex_index;
}
