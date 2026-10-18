// Block compression codec tests (docs/TEXTURE_MIPS.md "Block compression", src/render/bc_codec.c):
//   accounting   every BC7 mode the encoder writes (1, 3, 4, 5, 6, 7 with every partition / rotation /
//                index selection) decodes to exactly the error the encoder scored (bit layout, anchors,
//                endpoint expansion and the encoder agree)
//   solid        a flat colour: BC7 within 1 per channel, BC4 exact, BC1 within the 565 grid's error
//   gradients    noisy ramps, and two ramps on the two subsets of every partition: bounded errors, the
//                two-subset modes chosen where they win
//   BC4 optimum  within 10 % of an exhaustive search over every endpoint pair
//   alpha test   binary coverage (foliage cards) keeps every texel on its side of 0.5 in BC7 / BC3
//   images       real assets (an albedo photo, a packed data map, an alpha-tested atlas, a normal map):
//                PSNR floors per format and quality; encode speed reported
//   determinism  the same bytes for the same input, rows encoded in any split
// --dds <dir> also writes every encoded image as a .dds and our decode as a .png for
// tools/tests/bc_crosscheck.py (Pillow's decoders must agree).
#include "render/bc_codec.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static u32 g_rng = 0x2545F491u;
static u32 rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int rndi(int lo, int hi) { return lo + (int)(rnd() % (u32)(hi - lo + 1)); }

static const char *g_dds_dir;

static u32 block_err(const u8 *a, const u8 *b, int channels) {
    u32 e = 0;
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < channels; c++) {
            int d = (int)a[i * 4 + c] - (int)b[i * 4 + c];
            e += (u32)(d * d);
        }
    return e;
}
static int block_maxdiff(const u8 *a, const u8 *b, int channels) {
    int m = 0;
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < channels; c++) {
            int d = abs((int)a[i * 4 + c] - (int)b[i * 4 + c]);
            if (d > m) m = d;
        }
    return m;
}

static void smooth_block(u8 blk[64], bool alpha) {
    int c0[4], c1[4];
    for (int c = 0; c < 4; c++) c0[c] = rndi(0, 255), c1[c] = rndi(0, 255);
    if (!alpha) c0[3] = c1[3] = 255;
    int dir = rndi(0, 3);
    for (int i = 0; i < 16; i++) {
        int x = i & 3, y = i >> 2;
        float t = dir == 0 ? x / 3.0f : dir == 1 ? y / 3.0f : dir == 2 ? (x + y) / 6.0f : (x * y) / 9.0f;
        for (int c = 0; c < 4; c++) blk[i * 4 + c] = (u8)DG_CLAMP((int)lrintf(c0[c] + (c1[c] - c0[c]) * t) + rndi(-3, 3), 0, 255);
        if (!alpha) blk[i * 4 + 3] = 255;
    }
}

// ------------------------------------------------------------------ accounting

static void test_accounting(void) {
    u8 blk[64], enc[16], dec[64];
    int bad = 0, checked = 0;
    for (int trial = 0; trial < 40; trial++) {
        bool alpha = trial & 1;
        smooth_block(blk, alpha);
        for (int mode = 1; mode <= 7; mode++) {
            if (mode == 2) continue;
            int variants = mode == 1 || mode == 3 || mode == 7 ? 64 : mode == 4 ? 8 : mode == 5 ? 4 : 1;
            for (int v = 0; v < variants; v++) {
                for (int q = 0; q < 3; q++) {
                    u32 e = bc7_encode_block_mode(blk, enc, mode, v, (BcQuality)q);
                    CHECK(bc7_block_mode(enc) == mode, "mode %d written as %d", mode, bc7_block_mode(enc));
                    bc_decode_block(BC_7, enc, dec);
                    u32 m = block_err(blk, dec, 4);
                    checked++;
                    if (m != e) {
                        if (bad++ < 8) printf("  mode %d variant %d q %d: encoder scored %u, decoded error %u\n", mode, v, q, e, m);
                    }
                }
            }
        }
    }
    CHECK(bad == 0, "%d of %d forced-mode blocks decode to another error than the encoder scored", bad, checked);
}

// ------------------------------------------------------------------ solid / gradients / partitions

