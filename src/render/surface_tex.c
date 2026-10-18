// Surface textures (docs/TEXTURES.md): the terrain / building surface layers as two texture arrays
// (albedo sRGB 1024 px + the baked AO in its alpha; normal xy + roughness + height 512 px, then the
// metal layers' masks as STEX_METAL_SLOTS extra layers; both with mips) and the terrain material map
// (R8, one SurfTexLayer id per raster sample) in frame set bindings 12, 13, 14. The lit pipeline
// (shaders/surfaces.glsl) samples them for MAT_TERRAIN / MAT_LAYERED draws.
// Until the layers load (first open-world map) the bindings hold 1-texel placeholders.
#include "vk_internal.h"
#include "tex_compress.h"
#include "core/jobs.h"
#include "core/profiler.h"
#include "core/platform.h"
#include "core/cvar.h"
#include "stb_image.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define ALBEDO_SIZE 1024
#define NRH_SIZE 512
// Layers with a metal mask (<name>_metal.png: R bare metal, G paint wear), appended to the nrh array
// in this order (surfaces.glsl SURF_METAL_SLOT).
static const int STEX_METAL_LAYERS[] = { STEX_METAL, STEX_RUST };
#define STEX_METAL_SLOTS 2

static const char *const STEX_LAYER_NAMES[STEX_LAYER_COUNT] = {
    "grass", "meadow", "forest", "dirt", "rock", "sand", "gravel", "asphalt", "dirt_road", "concrete",
    "field", "crop", "plowed", "mud", "brick", "plaster", "wood", "metal", "rust", "shingles",
};

typedef struct SurfImage { VkImage image; VmaAllocation alloc; VkImageView view; } SurfImage;

static struct {
    SurfImage albedo, nrh, map;
    bool loaded, tried, dirty;
    vec4 map_xf;          // origin x, z, 1 / spacing, n
    Cvar *r_surfaces;
} S;

