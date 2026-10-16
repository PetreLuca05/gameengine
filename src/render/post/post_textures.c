// Shared post textures: 1x1 white / black, blue noise (assets/textures/bluenoise64.png), the 32^3
// grading LUT (identity by default; .cube files via post_lut_load_cube), a procedural lens dirt
// mask and the SMAA area / search tables (third_party/smaa, MIT).
#include "post_internal.h"
#include "core/platform.h"
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AreaTex.h"
#include "SearchTex.h"

#define LUT_N 32

static VkCommandBuffer once_begin(VkCommandPool *pool) {
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = R->queue_family;
    VK_CHECK(vkCreateCommandPool(R->device, &pci, NULL, pool));
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = *pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(R->device, &ai, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

static void once_end(VkCommandPool pool, VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(R->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(R->queue));
    vkDestroyCommandPool(R->device, pool, NULL);
}

// Creates a sampled image (2D or 3D) from host data and leaves it SHADER_READ_ONLY.
static void make_image(VkImage *img, VmaAllocation *alloc, VkImageView *view, VkFormat fmt, u32 w, u32 h, u32 d, const void *data, size_t bytes) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = d;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(R->vma, &ici, &aci, img, alloc, NULL));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = *img;
    vci.viewType = d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, view));
    GpuBuffer st = vkr_buffer_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(st.mapped, data, bytes);
    VkCommandPool pool;
    VkCommandBuffer cmd = once_begin(&pool);
    vkr_image_barrier(cmd, *img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w;
    r.imageExtent.height = h;
    r.imageExtent.depth = d;
    vkCmdCopyBufferToImage(cmd, st.buffer, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    vkr_image_barrier(cmd, *img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, 1);
    once_end(pool, cmd);
    vkr_buffer_destroy_now(&st);
}

static void destroy(VkImage *img, VmaAllocation *alloc, VkImageView *view) {
    if (*view) vkDestroyImageView(R->device, *view, NULL);
    if (*img) vmaDestroyImage(R->vma, *img, *alloc);
    *view = VK_NULL_HANDLE;
    *img = VK_NULL_HANDLE;
}

static u16 f2h(float f) {
    union { float f; u32 u; } v = { f };
    u32 x = v.u, sign = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    u32 m = x & 0x7FFFFFu;
    if (e <= 0) return (u16)sign;
    if (e >= 31) return (u16)(sign | 0x7C00u);
    return (u16)(sign | ((u32)e << 10) | (m >> 13));
}

static void upload_lut(const float *rgb) {
    destroy(&P.lut_img, &P.lut_alloc, &P.lut);
    u16 *px = (u16 *)malloc(sizeof(u16) * 4 * LUT_N * LUT_N * LUT_N);
    for (int i = 0; i < LUT_N * LUT_N * LUT_N; i++) {
        px[i * 4 + 0] = f2h(rgb[i * 3 + 0]);
        px[i * 4 + 1] = f2h(rgb[i * 3 + 1]);
        px[i * 4 + 2] = f2h(rgb[i * 3 + 2]);
        px[i * 4 + 3] = f2h(1.0f);
    }
    make_image(&P.lut_img, &P.lut_alloc, &P.lut, VK_FORMAT_R16G16B16A16_SFLOAT, LUT_N, LUT_N, LUT_N, px, sizeof(u16) * 4 * LUT_N * LUT_N * LUT_N);
    free(px);
}

// The baked default: identity (the look lives in the tonemapper + lift / gamma / gain); a .cube
// replaces it (resampled to 32^3 with trilinear interpolation).
static void identity_lut(void) {
    float *rgb = (float *)malloc(sizeof(float) * 3 * LUT_N * LUT_N * LUT_N);
    for (int b = 0; b < LUT_N; b++)
        for (int g = 0; g < LUT_N; g++)
            for (int r = 0; r < LUT_N; r++) {
                float *o = rgb + 3 * ((b * LUT_N + g) * LUT_N + r);
                o[0] = (float)r / (LUT_N - 1);
                o[1] = (float)g / (LUT_N - 1);
                o[2] = (float)b / (LUT_N - 1);
            }
    upload_lut(rgb);
    free(rgb);
}

bool post_lut_load_cube(const char *path, int slot) {
    (void)slot;
    if (!path || !path[0]) { identity_lut(); return true; }
    char buf[600];
    size_t size = 0;
    char *txt = platform_read_file(platform_data_path(path, buf, sizeof(buf)), &size);
    if (!txt) { log_warn("post: LUT %s not found", path); return false; }
    int n = 0;
    float dmin[3] = { 0, 0, 0 }, dmax[3] = { 1, 1, 1 };
    float *src = NULL;
    int count = 0;
    char *line = strtok(txt, "\r\n");
    while (line) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "LUT_3D_SIZE", 11)) {
            n = atoi(line + 11);
            if (n < 2 || n > 128) break;
            src = (float *)malloc(sizeof(float) * 3 * (size_t)n * n * n);
        } else if (!strncmp(line, "DOMAIN_MIN", 10)) sscanf(line + 10, "%f %f %f", &dmin[0], &dmin[1], &dmin[2]);
        else if (!strncmp(line, "DOMAIN_MAX", 10)) sscanf(line + 10, "%f %f %f", &dmax[0], &dmax[1], &dmax[2]);
        else if (src && ((line[0] >= '0' && line[0] <= '9') || line[0] == '-' || line[0] == '.')) {
            float r, g, b;
            if (sscanf(line, "%f %f %f", &r, &g, &b) == 3 && count < n * n * n) {
                src[count * 3] = r;
                src[count * 3 + 1] = g;
                src[count * 3 + 2] = b;
                count++;
            }
        }
        line = strtok(NULL, "\r\n");
    }
    free(txt);
    if (!src || count != n * n * n) { log_warn("post: %s: malformed .cube (%d of %d entries)", path, count, n * n * n); free(src); return false; }
    // Resample to 32^3 (the .cube order is r fastest, then g, then b).
    float *rgb = (float *)malloc(sizeof(float) * 3 * LUT_N * LUT_N * LUT_N);
    for (int b = 0; b < LUT_N; b++)
        for (int g = 0; g < LUT_N; g++)
            for (int r = 0; r < LUT_N; r++) {
                float c[3] = { (float)r / (LUT_N - 1), (float)g / (LUT_N - 1), (float)b / (LUT_N - 1) };
                float f[3];
                int i0[3], i1[3];
                for (int k = 0; k < 3; k++) {
                    float x = clampf((c[k] - dmin[k]) / fmaxf(dmax[k] - dmin[k], 1e-6f), 0, 1) * (float)(n - 1);
                    i0[k] = (int)floorf(x);
                    i1[k] = DG_MIN(i0[k] + 1, n - 1);
                    f[k] = x - (float)i0[k];
                }
                float *o = rgb + 3 * ((b * LUT_N + g) * LUT_N + r);
                for (int ch = 0; ch < 3; ch++) {
                    float acc = 0;
                    for (int k = 0; k < 8; k++) {
                        int ir = (k & 1) ? i1[0] : i0[0], ig = (k & 2) ? i1[1] : i0[1], ib = (k & 4) ? i1[2] : i0[2];
                        float wgt = ((k & 1) ? f[0] : 1 - f[0]) * ((k & 2) ? f[1] : 1 - f[1]) * ((k & 4) ? f[2] : 1 - f[2]);
                        acc += wgt * src[3 * ((ib * n + ig) * n + ir) + ch];
                    }
                    o[ch] = acc;
                }
            }
    upload_lut(rgb);
    free(rgb);
    free(src);
    log_info("post: LUT %s (%d^3 -> %d^3)", path, n, LUT_N);
    return true;
}

