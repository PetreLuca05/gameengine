// Grass asset tests (docs/FOLIAGE_GRASS.md, docs/GROUND_REFERENCE.md 7.1):
//   assets      every assets/foliage/grass/*.json parses; heights / widths / densities sit inside the target
//               spec's ranges for the asset's kind; fades grow with the fidelity level; cover ids fit
//   mesh        cards x segments x 2 triangles; every vertex inside grass_mesh_extent; uvs inside the
//               asset's cells (one cell per card); the root row at v = cell bottom, R = height 0 at the root
//               and 1 at the tip (the shading parameter); deterministic per variant, variants differ
//   placement   the placement rule (openworld_foliage.c grass_cover: jittered 0.35 m grid, sum of density x
//               clumping keep x cell area) yields the expected clumps per m2 within 5 % (grass_expected_density),
//               clumping makes bare and dense patches, the spec's meadow density (2 .. 6 clumps / m2) holds
//   material    MAT_COVER + id, two-sided masked foliage, fades / shadow per level, no shadow = 0.01 m
#include "render/vgeo/grass_asset.h"
#include "render/vgeo/vgeo_gpu.h"
#include "render/render.h"
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

static GrassAsset g_a[GRASS_MAX_ASSETS];
static int g_n;

// Target spec (GROUND_REFERENCE.md 7.1): card height range per asset (m), widest spread allowed.
typedef struct Spec { const char *name; float h_lo, h_hi; } Spec;
static const Spec k_spec[] = { { "grass_broad", 0.25f, 0.45f }, { "grass_tussock", 0.3f, 0.5f }, { "grass_tall", 0.45f, 0.8f },
                               { "grass_short", 0.15f, 0.35f }, { "wheat", 0.8f, 1.0f } };

static void test_assets(void) {
    g_n = grass_assets_load_dir(g_a, GRASS_MAX_ASSETS);
    CHECK(g_n >= 5, "%d grass assets loaded (>= 5: broad, tussock, tall, short, wheat)", g_n);
    for (int i = 0; i < g_n; i++) {
        const GrassAsset *a = &g_a[i];
        CHECK(a->atlas[0] != 0, "%s: atlas named", a->name);
        CHECK(a->cell_count >= 1 && a->cell_count <= a->cols * a->rows, "%s: cells %d in a %dx%d atlas", a->name, a->cell_count, a->cols, a->rows);
        for (int t = 1; t < GRASS_TIERS; t++) {
            CHECK(a->tier[t].end_m >= a->tier[t - 1].end_m, "%s: fade end grows with the level (%d)", a->name, t);
            CHECK(a->tier[t].density >= a->tier[t - 1].density, "%s: density grows with the level (%d)", a->name, t);
        }
        for (size_t s = 0; s < sizeof(k_spec) / sizeof(k_spec[0]); s++) {
            if (strcmp(k_spec[s].name, a->name)) continue;
            CHECK(a->height_m[0] >= k_spec[s].h_lo - 0.03f && a->height_m[1] <= k_spec[s].h_hi + 0.03f, "%s: height %.2f..%.2f m inside the spec %.2f..%.2f",
                  a->name, a->height_m[0], a->height_m[1], k_spec[s].h_lo, k_spec[s].h_hi);
        }
        float p[12];
        grass_cover_params(a, p);
        CHECK(p[0] > 0.5f && p[0] <= 1.0f, "%s: root darkening %.2f in (0.5, 1]", a->name, p[0]);
        CHECK(p[8] >= 0 && p[8] <= 1.0f, "%s: translucency %.2f in [0, 1]", a->name, p[8]);
    }
    CHECK(g_n <= GRASS_MAX_ASSETS, "cover ids fit the 3 flag bits");
}

