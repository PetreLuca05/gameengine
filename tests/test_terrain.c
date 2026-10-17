// Terrain shading tests (docs/TERRAIN_SHADING.md):
//   params       assets/terrain parses: every material slot has a layer and a texture, pair lookups are
//                symmetric (the reversed entry flagged), the families' modes / textures resolve
//   maps         blend map fractions = the box reference exactly (1 / 255), curvature = the CPU reference,
//                flow accumulates downhill (valley > slope > ridge), moist low ground, border distance
//                within 8 % + 0.3 m of the brute-force reference, path wear falls off beside the path
//   borders      the signed distance from the blend map is linear across a straight border (error < 5 %
//                of the radius), the band width is the pair's width within 10 % whatever the sampling step
//                (camera distance / terrain LOD), height blending is order independent and puts the taller
//                material on top at the band centre, a material outside its band never wins, noise-broken
//                borders have no straight run longer than 1.5 m, transition sets only inside the band,
//                moisture / wear biases move the border by their metres
//   tiling       hex bombing: autocorrelation at the tile period < 0.35 (plain tiling 1.0), the
//                histogram-preserving blend keeps mean and variance within 5 %
#include "world/terrain/terrain_params.h"
#include "world/terrain/terrain_maps.h"
#include "world/terrain/terrain_blend.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
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

static TerrainParams g_p;

static void test_params(void) {
    CHECK(terrain_params_load(&g_p, TER_DIR), "assets/terrain loads (%s)", g_p.err);
    for (int s = 0; s < 14; s++) {
        CHECK(g_p.slot_layer[s][0] != 0, "slot %d has a layer", s);
        CHECK(g_p.gpu.layer[s].tex[0] >= 0 && g_p.gpu.layer[s].tex[0] < g_p.tex_count, "slot %d (%s): texture %g", s, g_p.slot_layer[s], g_p.gpu.layer[s].tex[0]);
        CHECK(g_p.gpu.layer[s].tex[2] > 0, "slot %d: tile size", s);
    }
    CHECK(g_p.pair_count >= 8, "%d transition pairs", g_p.pair_count);
    int asym = 0;
    for (int a = 0; a < TER_SLOTS; a++)
        for (int b = 0; b < TER_SLOTS; b++) {
            u32 x = g_p.gpu.pair_of[a * TER_SLOTS + b], y = g_p.gpu.pair_of[b * TER_SLOTS + a];
            if ((x & 0x7FFFFFFFu) != (y & 0x7FFFFFFFu) || ((x | y) && (x >> 31) == (y >> 31) && x)) asym++;
        }
    CHECK(!asym, "pair table symmetric with the reversed entry flagged (%d bad)", asym);
    // grass -> dirt uses a transition set and a rim
    u32 gd = g_p.gpu.pair_of[0 * TER_SLOTS + 3] & 0x7FFFFFFFu;
    CHECK(gd > 0 && ((u32)g_p.gpu.pair[gd].b[3] & (TPM_SET | TPM_RIM)) == (TPM_SET | TPM_RIM) && g_p.gpu.pair[gd].b[0] >= 0, "grass / dirt: set + rim");
    CHECK(sizeof(TerrainGpu) % 16 == 0, "TerrainGpu is vec4-aligned (%zu B)", sizeof(TerrainGpu));
}

#define N 96
static float g_h[N * N];
static u8 g_mat[N * N], g_blend[N * N * 4], g_attr[N * N * 4];

// A synthetic raster: a gaussian valley along x = 48 (rising south), grass west of x = 40.5 / dirt east, a path
// strip along z = 70, a mud patch in the valley.
static void build_raster(void) {
    for (int z = 0; z < N; z++)
        for (int x = 0; x < N; x++) {
            float dx = (float)x - 48.0f;
            g_h[z * N + x] = 3.0f * (1.0f - expf(-dx * dx / 200.0f)) + 0.01f * (float)z;
            u8 m = x < 40 ? 0 : 3;
            if (z >= 69 && z <= 71) m = 8;
            if (abs(x - 48) <= 2 && z >= 10 && z <= 20) m = 13;
            g_mat[z * N + x] = m;
        }
}

