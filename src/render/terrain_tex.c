// Terrain shading resources (docs/TERRAIN_SHADING.md, shaders/terrain/): frame set bindings 20..28.
//   20 terrain_alb     2D array, sRGB albedo + the baked AO in alpha (assets/terrain/tex, "surf:" names from
//                      assets/textures/surfaces), r_terrain_tex_size px (1024; 512 on the lowest texture level)
//   21 terrain_nrh     2D array, normal xy + roughness + height (half the albedo size)
//   22 terrain_macro   2D array, 512 px macro colour multiplier (0.5 = x1) + macro height, per texture
//   23 terrain_blend   blend map (RGBA8: dominant / second material + fractions; texelFetch)
//   24 terrain_attr    attribute map (curvature, moisture, wear, border distance; linear)
//   25 terrain_paint   painted map (tint rgb around 0.5, variant mask; linear; the terrain tool)
//   26 / 27            scatter atlas albedo (sRGB + alpha) / nrh
//   28 TerrainGpu      parameters (std430), one buffer per frame in flight
// The maps share the material raster's grid (frame.surf_map). Until a map loads, 1-texel placeholders.
#include "vk_internal.h"
#include "tex_compress.h"
#include "core/platform.h"
#include "core/cvar.h"
#include "core/jobs.h"
#include "stb_image.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TT_MAX_TEX 48
#define MACRO_SIZE 512

typedef struct TtImage { VkImage image; VmaAllocation alloc; VkImageView view; } TtImage;

static struct {
    TtImage alb, nrh, macro, blend, attr, paint, sc_alb, sc_nrh;
    GpuBuffer params[FRAMES_IN_FLIGHT];
    size_t params_size;
    u8 *params_cpu;
    u32 params_gen, params_written[FRAMES_IN_FLIGHT];
    bool loaded, maps, dirty;
    int map_n;
    u8 *attr_cpu, *paint_cpu;       // CPU copies (terrain tool edits, re-uploaded)
    u32 tex_size;
    Cvar *r_terrain, *r_terrain_tex_size;
} T;

static TtImage tt_image(u32 w, u32 h, u32 mips, u32 layers, VkFormat fmt, bool array) {
    TtImage im;
    memset(&im, 0, sizeof(im));
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = (VkExtent3D){ w, h, 1 };
    ici.mipLevels = mips;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(R->vma, &ici, &aci, &im.image, &im.alloc, NULL));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = im.image;
    vci.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = mips;
    vci.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &im.view));
    return im;
}

static void tt_destroy(TtImage *im) {
    if (im->image) vkr_cancel_image_upload(im->image);
    if (im->view) vkDestroyImageView(R->device, im->view, NULL);
    if (im->image) vmaDestroyImage(R->vma, im->image, im->alloc);
    memset(im, 0, sizeof(*im));
}

static u32 tt_mips(u32 s) {
    u32 m = 1;
    while (s > 1) s >>= 1, m++;
    return m;
}

// Square RGBA8 layers with a box-filtered mip chain (mip by mip, layers contiguous: vkr_upload_image),
// block-compressed level by level (BC7, docs/TEXTURE_MIPS.md) while r_texture_compress is on.
static TtImage tt_upload_layers(u8 *const *layers, u32 count, u32 size, VkFormat fmt, bool mips_on) {
    u32 mips = mips_on ? tt_mips(size) : 1;
    size_t offs[16], total = 0;
    for (u32 m = 0; m < mips; m++) {
        u32 s = DG_MAX(size >> m, 1u);
        offs[m] = total;
        total += (size_t)s * s * 4 * count;
    }
    u8 *data = (u8 *)malloc(total);
    for (u32 l = 0; l < count; l++) memcpy(data + (size_t)size * size * 4 * l, layers[l], (size_t)size * size * 4);
    for (u32 m = 1; m < mips; m++) {
        u32 ps = DG_MAX(size >> (m - 1), 1u), s = DG_MAX(size >> m, 1u);
        for (u32 l = 0; l < count; l++) {
            const u8 *src = data + offs[m - 1] + (size_t)ps * ps * 4 * l;
            u8 *dst = data + offs[m] + (size_t)s * s * 4 * l;
            for (u32 y = 0; y < s; y++)
                for (u32 x = 0; x < s; x++)
                    for (int c = 0; c < 4; c++) {
                        u32 x0 = DG_MIN(x * 2, ps - 1), x1 = DG_MIN(x * 2 + 1, ps - 1), y0 = DG_MIN(y * 2, ps - 1), y1 = DG_MIN(y * 2 + 1, ps - 1);
                        u32 v = src[((size_t)y0 * ps + x0) * 4 + c] + src[((size_t)y0 * ps + x1) * 4 + c] + src[((size_t)y1 * ps + x0) * 4 + c] + src[((size_t)y1 * ps + x1) * 4 + c];
                        dst[((size_t)y * s + x) * 4 + c] = (u8)((v + 2) / 4);
                    }
        }
    }
    size_t bc_offs[16], bc_total = 0;
    TexcKind k = fmt == VK_FORMAT_R8G8B8A8_SRGB ? TEXC_COLOR : TEXC_DATA;
    u8 *bc = mips_on ? texc_encode_array(data, offs, mips, count, size, k, bc_offs, &bc_total) : NULL;
    TtImage im = tt_image(size, size, mips, count, bc ? (VkFormat)texc_vk_format(k) : fmt, true);
    if (bc) vkr_upload_image(im.image, size, size, mips, count, bc, bc_offs, bc_total);
    else vkr_upload_image(im.image, size, size, mips, count, data, offs, total);
    free(bc);
    free(data);
    return im;
}