static void make_dirt(void) {
    // Lens dirt: soft smudges + a few rings, deterministic.
    const int S = 512;
    u8 *px = (u8 *)calloc((size_t)S * S * 4, 1);
    u32 seed = 1234567u;
    float *acc = (float *)calloc((size_t)S * S, sizeof(float));
    for (int k = 0; k < 90; k++) {
        seed = seed * 1664525u + 1013904223u;
        float cx = (float)(seed % S);
        seed = seed * 1664525u + 1013904223u;
        float cy = (float)(seed % S);
        seed = seed * 1664525u + 1013904223u;
        float r = 8.0f + (float)(seed % 60);
        seed = seed * 1664525u + 1013904223u;
        float a = 0.15f + (float)(seed % 100) / 160.0f;
        bool ring = (seed & 7) == 0;
        for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); y++)
            for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); x++) {
                if (x < 0 || y < 0 || x >= S || y >= S) continue;
                float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
                float v = ring ? expf(-(d - 0.9f) * (d - 0.9f) * 60.0f) : fmaxf(0.0f, 1.0f - d) * fmaxf(0.0f, 1.0f - d);
                acc[y * S + x] += v * a;
            }
    }
    for (int i = 0; i < S * S; i++) {
        u8 v = (u8)(clampf(acc[i], 0, 1) * 255.0f);
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
        px[i * 4 + 3] = 255;
    }
    make_image(&P.dirt_img, &P.dirt_alloc, &P.dirt, VK_FORMAT_R8G8B8A8_UNORM, S, S, 1, px, (size_t)S * S * 4);
    free(acc);
    free(px);
}

