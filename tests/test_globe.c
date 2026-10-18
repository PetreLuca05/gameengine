// Unit tests for the Maps tab globe (docs/MENU_GLOBE.md): the orthographic projection and its
// inverse, flights (legs, pull back, the short way round), the tile pyramid's levels / bounds, the
// visible box; then the shipped data: assets/globe/tiles.dgt (index sorted, unique, levels 0..4
// complete, every tile a 512 x 512 JPEG), vectors.dgv (layout, every layer the game names, chunk
// boxes hold their points, triangle layers in threes) and regions.json (each region's layers).
// Build: part of the normal CMake build (target test_globe); run: ctest (from build*/bin).
#include "ui/globe/globe_math.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static double lon_diff(double a, double b) { return fabs(fmod(fmod(a - b + 540.0, 360.0) + 360.0, 360.0) - 180.0); }

static void test_projection(void) {
    GlobeView v = { 28.6, 44.2, 400.0, 800.0, 450.0 };
    GlobeBasis b;
    globe_basis(&v, &b);
    double p[3];
    globe_project(&b, v.lon, v.lat, p);
    CHECK(fabs(p[0] - 800) < 1e-9 && fabs(p[1] - 450) < 1e-9 && fabs(p[2] - 1) < 1e-12, "the view point is the centre, facing (%.3f %.3f %.3f)", p[0], p[1], p[2]);
    // North is up, east is right.
    globe_project(&b, v.lon, v.lat + 1.0, p);
    CHECK(p[1] < 450 && fabs(p[0] - 800) < 1e-6, "north up");
    globe_project(&b, v.lon + 1.0, v.lat, p);
    CHECK(p[0] > 800, "east right");
    // The antipode is behind.
    globe_project(&b, v.lon + 180.0, -v.lat, p);
    CHECK(p[2] < -0.999, "antipode behind (%.3f)", p[2]);
    // Round trips over the visible hemisphere.
    int bad = 0;
    for (int j = -8; j <= 8; j++)
        for (int i = -8; i <= 8; i++) {
            double lon = v.lon + i * 9.5, lat = v.lat + j * 5.0;
            if (lat > 89 || lat < -89) continue;
            globe_project(&b, lon, lat, p);
            if (p[2] < 0.05) continue;
            double l2, a2;
            if (!globe_unproject(&b, p[0], p[1], &l2, &a2) || lon_diff(l2, lon) > 1e-6 || fabs(a2 - lat) > 1e-6) bad++;
        }
    CHECK(bad == 0, "project / unproject round trip (%d bad)", bad);
    double lo, la;
    CHECK(!globe_unproject(&b, 800 + 401, 450, &lo, &la), "off the disc");
    // Deep zoom (a city at 100k px): a metre still moves the point.
    GlobeView d = { 28.634, 44.173, 100000.0, 800.0, 450.0 };
    globe_basis(&d, &b);
    double q[3];
    globe_project(&b, 28.634 + 0.0001, 44.173, q);
    CHECK(q[0] - 800 > 0.1, "deep zoom resolves ~8 m (%.4f px)", q[0] - 800);
}