static TtImage tt_upload_2d(const u8 *px, u32 w, u32 h, VkFormat fmt) {
    size_t off = 0;
    TtImage im = tt_image(w, h, 1, 1, fmt, false);
    vkr_upload_image(im.image, w, h, 1, 1, px, &off, (size_t)w * h * 4);
    return im;
}

// A sampled content map without mips (the scatter atlas), block-compressed like the layers.
static TtImage tt_upload_2d_content(const u8 *px, u32 w, u32 h, VkFormat fmt) {
    TexcImage ti = { px, w, h };
    TexcKind k = fmt == VK_FORMAT_R8G8B8A8_SRGB ? TEXC_COLOR : TEXC_DATA;
    size_t off = 0, total = 0;
    u8 *bc = texc_encode(&ti, 1, k, &off, &total);
    if (!bc) return tt_upload_2d(px, w, h, fmt);
    TtImage im = tt_image(w, h, 1, 1, (VkFormat)texc_vk_format(k), false);
    vkr_upload_image(im.image, w, h, 1, 1, bc, &off, total);
    free(bc);
    return im;
}

// A file resampled to size x size (box when it is an integer multiple, else nearest); fill when missing.
static u8 *tt_load(const char *rel, u32 size, int comp, const u8 fill[4], bool *ok) {
    char path[512];
    int w, h, c;
    u8 *px = stbi_load(platform_data_path(rel, path, sizeof(path)), &w, &h, &c, comp);
    u8 *out = (u8 *)malloc((size_t)size * size * 4);
    if (!px) {
        for (size_t i = 0; i < (size_t)size * size; i++) memcpy(out + i * 4, fill, 4);
        *ok = false;
        return out;
    }
    u32 k = ((u32)w % size == 0 && (u32)h % size == 0 && w == h) ? (u32)w / size : 0;
    for (u32 y = 0; y < size; y++)
        for (u32 x = 0; x < size; x++)
            for (int ch = 0; ch < 4; ch++) {
                u32 v;
                if (k >= 1) {
                    u32 acc = 0;
                    for (u32 dy = 0; dy < k; dy++)
                        for (u32 dx = 0; dx < k; dx++) acc += ch < comp ? px[(((size_t)(y * k + dy)) * (u32)w + x * k + dx) * comp + ch] : 255u;
                    v = (acc + k * k / 2) / (k * k);
                } else {
                    u32 sx = (u32)((u64)x * (u32)w / size), sy = (u32)((u64)y * (u32)h / size);
                    v = ch < comp ? px[((size_t)sy * (u32)w + sx) * comp + ch] : 255u;
                }
                out[((size_t)y * size + x) * 4 + ch] = (u8)v;
            }
    stbi_image_free(px);
    *ok = true;
    return out;
}

typedef struct TtJob {
    char names[TT_MAX_TEX][48];
    int n;
    u32 size;
    u8 *alb[TT_MAX_TEX], *nrh[TT_MAX_TEX], *mac[TT_MAX_TEX];
    SDL_AtomicInt missing;
} TtJob;

