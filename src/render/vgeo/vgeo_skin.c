#include "vgeo_skin.h"
#include "core/log.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PAL_SLOT_BYTES ((VkDeviceSize)VGEO_MAX_BONES * 64)

static const VkBufferUsageFlags SSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

bool vg_palettes_init(VgPalettes *p, const VgContext *ctx, u32 max_slots) {
    memset(p, 0, sizeof(*p));
    p->ctx = ctx;
    p->max_slots = max_slots ? max_slots : 1;
    if (ctx->frames > VG_MAX_FRAMES) return false;
    VkDeviceSize nb = PAL_SLOT_BYTES * p->max_slots, ns = sizeof(VgPaletteSlot) * (VkDeviceSize)p->max_slots;
    p->bones = vgc_buffer(ctx, nb * 2, SSBO, VGM_DEVICE);
    p->slots = vgc_buffer(ctx, ns, SSBO, VGM_DEVICE);
    for (u32 f = 0; f < ctx->frames; f++) p->staging[f] = vgc_buffer(ctx, nb + ns, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_UPLOAD);
    p->cpu_bones = (float *)calloc((size_t)p->max_slots * 2 * VGEO_MAX_BONES * 16, sizeof(float));
    p->cpu_slots = (VgPaletteSlot *)calloc(p->max_slots, sizeof(VgPaletteSlot));
    p->half = (u8 *)calloc(p->max_slots, 1);
    p->set = (u8 *)calloc(p->max_slots, 1);
    p->bone_bounds = calloc(p->max_slots, sizeof(*p->bone_bounds));
    p->has_bounds = (u8 *)calloc(p->max_slots, 1);
    if (!p->bones.buffer || !p->slots.buffer || !p->cpu_bones) { log_error("vgeo: palette buffers failed"); return false; }
    for (u32 f = 0; f < ctx->frames; f++)
        if (!p->staging[f].buffer) return false;
    vgc_upload(ctx, &p->slots, 0, p->cpu_slots, ns);   // every slot empty
    p->ok = true;
    return true;
}

void vg_palettes_shutdown(VgPalettes *p) {
    if (!p->ctx) return;
    if (p->ctx->device) vkDeviceWaitIdle(p->ctx->device);
    vgc_buffer_free(p->ctx, &p->bones);
    vgc_buffer_free(p->ctx, &p->slots);
    for (u32 f = 0; f < VG_MAX_FRAMES; f++) vgc_buffer_free(p->ctx, &p->staging[f]);
    free(p->cpu_bones);
    free(p->cpu_slots);
    free(p->half);
    free(p->set);
    free(p->bone_bounds);
    free(p->has_bounds);
    memset(p, 0, sizeof(*p));
}

void vg_palettes_attach(VgPalettes *p, VgScene *s) {
    s->palettes = p;
    s->generation++;   // cullers rewrite their descriptor sets (bindings 20 / 21)
}

void vg_palettes_begin(VgPalettes *p, u32 frame) { p->frame = frame < p->ctx->frames ? frame : 0; }

void vg_palettes_set(VgPalettes *p, u32 slot, const float *palette, u32 bone_count) {
    if (!p->ok || slot >= p->max_slots) return;
    if (bone_count > VGEO_MAX_BONES) bone_count = VGEO_MAX_BONES;
    if (!bone_count) { vg_palettes_clear(p, slot); return; }
    VgPaletteSlot *s = &p->cpu_slots[slot];
    bool live = s->bone_count != 0;
    u32 h = live ? p->half[slot] ^ 1u : 0u;
    if (p->set[slot]) h = p->half[slot];   // set twice in a frame: overwrite
    u32 first = (slot * 2 + h) * VGEO_MAX_BONES;
    memcpy(&p->cpu_bones[(size_t)first * 16], palette, sizeof(float) * 16 * bone_count);
    memcpy((u8 *)p->staging[p->frame].mapped + PAL_SLOT_BYTES * slot, palette, sizeof(float) * 16 * bone_count);
    if (!p->set[slot]) s->prev_first = live ? s->first : first;
    s->first = first;
    s->bone_count = bone_count;
    vgeo_palette_bounds(palette, bone_count, p->has_bounds[slot] ? (const float (*)[4])p->bone_bounds[slot] : NULL, &s->max_scale, &s->k0, &s->k1);
    p->half[slot] = (u8)h;
    p->set[slot] = 1;
}

void vg_palettes_clear(VgPalettes *p, u32 slot) {
    if (!p->ok || slot >= p->max_slots) return;
    memset(&p->cpu_slots[slot], 0, sizeof(VgPaletteSlot));
    p->set[slot] = 0;
}