static void test_flight(void) {
    GlobeView cur = { -40, 24, 340, 0, 0 };
    GlobeLeg legs[3] = { { 25.0, 45.9, 4000, 1.8f, 1.0f, 0.9f }, { 28.25, 44.24, 18000, 1.3f, 0, 0.7f }, { 28.634, 44.173, 85000, 1.4f, 0, 0 } };
    GlobeFlight f;
    globe_flight_start(&f, &cur, legs, 3, 10.0);
    CHECK(f.active && f.leg == 0, "starts on leg 0");
    globe_flight_update(&f, &cur, 10.0 + 0.9);
    // The dip pulls back mid-leg: below the log-lerp of 340 .. 4000 at t = 0.5.
    double mid = exp(log(340.0) + (log(4000.0) - log(340.0)) * globe_ease_in_out(0.5f));
    CHECK(cur.radius < mid * 0.6, "pulled back mid-leg (%.0f < %.0f)", cur.radius, mid * 0.6);
    double t = 10.0;
    int legs_seen = 1;
    int last = 0;
    for (int k = 0; k < 2000 && f.active; k++) {
        t += 1.0 / 120.0;
        globe_flight_update(&f, &cur, t);
        if (f.leg != last) { legs_seen++; last = f.leg; }
    }
    CHECK(!f.active, "the flight ends");
    CHECK(legs_seen == 3, "all three legs flown (%d)", legs_seen);
    CHECK(fabs(cur.lon - 28.634) < 1e-9 && fabs(cur.lat - 44.173) < 1e-9 && fabs(cur.radius - 85000) < 1e-6, "lands on the place (%.4f %.4f %.0f)", cur.lon, cur.lat, cur.radius);
    CHECK(t - 10.0 > 1.8 + 0.9 + 1.3 + 0.7 + 1.4 - 0.05 && t - 10.0 < 6.3, "takes its time (%.2f s)", t - 10.0);
    // The short way round the date line.
    double l = globe_lerp_lon(170.0, -170.0, 0.5);
    CHECK(lon_diff(l, 180.0) < 1e-9, "lerp_lon wraps (%.2f)", l);
    CHECK(fabs(globe_ease_in_out(0) - 0) < 1e-6 && fabs(globe_ease_in_out(1) - 1) < 1e-6 && fabs(globe_ease_in_out(0.5f) - 0.5f) < 1e-6, "ease ends");
}

static void test_tiles(void) {
    CHECK(fabs(globe_tile_deg(0) - 180.0) < 1e-12 && fabs(globe_tile_deg(10) - 360.0 / 2048.0) < 1e-12, "tile sizes");
    CHECK(fabs(globe_level_ppd(4) * 360.0 - 16320.0) < 1e-6, "level 4 is 16320 texels around");
    CHECK(globe_level_for(1.0, 10) == 0 && globe_level_for(globe_level_ppd(6), 10) == 6 && globe_level_for(globe_level_ppd(6) * 1.01, 10) == 7, "level_for");
    CHECK(globe_level_for(1e9, 10) == 10, "level_for clamps");
    int bad = 0;
    for (int L = 0; L <= 10; L++)
        for (int k = 0; k < 50; k++) {
            double lon = -179.9 + k * 7.19, lat = -89.0 + k * 3.55;
            int x, y;
            globe_tile_of(L, lon, lat, &x, &y);
            double w, s, e, n;
            globe_tile_bounds(L, x, y, &w, &s, &e, &n);
            if (lon < w - 1e-9 || lon > e + 1e-9 || lat < s - 1e-9 || lat > n + 1e-9) bad++;
        }
    CHECK(bad == 0, "a point lies in its tile (%d bad)", bad);
    int x, y;
    globe_tile_of(3, 180.0, 0.0, &x, &y);
    CHECK(x == 0, "lon 180 wraps to the first column (%d)", x);
}

static void test_visible(void) {
    GlobeView v = { 28.6, 44.2, 85000.0, 800.0, 450.0 };
    GlobeBasis b;
    globe_basis(&v, &b);
    GlobeBox box = globe_visible_box(&b, 1600, 900);
    CHECK(!box.all_lon && box.east - box.west < 2.0 && box.north - box.south < 1.0, "a city view is a small box (%.3f x %.3f)", box.east - box.west, box.north - box.south);
    CHECK(globe_box_hits(&box, 28.5, 44.1, 28.7, 44.3), "hits its own place");
    CHECK(!globe_box_hits(&box, -74.1, 40.6, -73.9, 40.8), "misses New York");
    // The whole globe small: about a hemisphere.
    GlobeView g = { -40, 24, 340.0, 800.0, 450.0 };
    globe_basis(&g, &b);
    box = globe_visible_box(&b, 1600, 900);
    CHECK(box.all_lon || box.east - box.west > 170.0, "the globe view spans a hemisphere (%.0f)", box.east - box.west);
    // Across the date line.
    GlobeView p = { 179.5, 0.0, 85000.0, 800.0, 450.0 };
    globe_basis(&p, &b);
    box = globe_visible_box(&b, 1600, 900);
    CHECK(globe_box_hits(&box, -180.0, -0.5, -179.0, 0.5) && globe_box_hits(&box, 179.0, -0.5, 180.0, 0.5), "date line: both sides hit");
    CHECK(globe_lon_in(-179.9, 179.0, 181.0) && !globe_lon_in(0.0, 179.0, 181.0), "lon_in wraps");
}