static void test_maps(void) {
    build_raster();
    TerrainMapParams mp;
    terrain_map_params_default(&mp);
    terrain_maps_build(g_h, g_mat, N, &mp, g_blend, g_attr);
    int bad_frac = 0, bad_curv = 0, bad_dist = 0;
    float worst_dist = 0;
    for (int z = 2; z < N - 2; z += 3)
        for (int x = 2; x < N - 2; x += 3) {
            int a, b;
            float fa, fb;
            terrain_ref_fractions(g_mat, N, x, z, mp.blend_radius, &a, &b, &fa, &fb);
            const u8 *t = &g_blend[(z * N + x) * 4];
            if (t[0] != a || fabsf(t[2] / 255.0f - fa) > 0.5f / 255.0f + 1e-4f || (fb > 0 && (t[1] != b || fabsf(t[3] / 255.0f - fb) > 0.5f / 255.0f + 1e-4f))) bad_frac++;
            float c = terrain_ref_curvature(g_h, N, x, z, 1.0f);
            float enc = 0.5f + c * mp.curvature_scale;
            enc = enc < 0 ? 0 : (enc > 1 ? 1 : enc);
            if (fabsf(g_attr[(z * N + x) * 4] / 255.0f - enc) > 1.0f / 255.0f + 1e-4f) bad_curv++;
            float d = terrain_ref_border_distance(g_mat, N, x, z, 1.0f), got = g_attr[(z * N + x) * 4 + 3] / 255.0f * 8.0f;
            if (d < 7.5f && fabsf(got - d) > 0.3f + 0.08f * d) { bad_dist++; worst_dist = fmaxf(worst_dist, fabsf(got - d)); }
        }
    CHECK(!bad_frac, "blend fractions = box reference (%d bad)", bad_frac);
    CHECK(!bad_curv, "curvature = CPU reference (%d bad)", bad_curv);
    CHECK(!bad_dist, "border distance within 8 %% + 0.3 m (%d bad, worst %.2f m)", bad_dist, worst_dist);
    // the valley floor is concave (cavity), the flanks less: curvature > 0.5 on the floor
    CHECK(g_attr[(50 * N + 48) * 4] > 128, "valley floor concave (curvature %d)", g_attr[(50 * N + 48) * 4]);
    // moisture: valley floor > flank, mud fully wet
    int floor_m = g_attr[(30 * N + 48) * 4 + 1], flank_m = g_attr[(30 * N + 20) * 4 + 1];
    CHECK(floor_m > flank_m + 40, "moisture: valley floor %d > flank %d", floor_m, flank_m);
    CHECK(g_attr[(15 * N + 48) * 4 + 1] == 255, "mud fully moist");
    // flow accumulation reference: the floor collects more than a flank cell
    static float acc[N * N];
    terrain_ref_flow(g_h, N, acc);
    CHECK(acc[30 * N + 48] > 20.0f * acc[30 * N + 20], "flow: valley %.0f >> flank %.0f", acc[30 * N + 48], acc[30 * N + 20]);
    // wear: 1 on the path, falling beside it
    CHECK(g_attr[(70 * N + 20) * 4 + 2] == 255 && g_attr[(73 * N + 20) * 4 + 2] < 200 && g_attr[(76 * N + 20) * 4 + 2] < 20, "path wear %d / %d / %d", g_attr[(70 * N + 20) * 4 + 2],
          g_attr[(73 * N + 20) * 4 + 2], g_attr[(76 * N + 20) * 4 + 2]);
}

static float g_maps[4] = { 0, 0, 1, N };

// d along a line across the grass / dirt border (x from 34 to 47 at z = 40)
static void test_borders(void) {
    TerrainMapParams mp;
    terrain_map_params_default(&mp);
    const float R = (float)mp.blend_radius;
    // linear signed distance across the straight border at x = 39.5
    float worst = 0;
    for (float x = 37.0f; x <= 42.0f; x += 0.05f) {
        TerWeights w;
        ter_gather(g_blend, g_maps, x, 40.0f, &w);
        float d = ter_border_distance(&w, R, 1.0f);
        float want = w.id[0] == 0 ? 39.5f - x : x - 39.5f;
        worst = fmaxf(worst, fabsf(d - want));
    }
    CHECK(worst < 0.05f * R + 0.06f, "signed border distance linear (worst error %.3f m)", worst);
    // band width = the pair's width at any sampling step
    TerPairGpu q = g_p.gpu.pair[g_p.gpu.pair_of[0 * TER_SLOTS + 3] & 0x7FFFFFFFu];
    q.b[3] = TPM_HEIGHT;   // a clean border (no noise) for the width
    const float steps[3] = { 0.01f, 0.05f, 0.2f };
    for (int s = 0; s < 3; s++) {
        float lo = 1e9f, hi = -1e9f;
        for (float x = 36.0f; x <= 43.0f; x += steps[s]) {
            TerWeights w;
            ter_gather(g_blend, g_maps, x, 40.0f, &w);
            float d = ter_border_distance(&w, R, 1.0f);
            float ta = ter_band(&q, w.id[0] == 0 ? d : -d);   // t of grass
            if (ta > 0.001f && ta < 0.999f) lo = fminf(lo, x), hi = fmaxf(hi, x);
        }
        float width = hi - lo + steps[s];
        CHECK(fabsf(width - q.a[0]) <= 0.1f * q.a[0] + steps[s], "band width %.3f m = pair width %.2f m within 10 %% (step %.2f m)", width, q.a[0], steps[s]);
    }
    // height blending: order independent, the taller on top at the centre, nothing outside its band
    float t2[2] = { 0.5f, 0.5f }, H[2] = { 0.8f, 0.2f }, o1[2], o2[2], t2r[2] = { 0.5f, 0.5f }, Hr[2] = { 0.2f, 0.8f };
    ter_height_blend(2, t2, H, 0.2f, o1);
    ter_height_blend(2, t2r, Hr, 0.2f, o2);
    CHECK(fabsf(o1[0] - o2[1]) < 1e-6f && fabsf(o1[1] - o2[0]) < 1e-6f, "height blend order independent");
    CHECK(o1[0] > 0.9f, "taller material on top at the band centre (%.3f)", o1[0]);
    float t3[2] = { 1.0f, 0.0f }, H3[2] = { 0.0f, 1.0f }, o3[2];
    ter_height_blend(2, t3, H3, 0.2f, o3);
    CHECK(o3[1] == 0.0f, "a material outside its band never wins on height (%.3f)", o3[1]);
    // transition set only inside the band
    CHECK(ter_set_weight(&q, 0.0f) == 0.0f && ter_set_weight(&q, 1.0f) == 0.0f && ter_set_weight(&q, 0.5f) > 0.5f, "transition set confined to the band");
    // noise-broken borders: along z the border position wanders; no straight run (constant within 2 cm) > 1.5 m
    TerPairGpu qn = g_p.gpu.pair[g_p.gpu.pair_of[0 * TER_SLOTS + 3] & 0x7FFFFFFFu];
    float attr[3] = { 0.5f, 0.5f, 0.5f }, zero[2] = { 0, 0 }, wind[2] = { 1, 0 };
    float run = 0, worst_run = 0, prev = -1e9f;
    for (float z = 10.0f; z < 80.0f; z += 0.1f) {
        float s = ter_border_shift(&qn, 39.5f, z, g_p.gpu.g.macro[1], attr, zero, zero, wind, 4);
        if (fabsf(s - prev) < 0.02f * 0.1f) run += 0.1f;
        else run = 0;
        worst_run = fmaxf(worst_run, run);
        prev = s;
    }
    CHECK(worst_run <= 1.5f, "no straight border run over 1.5 m (longest %.2f m)", worst_run);
    // weather / wear: the moisture bias moves the border by its metres
    TerPairGpu qm = qn;
    qm.b[3] = TPM_MOISTURE;
    qm.d[0] = 0.8f;
    float wet[3] = { 0.5f, 1.0f, 0.5f }, dry[3] = { 0.5f, 0.0f, 0.5f };
    float sw = ter_border_shift(&qm, 0, 0, 0.2f, wet, zero, zero, wind, 4), sd = ter_border_shift(&qm, 0, 0, 0.2f, dry, zero, zero, wind, 4);
    CHECK(fabsf(sw - 0.8f) < 1e-5f && fabsf(sd + 0.8f) < 1e-5f, "moisture bias shifts the border +-0.8 m (%.3f, %.3f)", sw, sd);
}