static SurfImage image_create(u32 w, u32 h, u32 mips, u32 layers, VkFormat fmt, bool array) {
    SurfImage im;
    memset(&im, 0, sizeof(im));
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
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

static void image_destroy(SurfImage *im) {
    if (im->image) vkr_cancel_image_upload(im->image);
    if (im->view) vkDestroyImageView(R->device, im->view, NULL);
    if (im->image) vmaDestroyImage(R->vma, im->image, im->alloc);
    memset(im, 0, sizeof(*im));
}

static u32 mip_levels(u32 s) {
    u32 m = 1;
    while (s > 1) { s >>= 1; m++; }
    return m;
}

// Square RGBA8 layers with a box-filtered mip chain: data laid out mip by mip, the layers of a mip
// contiguous (vkr_upload_image); block-compressed level by level (BC7, docs/TEXTURE_MIPS.md) while
// r_texture_compress is on.
static SurfImage upload_layers(u8 *const *layers, u32 count, u32 size, VkFormat fmt) {
    u32 mips = mip_levels(size);
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
    u8 *bc = texc_encode_array(data, offs, mips, count, size, k, bc_offs, &bc_total);
    SurfImage im = image_create(size, size, mips, count, bc ? (VkFormat)texc_vk_format(k) : fmt, true);
    if (bc) vkr_upload_image(im.image, size, size, mips, count, bc, bc_offs, bc_total);
    else vkr_upload_image(im.image, size, size, mips, count, data, offs, total);
    free(bc);
    free(data);
    return im;
}

// One layer file resampled (nearest power-of-two box / point) to size x size, or a flat fill.
static u8 *load_layer(const char *name, const char *suffix, u32 size, const u8 fill[4], bool *ok) {
    char rel[128], path[512];
    snprintf(rel, sizeof(rel), "assets/textures/surfaces/%s_%s", name, suffix);
    int w, h, comp;
    u8 *px = stbi_load(platform_data_path(rel, path, sizeof(path)), &w, &h, &comp, 4);
    u8 *out = (u8 *)malloc((size_t)size * size * 4);
    if (!px) {
        for (size_t i = 0; i < (size_t)size * size; i++) memcpy(out + i * 4, fill, 4);
        *ok = false;
        return out;
    }
    for (u32 y = 0; y < size; y++)
        for (u32 x = 0; x < size; x++) {
            u32 sx = (u32)((u64)x * (u32)w / size), sy = (u32)((u64)y * (u32)h / size);
            memcpy(out + ((size_t)y * size + x) * 4, px + ((size_t)sy * (u32)w + sx) * 4, 4);
        }
    stbi_image_free(px);
    return out;
}

// The baked AO (<name>_ao.png, 512 px grey, tools/vfxgen/surface_pbr.py) into the albedo's alpha,
// bilinear up to ALBEDO_SIZE (wrapping: the layers tile). Missing: alpha stays 255 (no occlusion).
static bool load_ao_into_alpha(const char *name, u8 *alb) {
    char rel[128], path[512];
    snprintf(rel, sizeof(rel), "assets/textures/surfaces/%s_ao.png", name);
    int w, h, comp;
    u8 *px = stbi_load(platform_data_path(rel, path, sizeof(path)), &w, &h, &comp, 1);
    if (!px) return false;
    for (u32 y = 0; y < ALBEDO_SIZE; y++)
        for (u32 x = 0; x < ALBEDO_SIZE; x++) {
            float fx = ((float)x + 0.5f) * (float)w / ALBEDO_SIZE - 0.5f, fy = ((float)y + 0.5f) * (float)h / ALBEDO_SIZE - 0.5f;
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            float tx = fx - (float)x0, ty = fy - (float)y0;
            int xa = (x0 % w + w) % w, xb = (xa + 1) % w, ya = (y0 % h + h) % h, yb = (ya + 1) % h;
            float a = (float)px[ya * w + xa] * (1 - tx) + (float)px[ya * w + xb] * tx;
            float b = (float)px[yb * w + xa] * (1 - tx) + (float)px[yb * w + xb] * tx;
            alb[((size_t)y * ALBEDO_SIZE + x) * 4 + 3] = (u8)(a * (1 - ty) + b * ty + 0.5f);
        }
    stbi_image_free(px);
    return true;
}

// One layer's files (albedo + baked AO, nrh; the metal slots' masks after the layers): independent of
// every other, decoded on the jobs (PF-0024; the same pixels as the serial loop).
typedef struct SurfLoad { u8 *alb[STEX_LAYER_COUNT], *nrh[STEX_LAYER_COUNT + STEX_METAL_SLOTS]; bool ok[STEX_LAYER_COUNT + STEX_METAL_SLOTS], ao[STEX_LAYER_COUNT]; } SurfLoad;
static void surf_load_job(void *ctx, int begin, int end) {
    static const u8 grey[4] = { 188, 188, 188, 255 }, flat[4] = { 128, 128, 217, 128 }, no_metal[4] = { 0, 0, 0, 0 };
    SurfLoad *j = (SurfLoad *)ctx;
    for (int l = begin; l < end; l++) {
        if (l >= STEX_LAYER_COUNT) {
            int k = l - STEX_LAYER_COUNT;
            bool ok = true;
            j->nrh[l] = load_layer(STEX_LAYER_NAMES[STEX_METAL_LAYERS[k]], "metal.png", NRH_SIZE, no_metal, &ok);
            j->ok[l] = ok;
            continue;
        }
        bool ok1 = true, ok2 = true;
        j->alb[l] = load_layer(STEX_LAYER_NAMES[l], "albedo.jpg", ALBEDO_SIZE, grey, &ok1);
        j->nrh[l] = load_layer(STEX_LAYER_NAMES[l], "nrh.png", NRH_SIZE, flat, &ok2);
        j->ok[l] = ok1 && ok2;
        j->ao[l] = load_ao_into_alpha(STEX_LAYER_NAMES[l], j->alb[l]);
    }
}

bool render_surfaces_load(void) {
    if (!R || !R->device) return false;
    if (S.tried) return S.loaded;
    S.tried = true;
    u64 t0 = SDL_GetTicksNS();
    SurfLoad *j = (SurfLoad *)calloc(1, sizeof(SurfLoad));
    int n = STEX_LAYER_COUNT + STEX_METAL_SLOTS;
    if (prof_fix_enabled(PFX_TEX_DECODE_JOBS)) jobs_parallel_for(n, 1, surf_load_job, j);
    else surf_load_job(j, 0, n);
    u8 **alb = j->alb, **nrh = j->nrh;
    int missing = 0, no_ao = 0;
    for (int l = 0; l < STEX_LAYER_COUNT; l++) {
        if (!j->ok[l]) { missing++; log_warn("surfaces: %s textures missing (flat placeholder)", STEX_LAYER_NAMES[l]); }
        if (!j->ao[l]) no_ao++;
    }
    for (int k = 0; k < STEX_METAL_SLOTS; k++)
        if (!j->ok[STEX_LAYER_COUNT + k]) log_warn("surfaces: %s metal mask missing (the material metallic stays)", STEX_LAYER_NAMES[STEX_METAL_LAYERS[k]]);
    if (no_ao) log_warn("surfaces: %d layers without baked AO (tools/vfxgen/surface_pbr.py)", no_ao);
    vkDeviceWaitIdle(R->device);   // once: the placeholders leave the frame sets
    image_destroy(&S.albedo);
    image_destroy(&S.nrh);
    S.albedo = upload_layers(alb, STEX_LAYER_COUNT, ALBEDO_SIZE, VK_FORMAT_R8G8B8A8_SRGB);
    S.nrh = upload_layers(nrh, STEX_LAYER_COUNT + STEX_METAL_SLOTS, NRH_SIZE, VK_FORMAT_R8G8B8A8_UNORM);
    for (int l = 0; l < STEX_LAYER_COUNT; l++) free(alb[l]);
    for (int l = 0; l < STEX_LAYER_COUNT + STEX_METAL_SLOTS; l++) free(nrh[l]);
    free(j);
    S.loaded = missing < STEX_LAYER_COUNT;
    S.dirty = true;
    log_info("surfaces: %d layers (%d missing) in %.0f ms", STEX_LAYER_COUNT, missing, (double)(SDL_GetTicksNS() - t0) / 1e6);
    return S.loaded;
}

void render_surface_map(const u8 *ids, int n, float origin_x, float origin_z, float spacing) {
    if (!R || !R->device) return;
    vkDeviceWaitIdle(R->device);   // map changes only: the old map leaves the frame sets
    image_destroy(&S.map);
    if (!ids || n <= 0) {
        u8 z = 0;
        size_t off = 0;
        S.map = image_create(1, 1, 1, 1, VK_FORMAT_R8_UNORM, false);
        vkr_upload_image(S.map.image, 1, 1, 1, 1, &z, &off, 1);
        S.map_xf = v4(0, 0, 0, 0);
    } else {
        size_t off = 0;
        S.map = image_create((u32)n, (u32)n, 1, 1, VK_FORMAT_R8_UNORM, false);
        vkr_upload_image(S.map.image, (u32)n, (u32)n, 1, 1, ids, &off, (size_t)n * (size_t)n);
        S.map_xf = v4(origin_x, origin_z, 1.0f / spacing, (float)n);
    }
    S.dirty = true;
}

void vks_init(void) {
    memset(&S, 0, sizeof(S));
    S.r_surfaces = cvar_reg_int("r_surfaces", 1, 0, 1, CVAR_LOCKED, "terrain / building surface textures (0: vertex colours)");
    static const u8 white[4] = { 255, 255, 255, 255 }, flat[4] = { 128, 128, 255, 128 };
    u8 *a = (u8 *)white, *b = (u8 *)flat;
    S.albedo = upload_layers(&a, 1, 1, VK_FORMAT_R8G8B8A8_SRGB);
    S.nrh = upload_layers(&b, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    u8 z = 0;
    size_t off = 0;
    S.map = image_create(1, 1, 1, 1, VK_FORMAT_R8_UNORM, false);
    vkr_upload_image(S.map.image, 1, 1, 1, 1, &z, &off, 1);
    S.dirty = true;
}

void vks_shutdown(void) {
    image_destroy(&S.albedo);
    image_destroy(&S.nrh);
    image_destroy(&S.map);
}

static void write_sets(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSet sets[FRAME_SET_COUNT];
        frame_sets_of(&R->frames[i], sets);
        for (int s = 0; s < FRAME_SET_COUNT; s++) {
            if (!sets[s]) continue;
            VkDescriptorImageInfo ii[3] = { { R->sampler_linear, S.albedo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_linear, S.nrh.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                            { R->sampler_clamp, S.map.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet w[3];
            for (int k = 0; k < 3; k++) {
                w[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[k].dstSet = sets[s];
                w[k].dstBinding = (u32)(12 + k);
                w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[k].pImageInfo = &ii[k];
            }
            vkUpdateDescriptorSets(R->device, 3, w, 0, NULL);
        }
    }
}

void vks_write_bindings(void) {
    if (!S.dirty) return;
    S.dirty = false;
    write_sets();
}

void vks_samplers_changed(void) { write_sets(); }   // the caller waited for the device

void vks_frame_data(FrameData *fd) {
    fd->surf_map = S.map_xf;
    fd->surf_info = v4(0, S.loaded && S.r_surfaces->i ? 1.0f : 0.0f, 0, 0);
}