static void test_mesh(void) {
    for (int i = 0; i < g_n; i++) {
        const GrassAsset *a = &g_a[i];
        float hmax, rmax;
        grass_mesh_extent(a, &hmax, &rmax);
        VgFoliageMesh m0, m1, m2;
        grass_gen_mesh(&m0, a, 0);
        grass_gen_mesh(&m1, a, 0);
        grass_gen_mesh(&m2, a, 1);
        CHECK(m0.index_count == (u32)(a->cards * a->segments * 6), "%s: %u indices = cards x segments x 2 triangles", a->name, m0.index_count);
        CHECK(m0.vertex_count == m1.vertex_count && !memcmp(m0.pos, m1.pos, sizeof(float) * 3 * m0.vertex_count), "%s: deterministic per variant", a->name);
        CHECK(a->variants < 2 || memcmp(m0.pos, m2.pos, sizeof(float) * 3 * DG_MIN(m0.vertex_count, m2.vertex_count)) != 0, "%s: variants differ", a->name);
        int outside = 0, bad_uv = 0, bad_root = 0, bad_tip = 0;
        u32 per_card = (u32)(a->segments + 1) * 2;
        for (u32 v = 0; v < m0.vertex_count; v++) {
            const float *p = &m0.pos[v * 3];
            if (p[1] < -1e-4f || p[1] > hmax + 1e-3f || sqrtf(p[0] * p[0] + p[2] * p[2]) > rmax + 1e-3f) outside++;
            float u = m0.uv[v * 2], vv = m0.uv[v * 2 + 1];
            // the card's cell: from its root vertex
            u32 c0 = v / per_card * per_card;
            int cx = (int)floorf(0.5f * (m0.uv[c0 * 2] + m0.uv[c0 * 2 + 2]) * (float)a->cols), cy = (int)floorf(m0.uv[c0 * 2 + 1] * (float)a->rows - 1e-3f);
            if (u < (float)cx / a->cols - 1e-4f || u > (float)(cx + 1) / a->cols + 1e-4f || vv < (float)cy / a->rows - 1e-4f || vv > (float)(cy + 1) / a->rows + 1e-4f) bad_uv++;
            float h = (float)(m0.color[v] & 255u) / 255.0f;
            u32 row = (v - c0) / 2;
            if (row == 0 && (h > 0.01f || p[1] > 1e-4f)) bad_root++;
            if (row == (u32)a->segments && h < 0.99f) bad_tip++;
        }
        CHECK(!outside, "%s: %d vertices outside the declared extent (h %.2f, r %.2f)", a->name, outside, hmax, rmax);
        CHECK(!bad_uv, "%s: %d uvs outside their card's cell", a->name, bad_uv);
        CHECK(!bad_root && !bad_tip, "%s: root rows at height 0 (%d bad), tip rows at 1 (%d bad)", a->name, bad_root, bad_tip);
        vgeo_foliage_mesh_free(&m0);
        vgeo_foliage_mesh_free(&m1);
        vgeo_foliage_mesh_free(&m2);
    }
}

// The placement rule of openworld_foliage.c grass_cover on one material over a 64 x 64 m patch.
static float placed_density(int mat, int tier, float x0, float z0, float side, int *per_asset) {
    const float cell = 0.35f;
    int per = (int)(side / cell), n = 0;
    float c = side / (float)per;
    u32 seed = 1234u;
    for (int gz = 0; gz < per; gz++)
        for (int gx = 0; gx < per; gx++) {
            u32 h = (u32)gx * 73856093u ^ (u32)gz * 19349663u ^ 707u * 83492791u;
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            h ^= h >> 15;
            float x = x0 + ((float)gx + (float)(h & 0xFFFFFFu) / 16777216.0f) * c, z = z0 + ((float)gz + (float)((h >> 9) & 0xFFFFFFu) / 16777216.0f) * c;
            float w[GRASS_MAX_ASSETS], sum = 0;
            for (int i = 0; i < g_n; i++) {
                w[i] = g_a[i].density[mat] > 0 ? g_a[i].density[mat] * g_a[i].tier[tier].density * grass_clump_keep(&g_a[i], x, z, seed + (u32)i * 31u) : 0;
                sum += w[i];
            }
            u32 h2 = (u32)gx * 2654435761u ^ (u32)gz * 40503u ^ 0x9E3779B9u;
            h2 ^= h2 >> 15;
            h2 *= 0x2C1B3C6Du;
            h2 ^= h2 >> 12;
            float pick = (float)(h2 & 0xFFFFFFu) / 16777216.0f;
            if (pick >= sum * c * c) continue;
            float t = pick / (c * c);
            int k = 0;
            while (k < g_n - 1 && t >= w[k]) t -= w[k++];
            per_asset[k]++;
            n++;
        }
    return (float)n / (side * side);
}