// ------------------------------------------------------------------ shipped data

static unsigned char *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) b[n] = 0;
    *size = (size_t)n;
    return b;
}

static void test_tile_pack(void) {
    size_t size = 0;
    unsigned char *d = read_all("assets/globe/tiles.dgt", &size);
    CHECK(d != NULL, "assets/globe/tiles.dgt present (run from build/bin)");
    if (!d) return;
    uint32_t h[8];
    memcpy(h, d, sizeof(h));
    CHECK(!memcmp(d, "DGGT", 4) && h[1] == 1 && h[2] == GLOBE_TILE_SIZE && h[3] == 1, "tile pack header");
    uint32_t n = h[4], maxl = h[5];
    CHECK(32 + 24ull * n <= size, "index fits");
    int counts[16] = { 0 }, bad = 0, order = 0;
    unsigned long long prev = 0;
    for (uint32_t i = 0; i < n && 32 + 24ull * (i + 1) <= size; i++) {
        uint32_t e[4];
        unsigned long long off;
        memcpy(e, d + 32 + 24 * i, 16);
        memcpy(&off, d + 32 + 24 * i + 16, 8);
        if (e[0] > maxl || e[0] > 15 || e[1] >= (2u << e[0]) || e[2] >= (1u << e[0])) { bad++; continue; }
        counts[e[0]]++;
        unsigned long long key = ((unsigned long long)e[0] << 48) | ((unsigned long long)e[2] << 24) | e[1];
        if (i && key <= prev) order++;
        prev = key;
        // A baseline JPEG, 512 x 512, one component.
        if (off + e[3] > size || e[3] < 64 || d[off] != 0xFF || d[off + 1] != 0xD8) { bad++; continue; }
        int w = 0, hh = 0, comp = 0;
        for (unsigned long long p = off + 2; p + 9 < off + e[3];) {
            if (d[p] != 0xFF) { p++; continue; }
            unsigned char m = d[p + 1];
            if (m == 0xC0 || m == 0xC1 || m == 0xC2) { hh = (d[p + 5] << 8) | d[p + 6]; w = (d[p + 7] << 8) | d[p + 8]; comp = d[p + 9]; break; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
            p += 2 + ((d[p + 2] << 8) | d[p + 3]);
        }
        if (w != GLOBE_TILE_SIZE || hh != GLOBE_TILE_SIZE || comp != 1) bad++;
    }
    CHECK(bad == 0, "every tile entry valid (%d bad)", bad);
    CHECK(order == 0, "index sorted by level, row, column and unique (%d out of order)", order);
    for (int L = 0; L <= 4; L++) CHECK(counts[L] == (2 << (2 * L)), "world level %d complete (%d of %d)", L, counts[L], 2 << (2 * L));
    CHECK(maxl >= 8, "regional detail levels present (max %u)", maxl);
    free(d);
}

typedef struct { char name[40]; uint32_t kind, first_lod, lod_count, pad; } VLayer;
typedef struct { float max_ppd; uint32_t first_line, line_count, pad; } VLod;
typedef struct { uint32_t first_point, count; float w, s, e, n; } VLine;

static int find_layer(const VLayer *L, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(L[i].name, name)) return (int)i;
    return -1;
}