static void test_solid(void) {
    u8 blk[64], enc[16], dec[64];
    int worst7 = 0, worst1 = 0, worst4 = 0;
    for (int trial = 0; trial < 400; trial++) {
        u8 c[4] = { (u8)rndi(0, 255), (u8)rndi(0, 255), (u8)rndi(0, 255), (u8)(trial & 1 ? rndi(0, 255) : 255) };
        if (trial < 8) c[0] = c[1] = c[2] = (u8)(trial * 36), c[3] = 255;
        for (int i = 0; i < 16; i++) memcpy(&blk[i * 4], c, 4);
        bc_encode_block(BC_7, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_7, enc, dec);
        worst7 = DG_MAX(worst7, block_maxdiff(blk, dec, 4));
        bc_encode_block(BC_1, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_1, enc, dec);
        worst1 = DG_MAX(worst1, block_maxdiff(blk, dec, 3));
        bc_encode_block(BC_4, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_4, enc, dec);
        worst4 = DG_MAX(worst4, block_maxdiff(blk, dec, 1));
    }
    CHECK(worst7 <= 1, "BC7 solid colour off by %d", worst7);
    CHECK(worst4 == 0, "BC4 solid value off by %d", worst4);
    CHECK(worst1 <= 3, "BC1 solid colour off by %d", worst1);
    printf("  solid: worst channel error BC7 %d, BC1 %d, BC4 %d\n", worst7, worst1, worst4);
}

static double rmse_of(u32 err, int n) { return sqrt((double)err / (double)n); }

static void test_gradients(void) {
    u8 blk[64], enc[16], dec[64];
    u32 e7 = 0, e1 = 0, e5 = 0, e3 = 0;
    for (int trial = 0; trial < 500; trial++) {
        smooth_block(blk, trial & 1);
        bc_encode_block(BC_7, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_7, enc, dec);
        e7 += block_err(blk, dec, 4);
        bc_encode_block(BC_3, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_3, enc, dec);
        e3 += block_err(blk, dec, 4);
        bc_encode_block(BC_1, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_1, enc, dec);
        e1 += block_err(blk, dec, 3);
        bc_encode_block(BC_5, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_5, enc, dec);
        e5 += block_err(blk, dec, 2);
    }
    double r7 = rmse_of(e7, 500 * 64), r3 = rmse_of(e3, 500 * 64), r1 = rmse_of(e1, 500 * 48), r5 = rmse_of(e5, 500 * 32);
    printf("  noisy ramps: RMSE BC7 %.2f, BC3 %.2f, BC1 %.2f, BC5 %.2f\n", r7, r3, r1, r5);
    // The +-3 noise alone is ~2.0 RMSE. The diagonal / product ramps hold up to 10 levels per channel
    // over the full range: 4-level BC1 (and BC3's colour) cannot follow them, 8-level BC4 / BC5 nearly.
    // Bounds ~15 % over the measured values: a regression detector.
    CHECK(r7 < 2.6, "BC7 ramp RMSE %.2f", r7);
    CHECK(r5 < 2.5, "BC5 ramp RMSE %.2f", r5);
    CHECK(r3 < 7.8, "BC3 ramp RMSE %.2f", r3);
    CHECK(r1 < 8.8, "BC1 ramp RMSE %.2f", r1);

    // two independent ramps (four colours, 8 steps: mode 1's 3-bit indices) on the two subsets of each
    // partition: no line through RGB space fits the block, a two-subset mode does
    u32 worst = 0;
    int picked2 = 0;
    for (int p = 0; p < 64; p++) {
        u8 two[64];
        int col[4][3];
        for (int k = 0; k < 4; k++)
            for (int c = 0; c < 3; c++) col[k][c] = rndi(0, 255);
        int seen[2] = { 0, 0 }, cnt[2] = { 0, 0 };
        for (int i = 0; i < 16; i++) cnt[bc7_partition_subset(p, i)]++;
        for (int i = 0; i < 16; i++) {
            int sb = bc7_partition_subset(p, i);
            float t = cnt[sb] > 1 ? (float)seen[sb]++ / (float)(cnt[sb] - 1) : 0.0f;
            t = roundf(t * 7.0f) / 7.0f;
            for (int c = 0; c < 3; c++) two[i * 4 + c] = (u8)lrintf((float)col[sb * 2][c] + (float)(col[sb * 2 + 1][c] - col[sb * 2][c]) * t);
            two[i * 4 + 3] = 255;
        }
        bc_encode_block(BC_7, two, enc, BC_Q_NORMAL);
        bc_decode_block(BC_7, enc, dec);
        u32 e = block_err(two, dec, 4);
        if (e > worst) worst = e;
        int m = bc7_block_mode(enc);
        if (m == 1 || m == 3) picked2++;
    }
    printf("  two-ramp partitions: worst block RMSE %.2f, two-subset mode chosen in %d / 64\n", rmse_of(worst, 48), picked2);
    CHECK(rmse_of(worst, 48) < 2.0, "two-ramp block RMSE %.2f", rmse_of(worst, 48));
    CHECK(picked2 >= 56, "two-subset modes chosen for %d of 64 two-ramp blocks", picked2);
}