static void tt_load_job(void *ctx, int begin, int end) {
    TtJob *j = (TtJob *)ctx;
    static const u8 grey[4] = { 150, 140, 110, 255 }, flat[4] = { 128, 128, 230, 128 }, neutral[4] = { 128, 128, 128, 128 };
    for (int i = begin; i < end; i++) {
        const char *name = j->names[i];
        bool surf = !strncmp(name, "surf:", 5);
        const char *dir = surf ? "assets/textures/surfaces" : "assets/terrain/tex";
        const char *base = surf ? name + 5 : name;
        char rel[256];
        bool ok1, ok2, ok3, ok4;
        snprintf(rel, sizeof(rel), "%s/%s_albedo.jpg", dir, base);
        j->alb[i] = tt_load(rel, j->size, 3, grey, &ok1);
        snprintf(rel, sizeof(rel), "%s/%s_ao.png", dir, base);
        u8 *ao = tt_load(rel, j->size, 1, neutral, &ok3);
        if (ok3)
            for (size_t p = 0; p < (size_t)j->size * j->size; p++) j->alb[i][p * 4 + 3] = ao[p * 4];
        else
            for (size_t p = 0; p < (size_t)j->size * j->size; p++) j->alb[i][p * 4 + 3] = 255;
        free(ao);
        snprintf(rel, sizeof(rel), "%s/%s_nrh.png", dir, base);
        j->nrh[i] = tt_load(rel, j->size / 2, 4, flat, &ok2);
        snprintf(rel, sizeof(rel), "%s/%s_macro.png", dir, base);
        j->mac[i] = tt_load(rel, MACRO_SIZE, 4, neutral, &ok4);
        if (!ok1 || !ok2) SDL_AtomicIncRef(&j->missing);
    }
}

