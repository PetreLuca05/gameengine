// mapgen_preview <level.json> [out.png] [metres per pixel=2]: top-down render of an open-world level's
// generated terrain (docs/ZOMBIE_MAP.md): hill-shaded materials, water, roads, bridges, POI footprints
// and labels' anchor crosses. The same generator code as the game (src/world/mapgen*.c), no engine.
#include "world/mapgen.h"
#include "world/mapgen_poi.h"
#include "world/level_include.h"
#include "cJSON.h"
#include "stb_image_write.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = 0;
    fclose(f);
    return s;
}

// Included levels (level_include.h) are read next to the level file.
static char g_dir[512];
static cJSON *read_level(const char *level, void *user) {
    (void)user;
    char path[700];
    snprintf(path, sizeof(path), "%s%s.json", g_dir, level);
    char *text = read_file(path);
    cJSON *r = text ? cJSON_Parse(text) : NULL;
    free(text);
    return r;
}

static void put(u8 *img, int w, int h, int x, int y, vec3 c) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    u8 *p = img + ((size_t)y * w + x) * 3;
    p[0] = (u8)DG_CLAMP((int)(c.x * 255), 0, 255);
    p[1] = (u8)DG_CLAMP((int)(c.y * 255), 0, 255);
    p[2] = (u8)DG_CLAMP((int)(c.z * 255), 0, 255);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: mapgen_preview <level.json> [out.png] [m per px]\n"); return 1; }
    const char *out = argc > 2 ? argv[2] : "mapgen_preview.png";
    float mpp = argc > 3 ? (float)atof(argv[3]) : 2.0f;
    char *text = read_file(argv[1]);
    if (!text) { printf("cannot read %s\n", argv[1]); return 1; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { printf("JSON parse error\n"); return 1; }
    snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    char *slash = strrchr(g_dir, '/'), *bslash = strrchr(g_dir, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) slash[1] = 0;
    else g_dir[0] = 0;
    char ierr[256];
    if (!level_include_resolve(root, read_level, NULL, ierr, sizeof(ierr))) { printf("%s\n", ierr); return 1; }
    const cJSON *world = cJSON_GetObjectItemCaseSensitive(root, "world");
    const cJSON *seed = cJSON_GetObjectItemCaseSensitive(world, "seed");
    static MapGen g;
    mapgen_init(&g, cJSON_IsNumber(seed) ? (u32)seed->valueint : 1);
    char err[256];
    if (!mapgen_parse(&g, world, err, sizeof(err))) { printf("world: %s\n", err); return 1; }
    static MapPois pois;
    if (!mappoi_plan(&pois, &g, cJSON_GetObjectItemCaseSensitive(root, "pois"), err, sizeof(err))) { printf("pois: %s\n", err); return 1; }
    mapgen_finalize(&g);
    MgRaster r;
    mapgen_raster_alloc(&r, g.raster_half, mpp);
    mapgen_raster_rows(&g, &r, 0, r.n);
    mapgen_raster_slopes(&g, &r, 0, r.n);
    int w = r.n, h = r.n;
    u8 *img = (u8 *)malloc((size_t)w * h * 3);
    vec3 sun = v3_norm(v3(-0.5f, 0.7f, 0.5f));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int iz = y;   // north (-Z) up
            size_t i = (size_t)iz * r.n + x;
            float wx = r.origin + (float)x * mpp, wz = r.origin + (float)iz * mpp;
            float hh = r.h[i];
            float dx = (r.h[(size_t)iz * r.n + DG_MIN(x + 1, r.n - 1)] - r.h[(size_t)iz * r.n + DG_MAX(x - 1, 0)]) / (2 * mpp);
            float dz = (r.h[(size_t)DG_MIN(iz + 1, r.n - 1) * r.n + x] - r.h[(size_t)DG_MAX(iz - 1, 0) * r.n + x]) / (2 * mpp);
            vec3 n = v3_norm(v3(-dx, 1, -dz));
            float shade = 0.45f + 0.75f * fmaxf(v3_dot(n, sun), 0.0f);
            vec3 c = v3_scale(mapgen_material_color(r.mat[i], 0.5f, sqrtf(dx * dx + dz * dz)), shade * 1.6f);
            float wl = mapgen_water_level(&g, wx, wz);
            if (wl > hh) {
                float d = saturate((wl - hh) / 8.0f);
                c = v3_lerp(v3_lerp(c, v3(0.15f, 0.3f, 0.35f), 0.6f), v3(0.04f, 0.12f, 0.22f), d);
            }
            if (fabsf(wx) > g.half_size || fabsf(wz) > g.half_size) c = v3_scale(c, 0.6f);
            // contour lines every 20 m
            if (fmodf(fabsf(hh) + 1000.0f, 20.0f) < mpp * 0.35f * (1.0f + sqrtf(dx * dx + dz * dz))) c = v3_scale(c, 0.8f);
            put(img, w, h, x, y, c);
        }
    // bridges (red), keep-outs (outlines), POI centres (crosses)
    for (int b = 0; b < g.bridge_count; b++) {
        const MgBridge *br = &g.bridges[b];
        for (int k = 0; k <= 40; k++) {
            vec2 p = v2_add(br->a, v2_scale(v2_sub(br->b, br->a), (float)k / 40.0f));
            for (int o = -1; o <= 1; o++) put(img, w, h, (int)((p.x - r.origin) / mpp) + o, (int)((p.y - r.origin) / mpp), v3(1, 0.1f, 0.1f));
        }
    }
    mappoi_build(&pois, &g, &r);
    for (int i = 0; i < pois.lot_count; i++) {
        const MpLot *p = &pois.lots[i];
        if (!strcmp(mappoi_lot_name(p->kind), "streetlights") || !strcmp(mappoi_lot_name(p->kind), "yard_clutter") || !strcmp(mappoi_lot_name(p->kind), "logs") ||
            !strcmp(mappoi_lot_name(p->kind), "rocks"))
            continue;
        float cs = cosf(p->yaw), sn = sinf(p->yaw), hx = p->size.x * 0.5f, hz = p->size.y * 0.5f;
        if (!strcmp(mappoi_lot_name(p->kind), "fence")) hz = 0.3f;
        for (int e = 0; e < 4; e++)
            for (int k = 0; k <= 20; k++) {
                float t = (float)k / 20.0f * 2.0f - 1.0f;
                float lx = e < 2 ? t * hx : (e == 2 ? -hx : hx), lz = e < 2 ? (e == 0 ? -hz : hz) : t * hz;
                float px = p->pos.x + lx * cs + lz * sn, pz = p->pos.y - lx * sn + lz * cs;
                put(img, w, h, (int)((px - r.origin) / mpp), (int)((pz - r.origin) / mpp), v3(0.95f, 0.9f, 0.75f));
            }
    }
    for (int i = 0; i < pois.poi_count; i++) {
        const MpPoi *p = &pois.pois[i];
        int cx = (int)((p->center.x - r.origin) / mpp), cy = (int)((p->center.y - r.origin) / mpp);
        for (int k = -6; k <= 6; k++) { put(img, w, h, cx + k, cy, v3(1, 0, 1)); put(img, w, h, cx, cy + k, v3(1, 0, 1)); }
    }
    stbi_write_png(out, w, h, 3, img, w * 3);
    printf("%s: %dx%d px (%.1f m/px), %d roads (%d samples), %d bridges, %d POIs, %d prims, %d entities\n", out, w, h, mpp, g.road_count, g.rs_count, g.bridge_count,
           pois.poi_count, pois.prim_count, pois.ent_count);
    for (int b = 0; b < g.bridge_count; b++) printf("  bridge %d: road '%s' (%.0f, %.0f) - (%.0f, %.0f) deck %.1f m\n", b, g.roads[g.bridges[b].road].name, g.bridges[b].a.x, g.bridges[b].a.y, g.bridges[b].b.x, g.bridges[b].b.y, g.bridges[b].h);
    for (int i = 0; i < pois.poi_count; i++) printf("  poi %-12s %-16s (%5.0f, %5.0f) ground %.1f\n", pois.pois[i].id, pois.pois[i].name, pois.pois[i].center.x, pois.pois[i].center.y, mapgen_raster_height(&r, pois.pois[i].center.x, pois.pois[i].center.y));
    if (argc > 4 && !strcmp(argv[4], "lots"))   // lot list: kind, POI, centre, size, yaw, flags
        for (int i = 0; i < pois.lot_count; i++) {
            const MpLot *l = &pois.lots[i];
            printf("  lot %-14s %-9s (%7.1f, %7.1f) %5.1f x %5.1f yaw %6.1f storeys %2d%s ground %.1f\n", mappoi_lot_name(l->kind), l->poi ? pois.pois[l->poi - 1].id : "-", l->pos.x,
                   l->pos.y, l->size.x, l->size.y, l->yaw * 57.29578f, l->storeys, (l->flags & MPL_ENTERABLE) ? " enterable" : "", mapgen_raster_height(&r, l->pos.x, l->pos.y));
        }
    free(img);
    mapgen_raster_free(&r);
    mappoi_free(&pois);
    mapgen_free(&g);
    cJSON_Delete(root);
    return 0;
}