// BC4 within 10 % of the best of all 65536 endpoint pairs (both palette modes), + 1 level RMS.
static void test_bc4_optimal(void) {
    double worst_ratio = 0;
    for (int trial = 0; trial < 24; trial++) {
        u8 blk[64], enc[16], dec[64];
        smooth_block(blk, false);
        if (trial % 3 == 2)   // extremes: the 6-value mode's 0 / 255
            for (int i = 0; i < 16; i += 5) blk[i * 4] = (u8)(i & 1 ? 255 : 0);
        bc_encode_block(BC_4, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_4, enc, dec);
        u32 mine = block_err(blk, dec, 1), best = 0xFFFFFFFFu;
        for (int r0 = 0; r0 < 256; r0++)
            for (int r1 = 0; r1 < 256; r1++) {
                u8 b[8] = { (u8)r0, (u8)r1, 0, 0, 0, 0, 0, 0 }, d[64];
                // every texel takes its nearest palette entry: decode index k for each k, keep the nearest
                u32 e = 0;
                int pal[8];
                for (int k = 0; k < 8; k++) {
                    b[2] = (u8)k;   // texel 0 index k (3 bits)
                    bc_decode_block(BC_4, b, d);
                    pal[k] = d[0];
                }
                for (int i = 0; i < 16; i++) {
                    int bd = 1 << 30;
                    for (int k = 0; k < 8; k++) {
                        int dd = (pal[k] - blk[i * 4]) * (pal[k] - blk[i * 4]);
                        if (dd < bd) bd = dd;
                    }
                    e += (u32)bd;
                }
                if (e < best) best = e;
            }
        // within 10 % of the optimum, or 1 level RMS over it on blocks it reproduces nearly exactly
        double ratio = (double)mine / (double)(best + 16);
        if (ratio > worst_ratio) worst_ratio = ratio;
        if (ratio > 1.1) {
            printf("    block %d: error %u, optimum %u (r0 %d r1 %d):", trial, mine, best, enc[0], enc[1]);
            for (int i = 0; i < 16; i++) printf(" %d>%d", blk[i * 4], dec[i * 4]);
            printf("\n");
        }
    }
    printf("  BC4 vs exhaustive search: worst error / (optimum + 16) %.3f\n", worst_ratio);
    CHECK(worst_ratio <= 1.10, "BC4 error %.3f x (the optimum + 16)", worst_ratio);
}

static void test_alpha_test(void) {
    u8 blk[64], enc[16], dec[64];
    int flips7 = 0, flips3 = 0;
    for (int trial = 0; trial < 500; trial++) {
        smooth_block(blk, false);
        for (int i = 0; i < 16; i++) blk[i * 4 + 3] = (rnd() & 1) ? 255 : 0;
        bc_encode_block(BC_7, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_7, enc, dec);
        for (int i = 0; i < 16; i++) flips7 += (blk[i * 4 + 3] >= 128) != (dec[i * 4 + 3] >= 128);
        bc_encode_block(BC_3, blk, enc, BC_Q_NORMAL);
        bc_decode_block(BC_3, enc, dec);
        for (int i = 0; i < 16; i++) flips3 += (blk[i * 4 + 3] >= 128) != (dec[i * 4 + 3] >= 128);
    }
    CHECK(flips7 == 0, "BC7: %d alpha-tested texels flipped", flips7);
    CHECK(flips3 == 0, "BC3: %d alpha-tested texels flipped", flips3);
}