static void tt_placeholders(void) {
    static const u8 white[4] = { 255, 255, 255, 255 }, flat[4] = { 128, 128, 255, 128 }, neutral[4] = { 128, 128, 128, 128 }, zero[4] = { 0, 0, 0, 0 };
    u8 *a = (u8 *)white, *b = (u8 *)flat, *c = (u8 *)neutral;
    if (!T.alb.image) T.alb = tt_upload_layers(&a, 1, 1, VK_FORMAT_R8G8B8A8_SRGB, false);
    if (!T.nrh.image) T.nrh = tt_upload_layers(&b, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    if (!T.macro.image) T.macro = tt_upload_layers(&c, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    if (!T.blend.image) T.blend = tt_upload_2d(zero, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    if (!T.attr.image) T.attr = tt_upload_2d(neutral, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    if (!T.paint.image) T.paint = tt_upload_2d(neutral, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    if (!T.sc_alb.image) T.sc_alb = tt_upload_2d(zero, 1, 1, VK_FORMAT_R8G8B8A8_SRGB);
    if (!T.sc_nrh.image) T.sc_nrh = tt_upload_2d(flat, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
}

bool render_terrain_load(const char (*tex)[48], int count, const char *scatter_atlas) {
    if (!R || !R->device || count <= 0) return false;
    u64 t0 = SDL_GetTicksNS();
    Cvar *tq = cvar_find("r_texture_quality");
    u32 size = (u32)DG_CLAMP(T.r_terrain_tex_size->i, 256, 2048);
    if (tq && tq->i <= 0) size = DG_MIN(size, 512u);   // the lowest texture level: half
    TtJob *j = (TtJob *)calloc(1, sizeof(TtJob));
    j->n = DG_MIN(count, TT_MAX_TEX);
    j->size = size;
    for (int i = 0; i < j->n; i++) dg_strcpy(j->names[i], 48, tex[i]);
    jobs_parallel_for(j->n, 1, tt_load_job, j);
    u8 *sa = NULL, *sn = NULL;
    int sw = 0, sh = 0, snw = 0, snh = 0, comp;
    if (scatter_atlas && scatter_atlas[0]) {
        char rel[256], path[512];
        snprintf(rel, sizeof(rel), "assets/terrain/tex/%s_albedo.png", scatter_atlas);
        sa = stbi_load(platform_data_path(rel, path, sizeof(path)), &sw, &sh, &comp, 4);
        snprintf(rel, sizeof(rel), "assets/terrain/tex/%s_nrh.png", scatter_atlas);
        sn = stbi_load(platform_data_path(rel, path, sizeof(path)), &snw, &snh, &comp, 4);
    }
    vkDeviceWaitIdle(R->device);   // once per load / reload: the old arrays leave the frame sets
    tt_destroy(&T.alb);
    tt_destroy(&T.nrh);
    tt_destroy(&T.macro);
    T.alb = tt_upload_layers(j->alb, (u32)j->n, size, VK_FORMAT_R8G8B8A8_SRGB, true);
    T.nrh = tt_upload_layers(j->nrh, (u32)j->n, size / 2, VK_FORMAT_R8G8B8A8_UNORM, true);
    T.macro = tt_upload_layers(j->mac, (u32)j->n, MACRO_SIZE, VK_FORMAT_R8G8B8A8_UNORM, true);
    if (sa && sn && sw == snw && sh == snh) {
        tt_destroy(&T.sc_alb);
        tt_destroy(&T.sc_nrh);
        T.sc_alb = tt_upload_2d_content(sa, (u32)sw, (u32)sh, VK_FORMAT_R8G8B8A8_SRGB);
        T.sc_nrh = tt_upload_2d_content(sn, (u32)snw, (u32)snh, VK_FORMAT_R8G8B8A8_UNORM);
    }
    if (sa) stbi_image_free(sa);
    if (sn) stbi_image_free(sn);
    for (int i = 0; i < j->n; i++) {
        free(j->alb[i]);
        free(j->nrh[i]);
        free(j->mac[i]);
    }
    int missing = SDL_GetAtomicInt(&j->missing);
    T.loaded = missing < j->n;
    T.tex_size = size;
    T.dirty = true;
    double mb = (double)j->n * ((double)size * size * 4 * 1.333 + (double)size * size * 1.333 + (double)MACRO_SIZE * MACRO_SIZE * 4 * 1.333) / (1024.0 * 1024.0);
    bool bc = texc_enabled();
    log_info("terrain: %d textures (%d missing) at %u px, %.0f MB%s, in %.0f ms", j->n, missing, size, bc ? mb / 4.0 : mb, bc ? " (BC7)" : "",
             (double)(SDL_GetTicksNS() - t0) / 1e6);
    free(j);
    return T.loaded;
}

void render_terrain_maps(const u8 *blend, const u8 *attr, int n, float origin_x, float origin_z, float spacing) {
    (void)origin_x, (void)origin_z, (void)spacing;   // the grid is the material map's (frame.surf_map)
    if (!R || !R->device) return;
    vkDeviceWaitIdle(R->device);
    tt_destroy(&T.blend);
    tt_destroy(&T.attr);
    tt_destroy(&T.paint);
    free(T.attr_cpu);
    free(T.paint_cpu);
    T.attr_cpu = T.paint_cpu = NULL;
    T.maps = false;
    if (blend && attr && n > 0) {
        T.map_n = n;
        size_t sz = (size_t)n * n * 4;
        T.attr_cpu = (u8 *)malloc(sz);
        memcpy(T.attr_cpu, attr, sz);
        T.paint_cpu = (u8 *)malloc(sz);
        for (size_t i = 0; i < (size_t)n * n; i++) T.paint_cpu[i * 4] = T.paint_cpu[i * 4 + 1] = T.paint_cpu[i * 4 + 2] = 128, T.paint_cpu[i * 4 + 3] = 0;
        T.blend = tt_upload_2d(blend, (u32)n, (u32)n, VK_FORMAT_R8G8B8A8_UNORM);
        T.attr = tt_upload_2d(T.attr_cpu, (u32)n, (u32)n, VK_FORMAT_R8G8B8A8_UNORM);
        T.paint = tt_upload_2d(T.paint_cpu, (u32)n, (u32)n, VK_FORMAT_R8G8B8A8_UNORM);
        T.maps = true;
    }
    tt_placeholders();
    T.dirty = true;
}

// Terrain tool: paint channel ch (0..2 tint r / g / b around 0.5, 3 variant mask, 4 wear, 5 moisture) toward
// value with a soft brush (radius m, strength 0..1) at world (x, z); grid = the material map's.
void render_terrain_paint(float x, float z, float radius, int ch, float value, float strength, const float map_xf[4]) {
    if (!T.maps || !T.paint_cpu || map_xf[3] < 1) return;
    int n = T.map_n;
    float gx = (x - map_xf[0]) * map_xf[2], gz = (z - map_xf[1]) * map_xf[2], rr = radius * map_xf[2];
    u8 target = (u8)lrintf(DG_CLAMP(value, 0.0f, 1.0f) * 255.0f);
    for (int y = (int)floorf(gz - rr); y <= (int)ceilf(gz + rr); y++)
        for (int xx = (int)floorf(gx - rr); xx <= (int)ceilf(gx + rr); xx++) {
            if (xx < 0 || y < 0 || xx >= n || y >= n) continue;
            float d = sqrtf((xx - gx) * (xx - gx) + (y - gz) * (y - gz)) / fmaxf(rr, 1e-3f);
            if (d >= 1) continue;
            float k = strength * (1 - d * d) * (1 - d * d);
            u8 *px = ch <= 3 ? &T.paint_cpu[((size_t)y * n + xx) * 4 + ch] : &T.attr_cpu[((size_t)y * n + xx) * 4 + (ch == 4 ? 2 : 1)];
            *px = (u8)lrintf((float)*px + ((float)target - (float)*px) * k);
        }
    vkDeviceWaitIdle(R->device);
    tt_destroy(&T.paint);
    tt_destroy(&T.attr);
    T.paint = tt_upload_2d(T.paint_cpu, (u32)n, (u32)n, VK_FORMAT_R8G8B8A8_UNORM);
    T.attr = tt_upload_2d(T.attr_cpu, (u32)n, (u32)n, VK_FORMAT_R8G8B8A8_UNORM);
    T.dirty = true;
}

// The painted map (RGBA, map_n x map_n) for saving; NULL without a map.
const u8 *render_terrain_paint_data(int *n) {
    *n = T.map_n;
    return T.maps ? T.paint_cpu : NULL;
}

void render_terrain_params(const void *gpu, size_t size) {
    if (!R || !R->device || !gpu || !size) return;
    if (size > T.params_size) {
        vkDeviceWaitIdle(R->device);
        for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
            if (T.params[i].buffer) vkr_buffer_destroy_now(&T.params[i]);
            T.params[i] = vkr_buffer_create(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        }
        T.params_cpu = (u8 *)realloc(T.params_cpu, size);
        T.params_size = size;
        T.dirty = true;
    }
    memcpy(T.params_cpu, gpu, size);
    T.params_gen++;
}

bool render_terrain_active(void) { return T.loaded && T.maps && T.params_size && T.r_terrain && T.r_terrain->i; }

static void tt_write_sets(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int s = 0; s < FRAME_SET_COUNT; s++) {
            if (!sets[s]) continue;
            VkDescriptorImageInfo ii[8] = { { R->sampler_linear, T.alb.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_linear, T.nrh.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_linear, T.macro.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_clamp, T.blend.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_clamp, T.attr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_clamp, T.paint.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_clamp, T.sc_alb.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_clamp, T.sc_nrh.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet w[9];
            for (int k = 0; k < 8; k++) {
                w[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[k].dstSet = sets[s];
                w[k].dstBinding = (u32)(20 + k);
                w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[k].pImageInfo = &ii[k];
            }
            VkDescriptorBufferInfo bi = { T.params[i].buffer, 0, VK_WHOLE_SIZE };
            w[8] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[8].dstSet = sets[s];
            w[8].dstBinding = 28;
            w[8].descriptorCount = 1;
            w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[8].pBufferInfo = &bi;
            vkUpdateDescriptorSets(R->device, 9, w, 0, NULL);
        }
    }
}

void vkt_init(void) {
    memset(&T, 0, sizeof(T));
    T.r_terrain = cvar_reg_int("r_terrain", 1, 0, 1, CVAR_LOCKED, "terrain shading (docs/TERRAIN_SHADING.md; 0: the older surface layer blend)");
    cvar_reg_int("r_terrain_quality", 4, 0, 4, CVAR_TIER, "terrain detail (0 minimal .. 4 ultra; bounded by Engine Fidelity, docs/TERRAIN_SHADING.md)");
    T.r_terrain_tex_size = cvar_reg_int("r_terrain_tex_size", 1024, 256, 2048, CVAR_TIER, "terrain layer texture size (px; 2048 about 4x the memory; at the next map load)");
    tt_placeholders();
    float zero[64] = { 0 };
    render_terrain_params(zero, sizeof(zero));   // a valid buffer before the first map
    T.dirty = true;
}

void vkt_shutdown(void) {
    tt_destroy(&T.alb);
    tt_destroy(&T.nrh);
    tt_destroy(&T.macro);
    tt_destroy(&T.blend);
    tt_destroy(&T.attr);
    tt_destroy(&T.paint);
    tt_destroy(&T.sc_alb);
    tt_destroy(&T.sc_nrh);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++)
        if (T.params[i].buffer) vkr_buffer_destroy_now(&T.params[i]);
    free(T.params_cpu);
    free(T.attr_cpu);
    free(T.paint_cpu);
}

void vkt_write_bindings(void) {
    if (!T.dirty) return;
    T.dirty = false;
    tt_write_sets();
}

void vkt_samplers_changed(void) { tt_write_sets(); }

// This frame slot's parameter buffer gets the latest parameters (its last use has completed).
void vkt_frame_data(FrameData *fd, int slot) {
    if (T.params_size && T.params_written[slot] != T.params_gen && T.params[slot].mapped) {
        memcpy(T.params[slot].mapped, T.params_cpu, T.params_size);
        T.params_written[slot] = T.params_gen;
    }
    fd->surf_info.z = render_terrain_active() ? 1.0f : 0.0f;
}