#define TS 128
static void test_tiling(void) {
    static float img[TS * TS];
    unsigned seed = 7;
    // a tileable test texture: value noise with a few blobs
    for (int y = 0; y < TS; y++)
        for (int x = 0; x < TS; x++) img[y * TS + x] = 0.5f + 0.25f * sinf(6.2831853f * x * 3 / TS) * sinf(6.2831853f * y * 2 / TS) + 0.1f * ter_noise(x * 0.25f, y * 0.25f);
    (void)seed;
    double mean = 0, var = 0;
    for (int i = 0; i < TS * TS; i++) mean += img[i];
    mean /= TS * TS;
    for (int i = 0; i < TS * TS; i++) var += (img[i] - mean) * (img[i] - mean);
    var /= TS * TS;
    // sample both schemes over 8 x 8 tiles
    enum { S = 256 };
    static float hex[S * S], plain[S * S];
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float u = (float)x / S * 8.0f, v = (float)y / S * 8.0f;
            hex[y * S + x] = ter_hex_sample(img, TS, (float)mean, u, v, 1.0f, 1.0f, 0.12f);
            plain[y * S + x] = ter_plain_sample(img, TS, u, v);
        }
    double hm = 0, hv = 0;
    for (int i = 0; i < S * S; i++) hm += hex[i];
    hm /= S * S;
    for (int i = 0; i < S * S; i++) hv += (hex[i] - hm) * (hex[i] - hm);
    hv /= S * S;
    CHECK(fabs(hm - mean) / mean < 0.05, "hex blend keeps the mean (%.4f vs %.4f)", hm, mean);
    CHECK(fabs(hv - var) / var < 0.05, "hex blend keeps the variance (%.5f vs %.5f)", hv, var);
    // autocorrelation at the tile period (32 samples): plain = 1
    double ch = 0, cp = 0, nh = 0, np = 0;
    const int lag = S / 8;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S - lag; x++) {
            ch += (hex[y * S + x] - hm) * (hex[y * S + x + lag] - hm);
            nh += (hex[y * S + x] - hm) * (hex[y * S + x] - hm);
            cp += (plain[y * S + x] - mean) * (plain[y * S + x + lag] - mean);
            np += (plain[y * S + x] - mean) * (plain[y * S + x] - mean);
        }
    CHECK(cp / np > 0.99, "plain tiling repeats (autocorrelation %.3f)", cp / np);
    CHECK(ch / nh < 0.35, "hex bombing breaks the repeat (autocorrelation %.3f < 0.35)", ch / nh);
}

int main(void) {
    test_params();
    test_maps();
    test_borders();
    test_tiling();
    printf("test_terrain: %d / %d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