void vg_palettes_set_bone_bounds(VgPalettes *p, u32 slot, const float (*bounds)[4]) {
    if (!p->ok || slot >= p->max_slots) return;
    p->has_bounds[slot] = bounds != NULL;
    if (bounds) memcpy(p->bone_bounds[slot], bounds, sizeof(p->bone_bounds[slot]));
}

void vgeo_bone_bounds_merge(float dst[VGEO_MAX_BONES][4], const float src[VGEO_MAX_BONES][4]) {
    for (u32 b = 0; b < VGEO_MAX_BONES; b++) {
        if (src[b][3] < 0) continue;
        if (dst[b][3] < 0) { memcpy(dst[b], src[b], 16); continue; }
        float d[3] = { src[b][0] - dst[b][0], src[b][1] - dst[b][1], src[b][2] - dst[b][2] };
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l + src[b][3] <= dst[b][3]) continue;
        if (l + dst[b][3] <= src[b][3]) { memcpy(dst[b], src[b], 16); continue; }
        float r = (l + dst[b][3] + src[b][3]) * 0.5f, k = (r - dst[b][3]) / l;
        for (int i = 0; i < 3; i++) dst[b][i] += d[i] * k;
        dst[b][3] = r * (1.0f + 1e-6f) + 1e-7f;
    }
}

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = src;
    mb.srcAccessMask = sa;
    mb.dstStageMask = dst;
    mb.dstAccessMask = da;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void vg_palettes_record(VgPalettes *p, VkCommandBuffer cmd) {
    if (!p->ok) return;
    VgBuffer *st = &p->staging[p->frame];
    VkDeviceSize table = PAL_SLOT_BYTES * p->max_slots;
    VkBufferCopy *rg = (VkBufferCopy *)malloc(sizeof(VkBufferCopy) * (p->max_slots + 1));
    u32 n = 0;
    for (u32 s = 0; s < p->max_slots; s++) {
        VgPaletteSlot *sl = &p->cpu_slots[s];
        if (!p->set[s]) {
            sl->prev_first = sl->first;   // not animated this frame: no motion
            continue;
        }
        rg[n].srcOffset = PAL_SLOT_BYTES * s;
        rg[n].dstOffset = (VkDeviceSize)sl->first * 64;
        rg[n].size = (VkDeviceSize)sl->bone_count * 64;
        n++;
        p->set[s] = 0;
    }
    memcpy((u8 *)st->mapped + table, p->cpu_slots, sizeof(VgPaletteSlot) * p->max_slots);
    vmaFlushAllocation(p->ctx->vma, st->alloc, 0, VK_WHOLE_SIZE);
    // The previous frame may still read the regions / table being overwritten.
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (n) vkCmdCopyBuffer(cmd, st->buffer, p->bones.buffer, n, rg);
    VkBufferCopy tc = { table, 0, sizeof(VgPaletteSlot) * (VkDeviceSize)p->max_slots };
    vkCmdCopyBuffer(cmd, st->buffer, p->slots.buffer, 1, &tc);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    free(rg);
}

// ------------------------------------------------------------------ CPU mirror

// sqrt(|A^T A|_inf) >= the spectral norm of the 3x3 A (exact for a rotation times a uniform scale).
static float norm_bound(const float a[9]) {
    float g[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) g[i * 3 + j] = a[i * 3] * a[j * 3] + a[i * 3 + 1] * a[j * 3 + 1] + a[i * 3 + 2] * a[j * 3 + 2];   // columns i . j
    float m = 0;
    for (int i = 0; i < 3; i++) m = fmaxf(m, fabsf(g[i * 3]) + fabsf(g[i * 3 + 1]) + fabsf(g[i * 3 + 2]));
    return sqrtf(m);
}

void vgeo_palette_bounds(const float *pal, u32 n, const float (*bb)[4], float *max_scale, float *k0, float *k1) {
    float s = 0, t = 0, d = 0, kb = 0;
    const float *r = pal;
    for (u32 b = 0; b < n; b++) {
        const float *m = &pal[b * 16];
        float a[9], e[9];
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) {
                a[c * 3 + k] = m[c * 4 + k];
                e[c * 3 + k] = m[c * 4 + k] - r[c * 4 + k];
            }
        s = fmaxf(s, norm_bound(a));
        float de = norm_bound(e);
        d = fmaxf(d, de);
        float dx = m[12] - r[12], dy = m[13] - r[13], dz = m[14] - r[14];
        t = fmaxf(t, sqrtf(dx * dx + dy * dy + dz * dz));
        if (bb && bb[b][3] >= 0) {   // |(P_b - P_0) v| <= |(P_b - P_0) c_b| + |A_b - A_0| r_b for v in the bone's sphere
            float q[3];
            for (int k = 0; k < 3; k++) q[k] = e[k] * bb[b][0] + e[3 + k] * bb[b][1] + e[6 + k] * bb[b][2] + (m[12 + k] - r[12 + k]);
            kb = fmaxf(kb, sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) + de * bb[b][3]);
        }
    }
    // float slack: the bounds hold after the shaders' f32 rounding
    *max_scale = s * (1.0f + 1e-5f);
    if (bb) {
        *k0 = kb * (1.0f + 1e-5f) + 1e-5f;
        *k1 = 0.0f;
    } else {
        *k0 = t * (1.0f + 1e-5f) + 1e-5f;
        *k1 = d * (1.0f + 1e-5f) + 1e-6f;
    }
}