static void test_vectors(void) {
    size_t size = 0;
    unsigned char *d = read_all("assets/globe/vectors.dgv", &size);
    CHECK(d != NULL, "assets/globe/vectors.dgv present");
    if (!d) return;
    uint32_t h[9];
    memcpy(h, d, sizeof(h));
    CHECK(!memcmp(d, "DGGV", 4) && h[1] == 1, "vectors header");
    uint32_t nl = h[2], nlod = h[3], nline = h[4], npt = h[5], nlab = h[6];
    size_t need = 36 + 56 * (size_t)nl + 16 * (size_t)nlod + 24 * (size_t)nline + 4 * (size_t)npt + 80 * (size_t)nlab;
    CHECK(size == need, "vectors size matches its counts (%zu vs %zu)", size, need);
    if (size != need) { free(d); return; }
    const VLayer *L = (const VLayer *)(d + 36);
    const VLod *lods = (const VLod *)(d + 36 + 56 * nl);
    const VLine *lines = (const VLine *)(d + 36 + 56 * nl + 16 * nlod);
    const char *need_layers[] = { "coast", "borders", "lakes", "rivers", "us_states", "us_counties", "ro_judete", "ro_communes" };
    for (int i = 0; i < 8; i++) CHECK(find_layer(L, nl, need_layers[i]) >= 0, "layer %s", need_layers[i]);
    int bad = 0, badppd = 0, badtri = 0;
    for (uint32_t i = 0; i < nl; i++) {
        if (L[i].first_lod + L[i].lod_count > nlod || !L[i].lod_count) { bad++; continue; }
        float prev = 0;
        for (uint32_t k = 0; k < L[i].lod_count; k++) {
            const VLod *o = &lods[L[i].first_lod + k];
            if (o->max_ppd <= prev) badppd++;
            prev = o->max_ppd;
            if (o->first_line + o->line_count > nline) { bad++; continue; }
            for (uint32_t j = 0; j < o->line_count; j++) {
                const VLine *ln = &lines[o->first_line + j];
                if (ln->first_point + ln->count > npt || ln->count < 2 || ln->w > ln->e || ln->s > ln->n || ln->s < -90.01f || ln->n > 90.01f) bad++;
                if (L[i].kind == 1 && ln->count % 3) badtri++;
            }
        }
    }
    CHECK(bad == 0, "every lod / chunk in range (%d bad)", bad);
    CHECK(badppd == 0, "lods ordered by pixels per degree (%d)", badppd);
    CHECK(badtri == 0, "triangle chunks hold whole triangles (%d)", badtri);
    // regions.json: each region's layers exist.
    size_t rs = 0;
    char *rj = (char *)read_all("assets/globe/regions.json", &rs);
    CHECK(rj != NULL, "assets/globe/regions.json present");
    if (rj) {
        int regions = 0;
        for (const char *p = strstr(rj, "\"layer\""); p; p = strstr(p + 1, "\"layer\"")) regions++;
        CHECK(regions >= 2, "regions listed (%d)", regions);
        const char *ids[] = { "us_ny", "ro_ct" };
        for (int i = 0; i < 2; i++) {
            char name[64];
            snprintf(name, sizeof(name), "\"%s\"", ids[i]);
            CHECK(strstr(rj, name) != NULL, "region %s in regions.json", ids[i]);
            snprintf(name, sizeof(name), "unit:%s", ids[i]);
            CHECK(find_layer(L, nl, name) >= 0, "layer %s", name);
            const char *parts[3] = { "out", "country", "unit" };
            for (int k = 0; k < 3; k++) {
                snprintf(name, sizeof(name), "mask:%s:%s", ids[i], parts[k]);
                int li = find_layer(L, nl, name);
                CHECK(li >= 0 && L[li].kind == 1, "triangle layer %s", name);
            }
        }
        free(rj);
    }
    free(d);
}

int main(void) {
    test_projection();
    test_flight();
    test_tiles();
    test_visible();
    test_tile_pack();
    test_vectors();
    printf("test_globe: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