// ------------------------------------------------------------------ images

static void write_dds(const char *path, BcFormat f, bool srgb, u32 w, u32 h, const u8 *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    u32 hdr[32];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x20534444u;   // "DDS "
    hdr[1] = 124;
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;
    hdr[3] = h;
    hdr[4] = w;
    hdr[5] = (u32)size;
    hdr[7] = 1;
    hdr[19] = 32;           // pixel format
    hdr[20] = 0x4;          // FOURCC
    hdr[21] = 0x30315844u;  // "DX10"
    hdr[27] = 0x1000;       // caps: texture
    fwrite(hdr, 4, 32, fp);
    u32 dxgi = f == BC_1 ? (srgb ? 72u : 71u) : f == BC_3 ? (srgb ? 78u : 77u) : f == BC_4 ? 80u : f == BC_5 ? 83u : (srgb ? 99u : 98u);
    u32 dx10[5] = { dxgi, 3, 0, 1, 0 };
    fwrite(dx10, 4, 5, fp);
    fwrite(data, 1, size, fp);
    fclose(fp);
}

static double psnr(const u8 *a, const u8 *b, size_t texels, int c0, int c1) {
    double se = 0;
    for (size_t i = 0; i < texels; i++)
        for (int c = c0; c < c1; c++) {
            double d = (double)a[i * 4 + c] - (double)b[i * 4 + c];
            se += d * d;
        }
    double mse = se / ((double)texels * (double)(c1 - c0));
    return mse <= 1e-12 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

typedef struct ImgCase { const char *path; const char *name; BcFormat f; int q; int c0, c1; double floor_db; bool srgb; } ImgCase;

static void test_images(void) {
    static const ImgCase cases[] = {
        // floors ~1.5 dB under the values measured when the encoder was written (a regression detector)
        { "assets/terrain/tex/dirt_albedo.jpg", "dirt_albedo_bc7", BC_7, BC_Q_NORMAL, 0, 3, 43.5, true },
        { "assets/terrain/tex/dirt_albedo.jpg", "dirt_albedo_bc7_fast", BC_7, BC_Q_FAST, 0, 3, 43.0, true },
        { "assets/terrain/tex/dirt_albedo.jpg", "dirt_albedo_bc1", BC_1, BC_Q_NORMAL, 0, 3, 34.0, true },
        { "assets/terrain/tex/dirt_nrh.png", "dirt_nrh_bc7", BC_7, BC_Q_NORMAL | BC_DATA, 0, 4, 34.0, false },
        { "assets/foliage/tex/grass_short_albedo.png", "grass_albedo_bc7", BC_7, BC_Q_NORMAL, 0, 4, 38.5, true },
        { "assets/foliage/tex/grass_short_albedo.png", "grass_albedo_bc3", BC_3, BC_Q_NORMAL, 0, 4, 38.0, true },
        { "assets/treefbx/bark01_normal.png", "bark_normal_bc5", BC_5, BC_Q_NORMAL, 0, 2, 37.5, false },
        { "assets/treefbx/bark01_normal.png", "bark_normal_bc7", BC_7, BC_Q_NORMAL | BC_DATA, 0, 3, 33.0, false },
        { "assets/textures/surfaces/brick_ao.png", "brick_ao_bc4", BC_4, BC_Q_NORMAL, 0, 1, 50.0, false },
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        const ImgCase *c = &cases[k];
        int w, h, comp;
        u8 *full = stbi_load(c->path, &w, &h, &comp, 4);
        if (!full) { printf("  skip %s (not found: run from build/bin)\n", c->path); continue; }
        // a 512 x 512 window (at most) keeps the test quick
        u32 cw = (u32)DG_MIN(w, 512), ch = (u32)DG_MIN(h, 512);
        u8 *px = (u8 *)malloc((size_t)cw * ch * 4), *dec = (u8 *)malloc((size_t)cw * ch * 4);
        for (u32 y = 0; y < ch; y++) memcpy(px + (size_t)y * cw * 4, full + ((size_t)y * (u32)w) * 4, (size_t)cw * 4);
        stbi_image_free(full);
        if (c->f == BC_4)   // a grey map: the value in r (as the engine would upload it)
            for (size_t i = 0; i < (size_t)cw * ch; i++) px[i * 4 + 1] = px[i * 4 + 2] = 0, px[i * 4 + 3] = 255;
        size_t bytes = bc_image_bytes(c->f, cw, ch);
        u8 *blocks = (u8 *)malloc(bytes);
        clock_t t0 = clock();
        bc_encode_rows(c->f, px, cw, ch, 0, (ch + 3) / 4, blocks, c->q);
        double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
        bc_decode_image(c->f, blocks, cw, ch, dec);
        double db = psnr(px, dec, (size_t)cw * ch, c->c0, c->c1);
        double mbps = (double)cw * ch / 1e6 / (sec > 1e-6 ? sec : 1e-6);
        printf("  %-22s %s %ux%u: PSNR %.2f dB (floor %.0f), %.2f Mtexel/s\n", c->name, bc_format_name(c->f), cw, ch, db, c->floor_db, mbps);
        CHECK(db >= c->floor_db, "%s: PSNR %.2f dB below %.1f", c->name, db, c->floor_db);
        // determinism: rows encoded in two halves give the same bytes
        u8 *again = (u8 *)malloc(bytes);
        u32 bh = (ch + 3) / 4;
        bc_encode_rows(c->f, px, cw, ch, bh / 2, bh, again, c->q);
        bc_encode_rows(c->f, px, cw, ch, 0, bh / 2, again, c->q);
        CHECK(memcmp(again, blocks, bytes) == 0, "%s: split encode differs", c->name);
        if (g_dds_dir) {
            char p[512];
            snprintf(p, sizeof(p), "%s/%s.dds", g_dds_dir, c->name);
            write_dds(p, c->f, false, cw, ch, blocks, bytes);   // UNORM: Pillow decodes the stored values either way
            snprintf(p, sizeof(p), "%s/%s_ref.png", g_dds_dir, c->name);
            stbi_write_png(p, (int)cw, (int)ch, 4, dec, (int)cw * 4);
        }
        free(again);
        free(blocks);
        free(px);
        free(dec);
    }
}

// Every BC7 mode / partition / rotation as one image for the cross-check: 64 x 64 texels, block i
// forced to mode table[i].
static void write_mode_sheet(void) {
    if (!g_dds_dir) return;
    const u32 W = 64, H = 64;   // 16 x 16 blocks = 256
    u8 *blocks = (u8 *)malloc(bc_image_bytes(BC_7, W, H)), *dec = (u8 *)malloc((size_t)W * H * 4);
    int b = 0;
    for (int mode = 1; mode <= 7 && b < 256; mode++) {
        if (mode == 2) continue;
        int variants = mode == 1 || mode == 3 || mode == 7 ? 64 : mode == 4 ? 8 : mode == 5 ? 4 : 16;
        for (int v = 0; v < variants && b < 256; v++, b++) {
            u8 blk[64];
            smooth_block(blk, mode >= 4);
            if (mode == 1 || mode == 3 || mode == 7)   // texels of different brightness so both subsets differ
                for (int i = 0; i < 16; i++) blk[i * 4 + (i & 3) % 3] = (u8)(i * 16);
            bc7_encode_block_mode(blk, blocks + (size_t)b * 16, mode, v, BC_Q_NORMAL);
        }
    }
    for (; b < 256; b++) {
        u8 blk[64];
        smooth_block(blk, true);
        bc_encode_block(BC_7, blk, blocks + (size_t)b * 16, BC_Q_NORMAL);
    }
    bc_decode_image(BC_7, blocks, W, H, dec);
    char p[512];
    snprintf(p, sizeof(p), "%s/bc7_modes.dds", g_dds_dir);
    write_dds(p, BC_7, false, W, H, blocks, bc_image_bytes(BC_7, W, H));
    snprintf(p, sizeof(p), "%s/bc7_modes_ref.png", g_dds_dir);
    stbi_write_png(p, (int)W, (int)H, 4, dec, (int)W * 4);
    free(blocks);
    free(dec);
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--dds")) g_dds_dir = argv[i + 1];
    test_accounting();
    test_solid();
    test_gradients();
    test_bc4_optimal();
    test_alpha_test();
    test_images();
    write_mode_sheet();
    printf("test_bc_codec: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