void vgeo_skin_matrix(const float *pal, const u8 j[4], const u8 w[4], float out[16]) {
    float f[4];
    for (int k = 0; k < 4; k++) f[k] = (float)w[k] / 255.0f;
    const float *a = &pal[j[0] * 16], *b = &pal[j[1] * 16], *c = &pal[j[2] * 16], *d = &pal[j[3] * 16];
    for (int i = 0; i < 16; i++) out[i] = a[i] * f[0] + b[i] * f[1] + c[i] * f[2] + d[i] * f[3];
}

void vgeo_mat4_point(const float m[16], const float p[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
}

void vgeo_mat4_dir(const float m[16], const float d[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = m[k] * d[0] + m[4 + k] * d[1] + m[8 + k] * d[2];
}

static void qrot(const float q[4], const float v[3], float o[3]) {
    float t[3] = { 2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

void vgeo_xf_decode(VgXfCpu *x, const VgInstance *in, const VgMesh *m, const VgPalettes *p) {
    memset(x, 0, sizeof(*x));
    vgeo_instance_decode(in, x->pos, x->rot, &x->pscale);
    x->scale = x->pscale;
    x->active = true;
    x->wpo = vg_wpo_margin(m, x->pscale);
    if (!((in->flags_phase >> 16) & VGI_SKINNED) || !(m->flags & VGF_SKINNED)) return;
    x->skinned = true;
    const VgPaletteSlot *s = p && in->extra < p->max_slots ? &p->cpu_slots[in->extra] : NULL;
    x->active = s && s->bone_count;
    if (!x->active) return;
    x->first = s->first;
    x->prev_first = s->prev_first;
    memcpy(x->root, &p->cpu_bones[(size_t)s->first * 16], sizeof(x->root));
    x->skin_s = s->max_scale;
    x->skin_k = s->k0 + s->k1 * (sqrtf(m->center[0] * m->center[0] + m->center[1] * m->center[1] + m->center[2] * m->center[2]) + m->radius);
    x->scale = x->pscale * s->max_scale;
}

void vgeo_xf_instance_sphere(const VgXfCpu *x, const float c[3], float r, float out[4]) {
    float v[3] = { c[0] * x->pscale, c[1] * x->pscale, c[2] * x->pscale }, w[3];
    qrot(x->rot, v, w);
    for (int k = 0; k < 3; k++) out[k] = x->pos[k] + w[k];
    out[3] = r * x->pscale;
}

void vgeo_xf_sphere(const VgXfCpu *x, const float c[3], float r, float out[4]) {
    if (!x->skinned) vgeo_xf_instance_sphere(x, c, r, out);
    else {
        float cc[3];
        vgeo_mat4_point(x->root, c, cc);
        vgeo_xf_instance_sphere(x, cc, r * x->skin_s + x->skin_k, out);
    }
    out[3] += x->wpo;
}

void vgeo_skin_cluster_sphere(const float *pal, const VgBoneBox *bx, u32 n, float out[4]) {
    float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (u32 i = 0; i < n; i++) {
        const float *m = &pal[bx[i].bone * 16];
        float c[3], e[3], cc[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (bx[i].min[k] + bx[i].max[k]) * 0.5f;
            e[k] = (bx[i].max[k] - bx[i].min[k]) * 0.5f;
        }
        vgeo_mat4_point(m, c, cc);
        for (int k = 0; k < 3; k++) {
            float ee = fabsf(m[k]) * e[0] + fabsf(m[4 + k]) * e[1] + fabsf(m[8 + k]) * e[2];
            mn[k] = fminf(mn[k], cc[k] - ee);
            mx[k] = fmaxf(mx[k], cc[k] + ee);
        }
    }
    float h[3], c2 = 0, h2 = 0;
    for (int k = 0; k < 3; k++) {
        out[k] = (mn[k] + mx[k]) * 0.5f;
        h[k] = (mx[k] - mn[k]) * 0.5f;
        c2 += out[k] * out[k];
        h2 += h[k] * h[k];
    }
    float hl = sqrtf(h2);
    out[3] = hl + 1e-5f * (sqrtf(c2) + hl) + 1e-6f;   // f32 slack (box centre / extent, the shaders' skinning)
}