static void test_placement(void) {
    const int mats[] = { 0, 1, 10 };   // grass, meadow, field
    for (int m = 0; m < 3; m++) {
        int per_asset[GRASS_MAX_ASSETS] = { 0 };
        float got = placed_density(mats[m], GRASS_TIERS - 1, 100.0f, -300.0f, 96.0f, per_asset);
        float want = 0;
        for (int i = 0; i < g_n; i++) want += grass_expected_density(&g_a[i], mats[m], GRASS_TIERS - 1);
        CHECK(want > 0 && fabsf(got - want) / want < 0.05f, "material %d: %.2f clumps / m2 placed, %.2f expected (5 %%)", mats[m], got, want);
        if (mats[m] == 1) CHECK(got >= 2.0f && got <= 6.0f, "meadow: %.2f clumps / m2 inside the spec 2 .. 6", got);
        for (int i = 0; i < g_n; i++) {
            float e = grass_expected_density(&g_a[i], mats[m], GRASS_TIERS - 1);
            if (e > 0.2f) CHECK(fabsf((float)per_asset[i] / (96.0f * 96.0f) - e) / e < 0.15f, "material %d: %s %.2f / m2 (expected %.2f, 15 %%)", mats[m], g_a[i].name,
                                 (float)per_asset[i] / (96.0f * 96.0f), e);
        }
    }
    // clumping: the keep rate has bare patches and full ones
    for (int i = 0; i < g_n; i++) {
        if (g_a[i].clump_amount < 0.4f) continue;
        float lo = 1, hi = 0;
        for (int z = 0; z < 80; z++)
            for (int x = 0; x < 80; x++) {
                float k = grass_clump_keep(&g_a[i], (float)x * 0.7f, (float)z * 0.7f, 5u);
                lo = fminf(lo, k);
                hi = fmaxf(hi, k);
            }
        CHECK(lo < 1.0f - g_a[i].clump_amount + 0.05f && hi > 0.9f, "%s: clumping keep spans %.2f .. %.2f", g_a[i].name, lo, hi);
    }
}

static void test_material(void) {
    for (int i = 0; i < g_n; i++) {
        VgMaterial m;
        for (int t = 0; t < GRASS_TIERS; t++) {
            grass_material(&g_a[i], t, (u32)i, &m);
            CHECK((m.flags & MAT_COVER) && MAT_COVER_ID(m.flags) == (u32)i, "%s: MAT_COVER id %u", g_a[i].name, MAT_COVER_ID(m.flags));
            CHECK((m.flags & (VGM_FOLIAGE | VGM_MASKED | VGM_TWO_SIDED | VGM_DITHER_FADE)) == (VGM_FOLIAGE | VGM_MASKED | VGM_TWO_SIDED | VGM_DITHER_FADE), "%s: masked two-sided foliage",
                  g_a[i].name);
            CHECK((m.wpo & 0xFFu) == WPO_GRASS, "%s: grass WPO (never the tree snippet)", g_a[i].name);
            CHECK(m.fade_start == g_a[i].tier[t].full_m && m.fade_end == g_a[i].tier[t].end_m, "%s level %d: fades", g_a[i].name, t);
            CHECK(g_a[i].tier[t].shadow_m > 0 ? m.shadow_max_dist == g_a[i].tier[t].shadow_m : m.shadow_max_dist < 0.02f, "%s level %d: shadow distance", g_a[i].name, t);
        }
    }
}

int main(void) {
    test_assets();
    test_mesh();
    test_placement();
    test_material();
    printf("test_grass: %d / %d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
