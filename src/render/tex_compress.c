// Block-compressed textures (docs/TEXTURE_MIPS.md "Block compression"): see tex_compress.h.
#include "tex_compress.h"
#include "vk_internal.h"
#include "core/jobs.h"
#include "core/platform.h"
#include "core/profiler.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXC_FILE_MAGIC 0x43424744u   // "DGBC"
#define TEXC_FILE_VERSION 1u
#define TEXC_ROWS_PER_TASK 8u         // block rows per job (a 1024-wide level: 2048 blocks)

static struct {
    Cvar *on;
    bool device_ok;                   // BC7 UNORM + SRGB sampleable with linear filtering
    char dir[512];
    float to_linear[256];
    u8 to_srgb[4096];
    SDL_AtomicInt encoded, cached, images;
    SDL_AtomicInt bytes_in_kb, bytes_out_kb, encode_ms, key_us, read_us;   // key_us: hashing, read_us: cache files
} TC;

// ------------------------------------------------------------------ mips

static void srgb_tables(void) {
    for (int i = 0; i < 256; i++) {
        float c = (float)i / 255.0f;
        TC.to_linear[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    for (int i = 0; i < 4096; i++) {
        float l = (float)i / 4095.0f;
        float c = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
        TC.to_srgb[i] = (u8)DG_CLAMP((int)(c * 255.0f + 0.5f), 0, 255);
    }
}

u8 *texc_halve(const u8 *src, int w, int h, bool srgb, int *ow, int *oh) {
    if (TC.to_srgb[4095] != 255) srgb_tables();   // before texc_init (unit tests): main thread
    int nw = DG_MAX(1, w / 2), nh = DG_MAX(1, h / 2);
    u8 *dst = (u8 *)malloc((size_t)nw * (size_t)nh * 4);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            float c[3] = { 0, 0, 0 };
            u32 a = 0;
            for (int k = 0; k < 4; k++) {
                int sx = DG_MIN(x * 2 + (k & 1), w - 1), sy = DG_MIN(y * 2 + (k >> 1), h - 1);
                const u8 *p = &src[((size_t)sy * (size_t)w + (size_t)sx) * 4];
                for (int i = 0; i < 3; i++) c[i] += srgb ? TC.to_linear[p[i]] : (float)p[i];
                a += p[3];
            }
            u8 *o = &dst[((size_t)y * (size_t)nw + (size_t)x) * 4];
            for (int i = 0; i < 3; i++)
                o[i] = srgb ? TC.to_srgb[DG_CLAMP((int)(c[i] * 0.25f * 4095.0f + 0.5f), 0, 4095)] : (u8)DG_CLAMP((int)(c[i] * 0.25f + 0.5f), 0, 255);
            o[3] = (u8)((a + 2) / 4);
        }
    *ow = nw;
    *oh = nh;
    return dst;
}

u8 *texc_mip_chain(const u8 *rgba, u32 w, u32 h, bool srgb, TexcImage im[16], u32 *levels) {
    size_t total = 0;
    u32 n = 0;
    for (u32 lw = w, lh = h;; lw = DG_MAX(1u, lw / 2), lh = DG_MAX(1u, lh / 2)) {
        total += (size_t)lw * lh * 4;
        n++;
        if ((lw == 1 && lh == 1) || n == 16) break;
    }
    u8 *chain = (u8 *)malloc(total);
    memcpy(chain, rgba, (size_t)w * h * 4);
    size_t at = 0;
    int lw = (int)w, lh = (int)h;
    for (u32 l = 0; l < n; l++) {
        im[l] = (TexcImage){ chain + at, (u32)lw, (u32)lh };
        if (l + 1 == n) break;
        int nw, nh;
        u8 *next = texc_halve(chain + at, lw, lh, srgb, &nw, &nh);
        at += (size_t)lw * lh * 4;
        memcpy(chain + at, next, (size_t)nw * nh * 4);
        free(next);
        lw = nw;
        lh = nh;
    }
    *levels = n;
    return chain;
}

// ------------------------------------------------------------------ cache key

static inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static inline u64 mix64(u64 h, u64 w) { return rotl64(h ^ (w * 0x87C37B91114253D5ull), 31) * 0x4CF5AD432745937Full; }
static u64 fmix64(u64 k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ull;
    return k ^ (k >> 33);
}

// Four independent lanes over 32-byte strides (memory speed), then the tail.
static u64 hash_bytes(u64 seed, const u8 *p, size_t n) {
    u64 a = seed ^ 0x9E3779B97F4A7C15ull, b = seed + 0xC2B2AE3D27D4EB4Full, c = rotl64(seed, 17) ^ 0x165667B19E3779F9ull, d = ~seed;
    size_t len = n;
    while (n >= 32) {
        u64 w[4];
        memcpy(w, p, 32);
        a = mix64(a, w[0]);
        b = mix64(b, w[1]);
        c = mix64(c, w[2]);
        d = mix64(d, w[3]);
        p += 32;
        n -= 32;
    }
    u64 t = 0;
    for (size_t i = 0; i < n; i++) t = t << 8 | p[i];
    a = mix64(a, t ^ (u64)len);
    return fmix64(a ^ rotl64(b, 17) ^ rotl64(c, 31) ^ rotl64(d, 47));
}

static u64 texc_key(const TexcImage *im, int n, TexcKind k, int q) {
    u64 head[4] = { (u64)BC_CODEC_VERSION << 32 | TEXC_FILE_VERSION, (u64)k << 32 | (u32)q, (u64)n, 0x7465786331ull };
    u64 h = hash_bytes(0x5DEECE66Dull, (const u8 *)head, sizeof(head));
    for (int i = 0; i < n; i++) {
        u64 dims = (u64)im[i].w << 32 | im[i].h;
        h = hash_bytes(h, (const u8 *)&dims, sizeof(dims));
        h = hash_bytes(h, im[i].rgba, (size_t)im[i].w * im[i].h * 4);
    }
    return h;
}

// ------------------------------------------------------------------ disk cache

typedef struct TexcFileHeader { u32 magic, version, codec, format, count, kind; u64 key; } TexcFileHeader;

static void cache_path(u64 key, char *buf, size_t size) { snprintf(buf, size, "%s%016llx.bct", TC.dir, (unsigned long long)key); }

// The blocks straight into `out` (one open, no copy); false when missing or not this input's file.
static bool cache_read(u64 key, const TexcImage *im, int n, u8 *out, size_t total) {
    char path[600];
    cache_path(key, path, sizeof(path));
    PROF_WAIT_BEGIN(PW_IO);
    FILE *f = fopen(path, "rb");
    if (!f) { PROF_WAIT_END(); return false; }
    TexcFileHeader h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == TEXC_FILE_MAGIC && h.version == TEXC_FILE_VERSION && h.codec == BC_CODEC_VERSION &&
              h.format == BC_7 && h.count == (u32)n && h.key == key;
    for (int i = 0; ok && i < n; i++) {
        u32 wh[2];
        ok = fread(wh, 8, 1, f) == 1 && wh[0] == im[i].w && wh[1] == im[i].h;
    }
    ok = ok && fread(out, 1, total, f) == total && fgetc(f) == EOF;
    fclose(f);
    PROF_WAIT_END();
    prof_count(PC_IO_READS, 1);
    prof_count(PC_IO_BYTES, (i64)total);
    if (!ok) log_warn("texc: %s does not match its key (ignored, re-encoded)", path);
    return ok;
}

// Written beside and renamed: another process never reads half a file.
static void cache_write(u64 key, const TexcImage *im, int n, TexcKind k, const u8 *blocks, size_t total) {
    size_t head = sizeof(TexcFileHeader) + (size_t)n * 8;
    u8 *data = (u8 *)malloc(head + total);
    if (!data) return;
    TexcFileHeader h = { TEXC_FILE_MAGIC, TEXC_FILE_VERSION, BC_CODEC_VERSION, BC_7, (u32)n, (u32)k, key };
    memcpy(data, &h, sizeof(h));
    for (int i = 0; i < n; i++) {
        u32 wh[2] = { im[i].w, im[i].h };
        memcpy(data + sizeof(h) + (size_t)i * 8, wh, 8);
    }
    memcpy(data + head, blocks, total);
    char path[600], tmp[640];
    cache_path(key, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.%u.tmp", path, (unsigned)SDL_GetCurrentThreadID());
    if (!platform_write_file(tmp, data, head + total) || !SDL_RenamePath(tmp, path))
        SDL_RemovePath(tmp);   // a full disk, or another process won the race (the same bytes)
    free(data);
}

// ------------------------------------------------------------------ encode

typedef struct TexcTask { u32 image, by0, by1; } TexcTask;
typedef struct TexcJob {
    const TexcImage *im;
    const size_t *offsets;
    u8 *out;
    const TexcTask *task;
    int q;
} TexcJob;

static void encode_tasks(void *ctx, int begin, int end) {
    const TexcJob *j = (const TexcJob *)ctx;
    for (int t = begin; t < end; t++) {
        const TexcTask *k = &j->task[t];
        const TexcImage *im = &j->im[k->image];
        bc_encode_rows(BC_7, im->rgba, im->w, im->h, k->by0, k->by1, j->out + j->offsets[k->image], j->q);
    }
}

bool texc_enabled(void) { return TC.device_ok && TC.on && TC.on->i && prof_fix_enabled(PFX_TEX_BC); }

u32 texc_vk_format(TexcKind k) { return k == TEXC_COLOR ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK; }

u8 *texc_encode(const TexcImage *im, int n, TexcKind k, size_t *offsets, size_t *total_out) {
    if (!texc_enabled() || n <= 0) return NULL;
    PROF_BEGIN("texc_encode");
    size_t total = 0;
    u32 tasks = 0;
    for (int i = 0; i < n; i++) {
        offsets[i] = total;
        total += bc_image_bytes(BC_7, im[i].w, im[i].h);
        tasks += ((im[i].h + 3) / 4 + TEXC_ROWS_PER_TASK - 1) / TEXC_ROWS_PER_TASK;
    }
    *total_out = total;
    int q = BC_Q_NORMAL | (k == TEXC_DATA ? BC_DATA : 0);
    u8 *out = (u8 *)malloc(total);
    u64 t_key = SDL_GetTicksNS();
    u64 key = texc_key(im, n, k, q);
    u64 t_read = SDL_GetTicksNS();
    SDL_AddAtomicInt(&TC.key_us, (int)((t_read - t_key) / 1000));
    size_t in_bytes = 0;
    for (int i = 0; i < n; i++) in_bytes += (size_t)im[i].w * im[i].h * 4;
    SDL_AddAtomicInt(&TC.images, n);
    SDL_AddAtomicInt(&TC.bytes_in_kb, (int)(in_bytes >> 10));
    SDL_AddAtomicInt(&TC.bytes_out_kb, (int)(total >> 10));
    bool hit = cache_read(key, im, n, out, total);
    SDL_AddAtomicInt(&TC.read_us, (int)((SDL_GetTicksNS() - t_read) / 1000));
    if (hit) {
        SDL_AtomicIncRef(&TC.cached);
        PROF_END();
        return out;
    }
    u64 t0 = SDL_GetTicksNS();
    TexcTask *task = (TexcTask *)malloc(sizeof(TexcTask) * tasks);
    u32 nt = 0;
    for (int i = 0; i < n; i++) {
        u32 rows = (im[i].h + 3) / 4;
        for (u32 r = 0; r < rows; r += TEXC_ROWS_PER_TASK) task[nt++] = (TexcTask){ (u32)i, r, DG_MIN(rows, r + TEXC_ROWS_PER_TASK) };
    }
    TexcJob job = { im, offsets, out, task, q };
    if (jobs_on_worker()) encode_tasks(&job, 0, (int)nt);
    else jobs_parallel_for((int)nt, 1, encode_tasks, &job);
    free(task);
    SDL_AddAtomicInt(&TC.encode_ms, (int)((SDL_GetTicksNS() - t0) / 1000000));
    SDL_AtomicIncRef(&TC.encoded);
    cache_write(key, im, n, k, out, total);
    PROF_END();
    return out;
}

u8 *texc_encode_array(const u8 *data, const size_t *mip_offs, u32 mips, u32 layers, u32 size, TexcKind k, size_t *bc_offs, size_t *total) {
    if (!texc_enabled() || !mips || !layers || size < 4) return NULL;   // 1-texel placeholders stay RGBA8
    u32 n = mips * layers;
    TexcImage *im = (TexcImage *)malloc(sizeof(TexcImage) * n);
    size_t *io = (size_t *)malloc(sizeof(size_t) * n);
    for (u32 m = 0; m < mips; m++) {
        u32 s = DG_MAX(size >> m, 1u);
        for (u32 l = 0; l < layers; l++) im[m * layers + l] = (TexcImage){ data + mip_offs[m] + (size_t)s * s * 4 * l, s, s };
    }
    u8 *bc = texc_encode(im, (int)n, k, io, total);
    if (bc)
        for (u32 m = 0; m < mips; m++) bc_offs[m] = io[m * layers];
    free(im);
    free(io);
    return bc;
}

// ------------------------------------------------------------------ commands

static void cmd_stats(int argc, const char **argv) {
    (void)argc, (void)argv;
    int in = SDL_GetAtomicInt(&TC.bytes_in_kb), out = SDL_GetAtomicInt(&TC.bytes_out_kb);
    log_info("texc: %s (device BC7 %s); %d sets from the cache, %d encoded (%d ms), keys %.0f ms, cache reads %.0f ms, %d images, %.1f MB RGBA8 -> "
             "%.1f MB BC7 (level data, %s)",
             texc_enabled() ? "on" : "off", TC.device_ok ? "yes" : "no", SDL_GetAtomicInt(&TC.cached), SDL_GetAtomicInt(&TC.encoded),
             SDL_GetAtomicInt(&TC.encode_ms), SDL_GetAtomicInt(&TC.key_us) / 1000.0, SDL_GetAtomicInt(&TC.read_us) / 1000.0, SDL_GetAtomicInt(&TC.images),
             (double)in / 1024.0, (double)out / 1024.0, TC.dir);
}

static void cmd_cache_clear(int argc, const char **argv) {
    (void)argc, (void)argv;
    int n = 0, count = 0;
    char **files = SDL_GlobDirectory(TC.dir, "*.bct", 0, &count);
    for (int i = 0; files && i < count; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s%s", TC.dir, files[i]);
        if (SDL_RemovePath(path)) n++;
    }
    SDL_free(files);
    log_info("texc: %d cache files deleted from %s", n, TC.dir);
}

void texc_init(void) {
    srgb_tables();
    TC.on = cvar_reg_int("r_texture_compress", 1, 0, 1, CVAR_LOCKED,
                         "block-compressed world / terrain / foliage textures (BC7, 1 byte per texel; docs/TEXTURE_MIPS.md, PF-0035): 0 RGBA8 (world textures at once, the rest at their next load)");
    // The startup's world textures load before the command line applies (main.c parse_args): a
    // "+set r_texture_compress <v>" there is taken now, so they load in that format once (dev builds).
    for (int i = 1; !cvar_release_locks() && i + 2 < g_engine.argc; i++)
        if (!strcmp(g_engine.argv[i], "+set") && !SDL_strcasecmp(g_engine.argv[i + 1], "r_texture_compress")) {
            CvarSource src = cvar_source_push(CVAR_SRC_HOST);
            cvar_set("r_texture_compress", g_engine.argv[i + 2]);
            cvar_source_pop(src);
        }
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatProperties a, b;
    vkGetPhysicalDeviceFormatProperties(R->gpu, VK_FORMAT_BC7_UNORM_BLOCK, &a);
    vkGetPhysicalDeviceFormatProperties(R->gpu, VK_FORMAT_BC7_SRGB_BLOCK, &b);
    TC.device_ok = (a.optimalTilingFeatures & need) == need && (b.optimalTilingFeatures & need) == need;
    if (!TC.device_ok) log_warn("texc: the device cannot sample BC7: textures stay RGBA8");
    platform_writable_dir("assets/cache/tex/", TC.dir, sizeof(TC.dir));
    cmd_register("tex_compress_stats", cmd_stats, "block compression (PF-0035): sets from the cache / encoded, RGBA8 vs BC7 bytes, the cache dir");
    cmd_register("tex_cache_clear", cmd_cache_clear, "delete the block-compressed texture cache (assets/cache/tex/*.bct: re-encoded at the next load)");
}