void post_textures_init(void) {
    u32 white = 0xFFFFFFFFu, black = 0xFF000000u;
    make_image(&P.white_img, &P.white_alloc, &P.white, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, &white, 4);
    make_image(&P.black_img, &P.black_alloc, &P.black, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, &black, 4);
    char buf[600];
    int w = 0, h = 0, comp = 0;
    u8 *noise = stbi_load(platform_data_path("assets/textures/bluenoise64.png", buf, sizeof(buf)), &w, &h, &comp, 4);
    if (noise && w == 64 && h == 64) {
        make_image(&P.noise_img, &P.noise_alloc, &P.noise, VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, noise, 64 * 64 * 4);
    } else {
        log_warn("post: assets/textures/bluenoise64.png missing (tools/gen_bluenoise.py) - white noise fallback");
        u8 *wn = (u8 *)malloc(64 * 64 * 4);
        u32 s = 99;
        for (int i = 0; i < 64 * 64 * 4; i++) { s = s * 1664525u + 1013904223u; wn[i] = (u8)(s >> 24); }
        make_image(&P.noise_img, &P.noise_alloc, &P.noise, VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, wn, 64 * 64 * 4);
        free(wn);
    }
    if (noise) stbi_image_free(noise);
    identity_lut();
    make_dirt();
    make_image(&P.smaa_area_img, &P.smaa_area_alloc, &P.smaa_area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, 1, areaTexBytes, AREATEX_SIZE);
    make_image(&P.smaa_search_img, &P.smaa_search_alloc, &P.smaa_search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, searchTexBytes,
               SEARCHTEX_SIZE);
}

void post_textures_shutdown(void) {
    destroy(&P.white_img, &P.white_alloc, &P.white);
    destroy(&P.black_img, &P.black_alloc, &P.black);
    destroy(&P.noise_img, &P.noise_alloc, &P.noise);
    destroy(&P.lut_img, &P.lut_alloc, &P.lut);
    destroy(&P.dirt_img, &P.dirt_alloc, &P.dirt);
    destroy(&P.smaa_area_img, &P.smaa_area_alloc, &P.smaa_area);
    destroy(&P.smaa_search_img, &P.smaa_search_alloc, &P.smaa_search);
}
