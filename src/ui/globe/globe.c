// Maps tab globe (globe.h, docs/MENU_GLOBE.md). The look follows the menu's globe design: a grey
// globe on black (limb darkening, rim rings, stars when far, a graticule fading out as it zooms),
// white vector lines, the region's country / unit dimming, the unit's moving hatch and outline,
// a glow on the place and the lock lines that sweep in from the screen edges and frame it.
// Design units: radii and sizes are pixels at a 900 px tall screen (scaled by gui_h() / 900).
#include "globe.h"
#include "globe_math.h"
#include "ui/gui/gui_draw.h"
#include "ui/gui/gui_theme.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/profiler.h"
#include "cJSON.h"
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REGIONS 8
#define MAX_TILE_SLOTS 160          // grey 512^2 textures with mips (~350 KB each)
#define TILE_LOADS_PER_FRAME 3
#define STAR_COUNT 260
#define PTS_MAX 512

// ------------------------------------------------------------------ files

typedef struct VLayer { char name[40]; u32 kind, first_lod, lod_count, pad; } VLayer;
typedef struct VLod { float max_ppd; u32 first_line, line_count, pad; } VLod;
typedef struct VLine { u32 first_point, count; float w, s, e, n; } VLine;
typedef struct VPoint { u16 x, y; } VPoint;
typedef struct VLabel { char layer[24], code[8], name[40]; float lon, lat; } VLabel;
_Static_assert(sizeof(VLayer) == 56 && sizeof(VLod) == 16 && sizeof(VLine) == 24 && sizeof(VLabel) == 80, "vectors.dgv layout");

typedef struct TileEntry { u32 level, x, y, size; u64 offset; } TileEntry;
_Static_assert(sizeof(TileEntry) == 24, "tiles.dgt index layout");

typedef struct TileSlot {
    int entry;                 // TileEntry index (-1 free)
    TextureId tex;
    u32 used_frame;
} TileSlot;

typedef struct Region {
    char id[24], label[64], unit[16], unit_code[8], unit_name[48], layer[24];
    double ov_lon, ov_lat, ov_r, focus_lon, focus_lat, focus_r, city_r;
    double detail_from;        // radius the detail layer (counties / communes) is fully in by (0: with the admin layer)
    int lay_admin, lay_detail, lay_unit, lay_mask[3];   // vector layers (-1 missing)
} Region;

typedef struct HeightGrid {
    char file[96];
    double w, s, e, n;
    u16 *px;
    int pw, ph;
    bool tried;
} HeightGrid;

static struct {
    bool registered, loaded, ok;
    // tiles
    char pack_path[512];
    TileEntry *tiles;
    int tile_count, max_level;
    int *tile_slot;            // per entry: slot index or -1
    u32 *hash;                 // open addressing: entry index + 1 (0 empty)
    int hash_cap;
    TileSlot slots[MAX_TILE_SLOTS];
    u32 frame;
    int want[64], want_count;
    // vectors
    u8 *vec;
    const VLayer *layers;
    const VLod *lods;
    const VLine *lines;
    const VPoint *points;
    const VLabel *labels;
    int layer_count, label_count;
    int lay_coast, lay_borders, lay_lakes, lay_rivers;
    // regions, heights
    Region regions[MAX_REGIONS];
    int region_count;
    HeightGrid heights[8];
    int height_count;
    // view
    TextureId target;
    int target_w, target_h;
    GlobeView cur;             // lon / lat / radius (design px)
    GlobeFlight fl;
    bool flown;                // a place has been selected once (no idle turning after)
    double dlon, dlat, vlon, vlat;   // drag offset + its spring
    bool dragging, releasing;
    vec2 drag_mouse;
    double drag_lon, drag_lat, last_move_t, release_t;
    // selection
    int sel_region;            // -1 none
    double sel_lon, sel_lat;
    char sel_map[32], sel_place[48];
    bool has_sel;
    float hl, hl_from, pin;
    double sel_t0, hl_t0, arrive_t;
    bool redraw, hl_t0_set, arrived_set;
    int phase;                 // 0 standby, 1 travelling, 2 locked
    double intro_at;           // first open: the flight starts after a beat
    float elev;                // the locked place's height (m)
    bool elev_ok;
    float stars[STAR_COUNT][4];
    u32 last_verts;            // the offscreen layer's vertices last frame (mm_globe_dump)
    float last_ms;             // CPU time of the last globe_draw (tile loads included)
    // cvars
    Cvar *cv_rotate, *cv_texture, *cv_lines, *cv_travel, *cv_detail;
} G;

static vec2 g_pts[PTS_MAX];

// ------------------------------------------------------------------ loading

static u64 tile_key(u32 level, u32 x, u32 y) { return ((u64)level << 56) | ((u64)y << 28) | (u64)x; }

static u32 key_hash(u64 k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (u32)k;
}

static int tile_find(int level, int x, int y) {
    if (!G.hash || level < 0 || level > G.max_level) return -1;
    u64 k = tile_key((u32)level, (u32)x, (u32)y);
    u32 m = (u32)G.hash_cap - 1;
    for (u32 i = key_hash(k) & m;; i = (i + 1) & m) {
        u32 e = G.hash[i];
        if (!e) return -1;
        const TileEntry *t = &G.tiles[e - 1];
        if (tile_key(t->level, t->x, t->y) == k) return (int)e - 1;
    }
}

static bool load_tiles(void) {
    platform_data_path("assets/globe/tiles.dgt", G.pack_path, sizeof(G.pack_path));
    u32 head[8];
    if (!platform_read_file_range(G.pack_path, 0, head, sizeof(head)) || memcmp(head, "DGGT", 4) || head[1] != 1 || head[2] != GLOBE_TILE_SIZE) {
        log_warn("globe: %s missing or not a v1 tile pack (tools/globe/build_globe.py)", G.pack_path);
        return false;
    }
    G.tile_count = (int)head[4];
    G.max_level = (int)head[5];
    G.tiles = (TileEntry *)malloc(sizeof(TileEntry) * (size_t)G.tile_count);
    if (!platform_read_file_range(G.pack_path, 32, G.tiles, sizeof(TileEntry) * (size_t)G.tile_count)) return false;
    G.tile_slot = (int *)malloc(sizeof(int) * (size_t)G.tile_count);
    for (int i = 0; i < G.tile_count; i++) G.tile_slot[i] = -1;
    G.hash_cap = 64;
    while (G.hash_cap < G.tile_count * 2) G.hash_cap <<= 1;
    G.hash = (u32 *)calloc((size_t)G.hash_cap, sizeof(u32));
    u32 m = (u32)G.hash_cap - 1;
    for (int i = 0; i < G.tile_count; i++) {
        u32 h = key_hash(tile_key(G.tiles[i].level, G.tiles[i].x, G.tiles[i].y)) & m;
        while (G.hash[h]) h = (h + 1) & m;
        G.hash[h] = (u32)i + 1;
    }
    for (int i = 0; i < MAX_TILE_SLOTS; i++) G.slots[i].entry = -1;
    return true;
}

static int layer_find(const char *name) {
    for (int i = 0; i < G.layer_count; i++)
        if (!strcmp(G.layers[i].name, name)) return i;
    return -1;
}

static bool load_vectors(void) {
    char path[512];
    size_t size = 0;
    G.vec = (u8 *)platform_read_file(platform_data_path("assets/globe/vectors.dgv", path, sizeof(path)), &size);
    if (!G.vec || size < 36 || memcmp(G.vec, "DGGV", 4)) {
        log_warn("globe: %s missing or invalid", path);
        return false;
    }
    const u32 *h = (const u32 *)G.vec;
    if (h[1] != 1) return false;
    u32 nl = h[2], nlod = h[3], nline = h[4], npt = h[5], nlab = h[6];
    size_t need = 36 + 56 * (size_t)nl + 16 * (size_t)nlod + 24 * (size_t)nline + 4 * (size_t)npt + 80 * (size_t)nlab;
    if (size < need) { log_warn("globe: vectors.dgv truncated"); return false; }
    const u8 *p = G.vec + 36;
    G.layers = (const VLayer *)p;
    p += 56 * (size_t)nl;
    G.lods = (const VLod *)p;
    p += 16 * (size_t)nlod;
    G.lines = (const VLine *)p;
    p += 24 * (size_t)nline;
    G.points = (const VPoint *)p;
    p += 4 * (size_t)npt;
    G.labels = (const VLabel *)p;
    G.layer_count = (int)nl;
    G.label_count = (int)nlab;
    G.lay_coast = layer_find("coast");
    G.lay_borders = layer_find("borders");
    G.lay_lakes = layer_find("lakes");
    G.lay_rivers = layer_find("rivers");
    return true;
}

static double jnum(const cJSON *o, const char *k, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void jstr(const cJSON *o, const char *k, char *dst, size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    dg_strcpy(dst, n, cJSON_IsString(v) ? v->valuestring : "");
}

static void load_regions(void) {
    char path[512], name[64];
    char *text = platform_read_file(platform_data_path("assets/globe/regions.json", path, sizeof(path)), NULL);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *rs = root ? cJSON_GetObjectItemCaseSensitive(root, "regions") : NULL;
    const cJSON *r;
    cJSON_ArrayForEach(r, rs) {
        if (G.region_count >= MAX_REGIONS) break;
        Region *g = &G.regions[G.region_count++];
        memset(g, 0, sizeof(*g));
        dg_strcpy(g->id, sizeof(g->id), r->string);
        jstr(r, "label", g->label, sizeof(g->label));
        jstr(r, "unit", g->unit, sizeof(g->unit));
        jstr(r, "unit_code", g->unit_code, sizeof(g->unit_code));
        jstr(r, "unit_name", g->unit_name, sizeof(g->unit_name));
        jstr(r, "layer", g->layer, sizeof(g->layer));
        const cJSON *ov = cJSON_GetObjectItemCaseSensitive(r, "overview"), *fo = cJSON_GetObjectItemCaseSensitive(r, "focus");
        g->ov_lon = jnum(ov, "lon", 0);
        g->ov_lat = jnum(ov, "lat", 0);
        g->ov_r = jnum(ov, "radius", 1000);
        g->focus_lon = jnum(fo, "lon", g->ov_lon);
        g->focus_lat = jnum(fo, "lat", g->ov_lat);
        g->focus_r = jnum(fo, "radius", g->ov_r * 4);
        g->city_r = jnum(r, "city_radius", g->focus_r * 6);
        g->detail_from = jnum(r, "detail_from", 0.0);
        g->lay_admin = layer_find(g->layer);
        // The admin layer's detail: US states -> counties, judete -> communes.
        g->lay_detail = layer_find(!strcmp(g->layer, "us_states") ? "us_counties" : !strcmp(g->layer, "ro_judete") ? "ro_communes" : "");
        snprintf(name, sizeof(name), "unit:%s", g->id);
        g->lay_unit = layer_find(name);
        static const char *const parts[3] = { "out", "country", "unit" };
        for (int k = 0; k < 3; k++) {
            snprintf(name, sizeof(name), "mask:%s:%s", g->id, parts[k]);
            g->lay_mask[k] = layer_find(name);
        }
    }
    cJSON_Delete(root);
    // Heights (the grids load when first read).
    text = platform_read_file(platform_data_path("assets/globe/heights.json", path, sizeof(path)), NULL);
    root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *gs = root ? cJSON_GetObjectItemCaseSensitive(root, "grids") : NULL;
    cJSON_ArrayForEach(r, gs) {
        if (G.height_count >= (int)(sizeof(G.heights) / sizeof(G.heights[0]))) break;
        HeightGrid *hg = &G.heights[G.height_count++];
        memset(hg, 0, sizeof(*hg));
        jstr(r, "file", hg->file, sizeof(hg->file));
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(r, "bbox");
        if (cJSON_GetArraySize(b) == 4) {
            hg->w = cJSON_GetArrayItem(b, 0)->valuedouble;
            hg->s = cJSON_GetArrayItem(b, 1)->valuedouble;
            hg->e = cJSON_GetArrayItem(b, 2)->valuedouble;
            hg->n = cJSON_GetArrayItem(b, 3)->valuedouble;
        }
    }
    cJSON_Delete(root);
}

static const Region *region_find(const char *id) {
    for (int i = 0; i < G.region_count; i++)
        if (!strcmp(G.regions[i].id, id)) return &G.regions[i];
    return NULL;
}

// The finest height grid holding (lon, lat), bilinear; false without one.
static bool height_at(double lon, double lat, float *out) {
    const HeightGrid *best = NULL;
    for (int i = 0; i < G.height_count; i++) {
        HeightGrid *hg = &G.heights[i];
        if (lon < hg->w || lon > hg->e || lat < hg->s || lat > hg->n) continue;
        if (!hg->tried) {
            hg->tried = true;
            char path[512];
            size_t size = 0;
            void *data = platform_read_file(platform_data_path(hg->file, path, sizeof(path)), &size);
            int c = 0;
            if (data) hg->px = stbi_load_16_from_memory((const stbi_uc *)data, (int)size, &hg->pw, &hg->ph, &c, 1);
            free(data);
        }
        if (!hg->px) continue;
        if (!best || (hg->e - hg->w) / hg->pw < (best->e - best->w) / best->pw) best = hg;
    }
    if (!best) return false;
    double fx = (lon - best->w) / (best->e - best->w) * best->pw - 0.5, fy = (best->n - lat) / (best->n - best->s) * best->ph - 0.5;
    int x0 = DG_CLAMP((int)floor(fx), 0, best->pw - 1), y0 = DG_CLAMP((int)floor(fy), 0, best->ph - 1);
    int x1 = DG_MIN(x0 + 1, best->pw - 1), y1 = DG_MIN(y0 + 1, best->ph - 1);
    double ax = DG_CLAMP(fx - x0, 0.0, 1.0), ay = DG_CLAMP(fy - y0, 0.0, 1.0);
    const u16 *p = best->px;
    double v = (p[y0 * best->pw + x0] * (1 - ax) + p[y0 * best->pw + x1] * ax) * (1 - ay) + (p[y1 * best->pw + x0] * (1 - ax) + p[y1 * best->pw + x1] * ax) * ay;
    *out = (float)(v - 11000.0);
    return true;
}

static void load_all(void) {
    if (G.loaded) return;
    G.loaded = true;
    double t0 = platform_time();
    bool tiles = load_tiles(), vec = load_vectors();
    load_regions();
    G.ok = tiles;
    if (!vec) G.layer_count = 0;
    // The opening view: the Atlantic, the whole globe small (the design's first frame).
    G.cur = (GlobeView){ -40.0, 24.0, 340.0, 0.0, 0.0 };
    // Stars: fixed positions over the design frame (1600 x 900).
    u32 x = 7;
    for (int i = 0; i < STAR_COUNT; i++)
        for (int k = 0; k < 4; k++) {
            x = (x * 9301u + 49297u) % 233280u;
            G.stars[i][k] = (float)x / 233280.0f;
        }
    log_info("globe: %d tiles (levels 0..%d), %d vector layers, %d regions, %.0f ms", G.tile_count, G.max_level, G.layer_count, G.region_count,
             (platform_time() - t0) * 1000.0);
}

// ------------------------------------------------------------------ tile cache

// JPEG -> grey levels (box filtered) -> an R8 texture sampled as grey.
static TextureId tile_upload(int e) {
    const TileEntry *t = &G.tiles[e];
    u8 *jpg = (u8 *)malloc(t->size);
    if (!jpg || !platform_read_file_range(G.pack_path, t->offset, jpg, t->size)) { free(jpg); return 0; }
    int w = 0, h = 0, c = 0;
    u8 *px = stbi_load_from_memory(jpg, (int)t->size, &w, &h, &c, 1);
    free(jpg);
    if (!px || w != GLOBE_TILE_SIZE || h != GLOBE_TILE_SIZE) { stbi_image_free(px); return 0; }
    size_t sizes[16], total = 0;
    int levels = 0;
    for (int s = GLOBE_TILE_SIZE; s >= 1; s >>= 1) { sizes[levels++] = (size_t)s * (size_t)s; total += (size_t)s * (size_t)s; }
    u8 *all = (u8 *)malloc(total);
    memcpy(all, px, sizes[0]);
    stbi_image_free(px);
    u8 *src = all;
    for (int l = 1, s = GLOBE_TILE_SIZE / 2; l < levels; l++, s >>= 1) {
        u8 *dst = src + (size_t)(s * 2) * (size_t)(s * 2);
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                const u8 *a = src + (size_t)(y * 2) * (size_t)(s * 2) + (size_t)x * 2;
                dst[y * s + x] = (u8)((a[0] + a[1] + a[s * 2] + a[s * 2 + 1] + 2) >> 2);
            }
        src = dst;
    }
    TextureId tex = render_texture_create_gray(GLOBE_TILE_SIZE, GLOBE_TILE_SIZE, levels, all, sizes);
    free(all);
    return tex;
}

static bool tile_load(int e) {
    if (G.tile_slot[e] >= 0) return true;
    // A free slot, else the least recently used one not drawn in the last two frames.
    int best = -1;
    for (int i = 0; i < MAX_TILE_SLOTS; i++) {
        TileSlot *s = &G.slots[i];
        if (s->entry < 0) { best = i; break; }
        if (G.frame - s->used_frame < 3 || G.tiles[s->entry].level <= 1) continue;   // levels 0..1 stay (the fallback)
        if (best < 0 || s->used_frame < G.slots[best].used_frame) best = i;
    }
    if (best < 0) return false;
    TextureId tex = tile_upload(e);
    if (!tex) return false;
    TileSlot *s = &G.slots[best];
    if (s->entry >= 0) {
        G.tile_slot[s->entry] = -1;
        render_texture_destroy(s->tex);
    }
    s->entry = e;
    s->tex = tex;
    s->used_frame = G.frame;
    G.tile_slot[e] = best;
    return true;
}

static void want_tile(int e, int level) {
    if (G.tile_slot[e] >= 0) return;
    for (int i = 0; i < G.want_count; i++) if (G.want[i] == e) return;
    if (G.want_count < (int)(sizeof(G.want) / sizeof(G.want[0]))) G.want[G.want_count++] = e;
    else {
        // Full: a coarser tile replaces the finest wanted one (coarse first: they fill the screen).
        int worst = 0;
        for (int i = 1; i < G.want_count; i++) if (G.tiles[G.want[i]].level > G.tiles[G.want[worst]].level) worst = i;
        if ((int)G.tiles[G.want[worst]].level > level) G.want[worst] = e;
    }
}

static void service_loads(void) {
    // Coarse first.
    for (int k = 0; k < TILE_LOADS_PER_FRAME && G.want_count; k++) {
        int bi = 0;
        for (int i = 1; i < G.want_count; i++) if (G.tiles[G.want[i]].level < G.tiles[G.want[bi]].level) bi = i;
        int e = G.want[bi];
        G.want[bi] = G.want[--G.want_count];
        tile_load(e);
    }
    G.want_count = 0;
}

// ------------------------------------------------------------------ projection helpers

typedef struct Proj {
    GlobeBasis b;
    GlobeBox box;
    double ppd;                // pixels per degree at the centre
    float s;                   // design px -> pixels
    float sw, sh;
} Proj;

static inline void proj_pt(const Proj *P, double lon, double lat, float *x, float *y, float *z) {
    double o[3];
    globe_project(&P->b, lon, lat, o);
    *x = (float)o[0];
    *y = (float)o[1];
    *z = (float)o[2];
}

// Behind the limb: pushed out onto it (triangles straddling the horizon end at the rim).
static inline void to_limb(const Proj *P, float *x, float *y) {
    double dx = *x - P->b.cx, dy = *y - P->b.cy, l = sqrt(dx * dx + dy * dy);
    if (l < 1e-6) return;
    *x = (float)(P->b.cx + dx / l * P->b.radius);
    *y = (float)(P->b.cy + dy / l * P->b.radius);
}

static u32 grey(float v, float a) {
    int g = DG_CLAMP((int)(v * 255.0f + 0.5f), 0, 255), al = DG_CLAMP((int)(a * 255.0f + 0.5f), 0, 255);
    return UI_RGBA(g, g, g, al);
}

static u32 white(float a) { return grey(1.0f, a); }

// ------------------------------------------------------------------ texture

static void draw_tile(const Proj *P, int level, int x, int y, float opacity) {
    // The tile's own texture, else the nearest loaded ancestor (a sub-rect of it).
    int e = tile_find(level, x, y);
    if (e >= 0) want_tile(e, level);
    int al = level, ax = x, ay = y, ae = e;
    while (al >= 0 && (ae < 0 || G.tile_slot[ae] < 0)) {
        al--;
        ax >>= 1;
        ay >>= 1;
        ae = al >= 0 ? tile_find(al, ax, ay) : -1;
        if (ae >= 0 && G.tile_slot[ae] < 0) want_tile(ae, al);
    }
    if (al < 0 || ae < 0) return;
    TileSlot *slot = &G.slots[G.tile_slot[ae]];
    slot->used_frame = G.frame;
    double w, s, e2, n, aw, as, ae2, an;
    globe_tile_bounds(level, x, y, &w, &s, &e2, &n);
    globe_tile_bounds(al, ax, ay, &aw, &as, &ae2, &an);
    double deg = e2 - w, adeg = ae2 - aw;
    int N = DG_CLAMP((int)ceil(deg / 2.8), 4, 32);
    UiVertex v[33 * 33];
    u32 idx[32 * 32 * 6];
    float zs[33 * 33];
    int nv = 0;
    for (int j = 0; j <= N; j++)
        for (int i = 0; i <= N; i++) {
            double lon = w + deg * i / N, lat = n - deg * j / N;
            float px, py, pz;
            proj_pt(P, lon, lat, &px, &py, &pz);
            zs[nv] = pz;
            if (pz <= 0) to_limb(P, &px, &py);
            float shade = 0.45f + 0.55f * powf(fmaxf(pz, 0.0f), 0.6f);   // limb darkening: it reads as a sphere
            float u = (float)((1.0 + (lon - aw) / adeg * GLOBE_TILE_INNER) / GLOBE_TILE_SIZE);
            float t = (float)((1.0 + (an - lat) / adeg * GLOBE_TILE_INNER) / GLOBE_TILE_SIZE);
            v[nv++] = (UiVertex){ v2(px, py), v2(u, t), grey(shade * opacity, 1.0f) };
        }
    int ni = 0;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            u32 a = (u32)(j * (N + 1) + i), b = a + 1, c = a + (u32)(N + 1), d = c + 1;
            if (zs[a] <= 0 && zs[b] <= 0 && zs[c] <= 0 && zs[d] <= 0) continue;
            idx[ni++] = a; idx[ni++] = b; idx[ni++] = d;
            idx[ni++] = a; idx[ni++] = d; idx[ni++] = c;
        }
    if (ni) gd_tris(slot->tex, v, nv, idx, ni);
}

static void visit_tile(const Proj *P, int level, int x, int y, int target, float opacity) {
    double w, s, e, n;
    globe_tile_bounds(level, x, y, &w, &s, &e, &n);
    if (!globe_box_hits(&P->box, w, s, e, n)) return;
    // Front-facing at all? (a tile wholly behind the limb draws nothing)
    if (level < target) {
        bool kids = false;
        for (int k = 0; k < 4 && !kids; k++) kids = tile_find(level + 1, x * 2 + (k & 1), y * 2 + (k >> 1)) >= 0;
        if (kids) {
            for (int k = 0; k < 4; k++) visit_tile(P, level + 1, x * 2 + (k & 1), y * 2 + (k >> 1), target, opacity);
            return;
        }
    }
    draw_tile(P, level, x, y, opacity);
}

static void draw_texture(const Proj *P, float opacity) {
    if (!G.ok || opacity <= 0) return;
    int target = globe_level_for(P->ppd * 0.9, G.max_level);
    visit_tile(P, 0, 0, 0, target, opacity);
    visit_tile(P, 0, 1, 0, target, opacity);
}

// ------------------------------------------------------------------ vectors

static const VLod *layer_lod(int layer, double ppd) {
    if (layer < 0 || layer >= G.layer_count) return NULL;
    const VLayer *L = &G.layers[layer];
    if (!L->lod_count) return NULL;
    for (u32 k = 0; k < L->lod_count; k++) {
        const VLod *d = &G.lods[L->first_lod + k];
        if (d->max_ppd >= ppd) return d;
    }
    return &G.lods[L->first_lod + L->lod_count - 1];
}

static inline void line_pt(const VLine *ln, const VPoint *p, double *lon, double *lat) {
    *lon = ln->w + (double)p->x * (1.0 / 65535.0) * (ln->e - ln->w);
    *lat = ln->s + (double)p->y * (1.0 / 65535.0) * (ln->n - ln->s);
}

// One chunk: front (z > 0) or back runs, points closer than ~1.2 px merged; `limit` points at most.
static void draw_chunk(const Proj *P, const VLine *ln, bool front, float width, u32 c, u32 limit) {
    int n = 0;
    float lx = 0, ly = 0;
    bool pend = false;
    vec2 pv = v2(0, 0);
    u32 count = DG_MIN(ln->count, limit);
    for (u32 i = 0; i < count; i++) {
        double lon, lat;
        line_pt(ln, &G.points[ln->first_point + i], &lon, &lat);
        float x, y, z;
        proj_pt(P, lon, lat, &x, &y, &z);
        bool vis = front ? z > 0 : z <= 0;
        if (!vis) {
            if (pend && n < PTS_MAX) g_pts[n++] = pv;
            if (n >= 2) gd_polyline(g_pts, n, width, c, false);
            n = 0;
            pend = false;
            continue;
        }
        if (n > 0 && (x - lx) * (x - lx) + (y - ly) * (y - ly) < 1.44f && i + 1 < count) {
            pend = true;
            pv = v2(x, y);
            continue;
        }
        if (n >= PTS_MAX) {
            gd_polyline(g_pts, n, width, c, false);
            g_pts[0] = g_pts[n - 1];
            n = 1;
        }
        g_pts[n++] = v2(x, y);
        lx = x;
        ly = y;
        pend = false;
    }
    if (pend && n < PTS_MAX) g_pts[n++] = pv;
    if (n >= 2) gd_polyline(g_pts, n, width, c, false);
}

static void draw_layer(const Proj *P, int layer, bool front, float width, u32 c) {
    if ((c >> 24) == 0) return;
    const VLod *d = layer_lod(layer, P->ppd);
    if (!d) return;
    for (u32 i = 0; i < d->line_count; i++) {
        const VLine *ln = &G.lines[d->first_line + i];
        if (front && !globe_box_hits(&P->box, ln->w, ln->s, ln->e, ln->n)) continue;
        draw_chunk(P, ln, front, width, c, ln->count);
    }
}

// A unit outline drawn in: the first frac of its points (chunks in order).
static void draw_layer_frac(const Proj *P, int layer, float width, u32 c, float frac) {
    const VLod *d = layer_lod(layer, P->ppd);
    if (!d || frac <= 0) return;
    u32 total = 0;
    for (u32 i = 0; i < d->line_count; i++) total += G.lines[d->first_line + i].count;
    u32 left = (u32)(total * fminf(frac, 1.0f));
    for (u32 i = 0; i < d->line_count && left > 1; i++) {
        const VLine *ln = &G.lines[d->first_line + i];
        u32 k = DG_MIN(ln->count, left);
        draw_chunk(P, ln, true, width, c, k);
        left -= k;
    }
}

// Triangles of a mask layer in one colour (the dimming).
static void draw_tris(const Proj *P, int layer, u32 c) {
    if ((c >> 24) == 0 || layer < 0 || layer >= G.layer_count || G.layers[layer].kind != 1) return;
    const VLod *d = &G.lods[G.layers[layer].first_lod];
    vec2 uv = gd_white_uv();
    static UiVertex v[3 * 1024];
    static u32 idx[3 * 1024];
    for (u32 i = 0; i < d->line_count; i++) {
        const VLine *ln = &G.lines[d->first_line + i];
        if (!globe_box_hits(&P->box, ln->w, ln->s, ln->e, ln->n)) continue;
        int nv = 0;
        for (u32 t = 0; t + 2 < ln->count; t += 3) {
            float x[3], y[3], z[3];
            for (int k = 0; k < 3; k++) {
                double lon, lat;
                line_pt(ln, &G.points[ln->first_point + t + (u32)k], &lon, &lat);
                proj_pt(P, lon, lat, &x[k], &y[k], &z[k]);
            }
            if (z[0] <= 0 && z[1] <= 0 && z[2] <= 0) continue;
            for (int k = 0; k < 3; k++) {
                if (z[k] <= 0) to_limb(P, &x[k], &y[k]);
                idx[nv] = (u32)nv;
                v[nv++] = (UiVertex){ v2(x[k], y[k]), uv, c };
            }
            if (nv >= 3 * 1024) { gd_tris(gd_atlas(), v, nv, idx, nv); nv = 0; }
        }
        if (nv) gd_tris(gd_atlas(), v, nv, idx, nv);
    }
}

// The unit's moving hatch: diagonal screen lines (down-right) cut to the unit by its outline's
// crossings (even-odd), only over the outline's screen box.
static void draw_hatch(const Proj *P, int layer, float step, float off, u32 c) {
    const VLod *d = layer_lod(layer, P->ppd);
    if (!d) return;
    // Projected outline segments (front only) and their box.
    static float seg[8192][4];
    int ns = 0;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (u32 i = 0; i < d->line_count && ns < 8192; i++) {
        const VLine *ln = &G.lines[d->first_line + i];
        float px = 0, py = 0, pz = 0;
        for (u32 k = 0; k < ln->count && ns < 8192; k++) {
            double lon, lat;
            line_pt(ln, &G.points[ln->first_point + k], &lon, &lat);
            float x, y, z;
            proj_pt(P, lon, lat, &x, &y, &z);
            if (k && z > 0 && pz > 0) {
                seg[ns][0] = px; seg[ns][1] = py; seg[ns][2] = x; seg[ns][3] = y;
                ns++;
                bx0 = fminf(bx0, x); by0 = fminf(by0, y); bx1 = fmaxf(bx1, x); by1 = fmaxf(by1, y);
            }
            px = x; py = y; pz = z;
        }
    }
    if (ns < 3) return;
    bx0 = fmaxf(bx0, -P->sh); by0 = fmaxf(by0, 0); bx1 = fminf(bx1, P->sw + P->sh); by1 = fminf(by1, P->sh);
    if (bx1 <= bx0 || by1 <= by0) return;
    // Line k: x - y = c0 + k * step (points (c + t, t)).
    float cmin = bx0 - by1, cmax = bx1 - by0;
    float c0 = floorf((cmin - off) / step) * step + off;
    static float ts[512];
    for (float cc = c0; cc <= cmax; cc += step) {
        int nt = 0;
        for (int i = 0; i < ns && nt < 512; i++) {
            float x0 = seg[i][0] - seg[i][1], x1 = seg[i][2] - seg[i][3];   // x - y at the ends
            if ((x0 < cc) == (x1 < cc)) continue;
            float u = (cc - x0) / (x1 - x0);
            ts[nt++] = seg[i][1] + (seg[i][3] - seg[i][1]) * u;              // t = y at the crossing
        }
        if (nt < 2) continue;
        for (int i = 1; i < nt; i++) {   // insertion sort (few crossings per line)
            float t = ts[i];
            int j = i - 1;
            while (j >= 0 && ts[j] > t) { ts[j + 1] = ts[j]; j--; }
            ts[j + 1] = t;
        }
        for (int i = 0; i + 1 < nt; i += 2) gd_line(cc + ts[i], ts[i], cc + ts[i + 1], ts[i + 1], 1.0f, c);
    }
}

static void draw_graticule(const Proj *P, float width, u32 c) {
    for (int lo = -180; lo < 180; lo += 15) {
        int n = 0;
        for (int la = -90; la <= 90; la += 3) {
            float x, y, z;
            proj_pt(P, lo, la, &x, &y, &z);
            if (z <= 0) { if (n >= 2) gd_polyline(g_pts, n, width, c, false); n = 0; continue; }
            g_pts[n++] = v2(x, y);
        }
        if (n >= 2) gd_polyline(g_pts, n, width, c, false);
    }
    for (int la = -75; la <= 75; la += 15) {
        int n = 0;
        for (int lo = -180; lo <= 180; lo += 3) {
            float x, y, z;
            proj_pt(P, lo, la, &x, &y, &z);
            if (z <= 0) { if (n >= 2) gd_polyline(g_pts, n, width, c, false); n = 0; continue; }
            g_pts[n++] = v2(x, y);
        }
        if (n >= 2) gd_polyline(g_pts, n, width, c, false);
    }
}

static void draw_circle(float cx, float cy, float r, float width, u32 c, float sw, float sh) {
    if (r > (sw + sh) * 4.0f) return;   // the rim is far off screen
    int n = DG_CLAMP((int)(r * 0.5f), 64, PTS_MAX);
    for (int k = 0; k < n; k++) {
        float a = 6.2831853f * (float)k / (float)n;
        g_pts[k] = v2(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    gd_polyline(g_pts, n, width, c, true);
}

// Radial glow: white 0.95 at the centre, 0.45 at 35 %, 0 at the edge - a fan of rings with the
// alpha in the vertex colours (smooth, no banding).
static void draw_glow(float x, float y, float r, float k) {
    enum { SEG = 64, RINGS = 6 };
    static const float rt[RINGS] = { 0.0f, 0.12f, 0.35f, 0.55f, 0.78f, 1.0f };
    UiVertex v[RINGS * SEG];
    u32 idx[(RINGS - 1) * SEG * 6];
    vec2 uv = gd_white_uv();
    int nv = 0, ni = 0;
    for (int ri = 0; ri < RINGS; ri++) {
        float t = rt[ri];
        float g = t < 0.35f ? 0.95f - 0.5f * t / 0.35f : 0.45f * (1.0f - (t - 0.35f) / 0.65f);
        g *= g > 0.0f ? 1.0f : 0.0f;
        // A softer falloff than linear between the stops (reads as light, like the design's additive glow).
        if (t > 0.35f) g *= 1.0f - 0.35f * (t - 0.35f) / 0.65f;
        for (int k2 = 0; k2 < SEG; k2++) {
            float a = 6.2831853f * (float)k2 / (float)SEG;
            v[nv++] = (UiVertex){ v2(x + cosf(a) * r * t, y + sinf(a) * r * t), uv, white(g * k) };
        }
    }
    for (int ri = 0; ri + 1 < RINGS; ri++)
        for (int k2 = 0; k2 < SEG; k2++) {
            u32 a = (u32)(ri * SEG + k2), b = (u32)(ri * SEG + (k2 + 1) % SEG), c = a + SEG, d = b + SEG;
            idx[ni++] = a; idx[ni++] = c; idx[ni++] = d;
            idx[ni++] = a; idx[ni++] = d; idx[ni++] = b;
        }
    gd_tris(gd_atlas(), v, nv, idx, ni);
}

// ------------------------------------------------------------------ selection / camera

void globe_select(const MapInfo *m) {
    load_all();
    bool has = m && m->has_geo;
    if (!has) {
        G.has_sel = false;
        G.sel_region = -1;
        G.sel_map[0] = 0;
        G.phase = 0;
        G.fl.active = false;
        return;
    }
    if (G.has_sel && !strcmp(G.sel_map, m->id)) return;
    const Region *reg = region_find(m->geo_region);
    int ri = reg ? (int)(reg - G.regions) : -1;
    bool same = G.has_sel && ri >= 0 && ri == G.sel_region;
    double now = gui_time();
    float travel = G.cv_travel ? G.cv_travel->f : 1.8f;
    GlobeLeg legs[3];
    int n = 0;
    if (same) legs[n++] = (GlobeLeg){ m->geo_lon, m->geo_lat, reg->city_r, 1.1f, 0.35f, 0.0f };
    else if (reg) {
        // Into a region: an overview of its whole admin layer, the unit, then down onto the place.
        legs[n++] = (GlobeLeg){ reg->ov_lon, reg->ov_lat, reg->ov_r, travel, 1.0f, 0.9f };
        legs[n++] = (GlobeLeg){ reg->focus_lon, reg->focus_lat, reg->focus_r, 1.3f, 0.0f, 0.7f };
        legs[n++] = (GlobeLeg){ m->geo_lon, m->geo_lat, reg->city_r, 1.4f, 0.0f, 0.0f };
    } else legs[n++] = (GlobeLeg){ m->geo_lon, m->geo_lat, 1400.0, travel, 1.0f, 0.0f };
    // First open: a beat on the whole globe before the flight (the design's intro).
    double start = G.flown ? now : now + 0.9;
    globe_flight_start(&G.fl, &G.cur, legs, n, start);
    G.flown = true;
    G.sel_region = ri;
    G.sel_lon = m->geo_lon;
    G.sel_lat = m->geo_lat;
    dg_strcpy(G.sel_map, sizeof(G.sel_map), m->id);
    dg_strcpy(G.sel_place, sizeof(G.sel_place), m->geo_place);
    G.has_sel = true;
    G.redraw = !same;
    G.hl_from = same ? 1.0f : G.hl;
    G.pin = 0;
    G.sel_t0 = start;
    G.hl_t0_set = false;
    G.arrived_set = false;
    G.phase = 1;
    G.elev_ok = height_at(m->geo_lon, m->geo_lat, &G.elev);
}

static void cmd_finish(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    // Jump to the end of the flight (scripted screenshots).
    if (!G.fl.active) return;
    const GlobeLeg *l = &G.fl.legs[G.fl.count - 1];
    G.cur.lon = l->lon;
    G.cur.lat = l->lat;
    G.cur.radius = l->radius;
    G.fl.leg = G.fl.count - 1;
    G.fl.from = G.cur;
    G.fl.t0 = gui_time() - l->dur - 0.75;
    G.hl = 1.0f;
    G.hl_t0_set = true;
    G.hl_t0 = gui_time() - 1.0;
}

static void update_camera(double now) {
    if (G.has_sel && G.fl.count) {   // flying, or arrived (the last leg's values hold)
        GlobeFlight *f = &G.fl;
        if (now < f->t0) return;   // the intro beat
        int leg = f->leg;
        const GlobeLeg *g = &f->legs[leg];
        double t0 = f->t0;
        bool last = globe_flight_last_leg(f);
        float t = f->active ? globe_flight_update(f, &G.cur, now) : 1.0f;
        double el = now - t0;
        float fade = G.hl_from * saturate(1.0f - (float)(now - G.sel_t0) / 0.3f);
        bool hl_leg = leg >= f->count - 2;   // the unit's outline draws in on the unit leg
        if (hl_leg && G.redraw) {
            if (!G.hl_t0_set) { G.hl_t0 = t0 + g->dur * 0.6; G.hl_t0_set = true; }
            G.hl = fade + (1.0f - fade) * saturate((float)(now - G.hl_t0) / 0.9f);
        } else G.hl = hl_leg ? 1.0f : fade;
        if (last) {
            G.pin = saturate((float)(el - g->dur * 0.8) / 0.7f);
            if (t >= 1.0f && !G.arrived_set) { G.arrived_set = true; G.arrive_t = now; }
            if (G.arrived_set && now > G.arrive_t + 0.7 && G.phase == 1) G.phase = 2;
        } else G.pin = 0;
    } else if (!G.flown && (!G.cv_rotate || G.cv_rotate->i)) {
        G.cur.lon += 0.08 * 60.0 * gui_dt();
    }
}

static void update_drag(GuiRect input, double now) {
    GuiInteract in = gui_interact(gui_id("mm_globe_drag"), input, GUI_NO_HOVER_SOUND);
    vec2 m = gui_mouse();
    double k = 57.3 / G.cur.radius;
    if (in.pressed) {
        G.dragging = true;
        G.releasing = false;
        G.drag_mouse = m;
        G.drag_lon = G.dlon;
        G.drag_lat = G.dlat;
        G.last_move_t = now;
    }
    float s = gui_h() / 900.0f;
    if (G.dragging) {
        if (!gui_mouse_down(0)) {
            G.dragging = false;
            G.releasing = true;
            G.release_t = now;
        } else {
            // Pan clamp: ~260 design px of drift, then rubber-band resistance past it.
            double lim = fmin(260.0, 0.75 * G.cur.radius) * k;
            double nlon = G.drag_lon - (m.x - G.drag_mouse.x) / s * k, nlat = G.drag_lat + (m.y - G.drag_mouse.y) / s * k;
            double soft[2] = { nlon, nlat };
            for (int i = 0; i < 2; i++) {
                double a = fabs(soft[i]);
                if (a > lim) soft[i] = (soft[i] < 0 ? -1 : 1) * (lim + lim * 0.45 * tanh((a - lim) / lim));
            }
            double dt = fmax(1e-3, now - G.last_move_t);
            if (gui_mouse_moved()) {
                G.vlon = (soft[0] - G.dlon) / dt;
                G.vlat = (soft[1] - G.dlat) / dt;
                G.last_move_t = now;
            }
            G.dlon = soft[0];
            G.dlat = soft[1];
        }
    }
    if (G.releasing) {
        // Damped spring back (keeps the fling: a flick overshoots a touch and settles).
        double dt = fmin(0.05, now - G.release_t);
        G.release_t = now;
        const double K = 90.0, C = 2.0 * sqrt(K) * 0.72;
        double vcap = 600.0 * 57.3 / G.cur.radius;
        G.vlon = DG_CLAMP(G.vlon, -vcap, vcap);
        G.vlat = DG_CLAMP(G.vlat, -vcap, vcap);
        G.vlon += (-K * G.dlon - C * G.vlon) * dt;
        G.dlon += G.vlon * dt;
        G.vlat += (-K * G.dlat - C * G.vlat) * dt;
        G.dlat += G.vlat * dt;
        double px = G.cur.radius / 57.3;
        if (fabs(G.dlon) * px < 0.2 && fabs(G.dlat) * px < 0.2 && fabs(G.vlon) * px < 2 && fabs(G.vlat) * px < 2) {
            G.dlon = G.dlat = G.vlon = G.vlat = 0;
            G.releasing = false;
        }
    }
}

// ------------------------------------------------------------------ draw

static void draw_lock(const Proj *P, float now) {
    if (!G.has_sel || G.pin <= 0) return;
    float x, y, z;
    proj_pt(P, G.sel_lon, G.sel_lat, &x, &y, &z);
    if (z <= 0) return;
    float s = P->s, W = P->sw, H = P->sh;
    float e = globe_ease_out(G.pin), a = saturate(G.pin * 3.0f);
    // The square shrinks onto the place as it locks.
    float half = (DG_CLAMP((float)G.cur.radius * 0.0009f, 26.0f, 90.0f) + 90.0f * (1.0f - e)) * s;
    u32 c = white(a * 0.85f);
    // Horizontal lines grow in from the left and right edges, vertical ones from the top and bottom.
    gd_line(0, y - half, W * e, y - half, 1.0f, c);
    gd_line(W, y + half, W - W * e, y + half, 1.0f, c);
    gd_line(x - half, 0, x - half, H * e, 1.0f, c);
    gd_line(x + half, H, x + half, H - H * e, 1.0f, c);
    // The square once the lines meet, the dot, the name and the coordinates.
    float t = fmaxf(1.0f, 1.5f * s);
    u32 cs = white(a * e);
    gd_line(x - half, y - half, x + half, y - half, t, cs);
    gd_line(x + half, y - half, x + half, y + half, t, cs);
    gd_line(x + half, y + half, x - half, y + half, t, cs);
    gd_line(x - half, y + half, x - half, y - half, t, cs);
    gd_circle(x, y, 2.5f * s, cs);
    const GuiTheme *th = gui_theme();
    GuiText nt = { 14.0f * s * 1.2f, GW_SEMIBOLD, cs, GUI_LEFT, th->tracking_caps, true, true, 0 };
    gd_text(&nt, x + half + 10.0f * s, y - half - 4.0f * s - gd_line_height(nt.size), G.sel_place);
    char co[96];
    int n = snprintf(co, sizeof(co), "%.2f%s %.2f%s", fabs(G.sel_lat), G.sel_lat >= 0 ? "N" : "S", fabs(G.sel_lon), G.sel_lon >= 0 ? "E" : "W");
    if (G.elev_ok && n > 0) snprintf(co + n, sizeof(co) - (size_t)n, "  ·  %.0f m", (double)G.elev);
    GuiText ct = { 11.0f * s * 1.2f, GW_MEDIUM, white(a * e * 0.7f), GUI_LEFT, th->tracking_caps * 0.6f, true, true, 0 };
    gd_text(&ct, x + half + 10.0f * s, y + half + 14.0f * s - gd_line_height(ct.size), co);
    (void)now;
}

static void draw_focus(const Proj *P, const Region *reg, float k, double now) {
    if (!reg || k <= 0) return;
    // Dim the world, the country a little less, the unit least - then light the place.
    draw_tris(P, reg->lay_mask[0], grey(0, 0.62f * k));
    draw_tris(P, reg->lay_mask[1], grey(0, 0.30f * k));
    draw_tris(P, reg->lay_mask[2], grey(0, 0.08f * k));
    float x, y, z;
    proj_pt(P, G.sel_lon, G.sel_lat, &x, &y, &z);
    if (z > 0) {
        float r = DG_CLAMP((float)G.cur.radius * 0.0045f, 26.0f, 260.0f) * (1.0f + 0.06f * sinf((float)(now / 0.42))) * P->s;
        // Eases to 40 % once locked: the place's imagery shows under it.
        draw_glow(x, y, r, k * (1.0f - 0.6f * globe_ease_out(G.pin)));
    }
}

static void draw_unit_label(const Proj *P, const Region *reg, float lk) {
    if (lk <= 0) return;
    for (int i = 0; i < G.label_count; i++) {
        const VLabel *L = &G.labels[i];
        if (strcmp(L->layer, reg->layer) || strcmp(L->code, reg->unit_code)) continue;
        float x, y, z;
        proj_pt(P, L->lon, L->lat, &x, &y, &z);
        if (z <= 0) return;
        const char *text = G.cur.radius > reg->ov_r * 2.2 ? (reg->unit_name[0] ? reg->unit_name : L->name) : L->code;
        GuiText t = { 13.0f * P->s * 1.2f, GW_SEMIBOLD, white(lk), GUI_CENTER, 0.35f, true, true, 0 };
        gd_text(&t, x, y - gd_line_height(t.size) * 0.5f, text);
        return;
    }
}

void globe_draw(float cx, float cy, GuiRect input, float alpha) {
    PROF_BEGIN("ui:globe");
    double t_start = platform_time();
    load_all();
    G.frame++;
    float sw = gui_w(), sh = gui_h();
    if (sw < 2 || sh < 2) { PROF_END(); return; }
    // The offscreen target (screen sized).
    if (!G.target || G.target_w != (int)sw || G.target_h != (int)sh) {
        if (G.target) render_ui3d_target_destroy(G.target);
        G.target = render_ui3d_target_create((int)sw, (int)sh);
        G.target_w = (int)sw;
        G.target_h = (int)sh;
    }
    if (!G.target) { PROF_END(); return; }
    double now = gui_time();
    update_drag(input, now);
    update_camera(now);
    float s = sh / 900.0f;
    const Region *reg = G.has_sel && G.sel_region >= 0 ? &G.regions[G.sel_region] : NULL;
    double breathe = G.phase == 2 ? 1.0 + 0.004 * sin(now / 0.9) : 1.0;
    GlobeView view = { G.cur.lon + G.dlon, DG_CLAMP(G.cur.lat + G.dlat, -85.0, 85.0), G.cur.radius * breathe * s, cx, cy };
    Proj P;
    globe_basis(&view, &P.b);
    P.sw = sw;
    P.sh = sh;
    P.s = s;
    P.ppd = globe_px_per_deg(&view);
    P.box = globe_visible_box(&P.b, sw, sh);
    double R = G.cur.radius * breathe;   // design px
    // Admin layer visibility: fully on by the region's overview radius.
    double on_r = reg ? reg->ov_r : 2000.0;
    float zoom_k = saturate((float)((R - on_r * 0.45) / (on_r * 0.5)));   // 0 = globe, 1 = states / judete
    float far = saturate((float)((1600.0 - R) / 1200.0));                 // 1 = far away
    float lw = G.cv_lines ? G.cv_lines->f : 1.0f;
    float lwp = lw * fmaxf(1.0f, s);
    float rr = (float)view.radius;

    gd_offscreen_begin(G.target, UI_RGBA(0, 0, 0, 255));
    // Stars, far out.
    for (int i = 0; far > 0 && i < STAR_COUNT; i++) gd_circle(G.stars[i][0] * sw, G.stars[i][1] * sh, (0.4f + G.stars[i][2] * 1.2f) * s, white((0.25f + G.stars[i][3] * 0.6f) * far));
    if (rr < (sw + sh) * 4.0f) gd_circle(cx, cy, rr, UI_RGBA(0, 0, 0, 255));
    draw_texture(&P, G.cv_texture ? G.cv_texture->f : 0.55f);
    if (reg) draw_focus(&P, reg, G.hl, now);
    draw_circle(cx, cy, rr, 1.5f * lwp, white(0.6f), sw, sh);
    draw_circle(cx, cy, rr + 14.0f * s, 1.0f, white(0.15f), sw, sh);
    if (G.layer_count) {
        // The back hemisphere, faint - sells the sphere.
        if (far > 0) {
            draw_layer(&P, G.lay_coast, false, lwp, white(0.09f * far));
            draw_layer(&P, G.lay_borders, false, lwp, white(0.09f * far));
        }
        if (zoom_k < 1) draw_graticule(&P, 0.8f * lwp, white(0.16f * (1.0f - zoom_k)));
        // Water detail: lakes and rivers come in as the globe grows.
        float wd = (G.cv_detail ? G.cv_detail->f : 1.0f) * saturate((float)(P.ppd - 4.0) / 10.0f);
        draw_layer(&P, G.lay_rivers, true, 0.8f * lwp, white(0.16f * wd));
        draw_layer(&P, G.lay_lakes, true, 0.8f * lwp, white(0.3f * wd));
        // Coasts and borders, crossfading into the region's admin layer.
        u32 wc = white(0.8f * (1.0f - zoom_k) + 0.15f);
        draw_layer(&P, G.lay_coast, true, lwp, wc);
        draw_layer(&P, G.lay_borders, true, lwp, wc);
        if (zoom_k > 0 && reg) {
            float dk = reg->detail_from > 0 ? saturate((float)((R - reg->detail_from * 0.5) / (reg->detail_from * 0.5))) : 1.0f;
            draw_layer(&P, reg->lay_detail, true, lwp, white(0.38f * zoom_k * dk * (G.cv_detail ? G.cv_detail->f : 1.0f)));
            draw_layer(&P, reg->lay_admin, true, 1.1f * lwp, white(0.95f * zoom_k));
            draw_unit_label(&P, reg, saturate((zoom_k - 0.5f) * 2.0f));
        }
        // The selected unit: hatched + its outline drawn in.
        if (reg && G.hl > 0) {
            float hatch_k = saturate((float)(1.0 - (R - reg->focus_r) / (reg->city_r - reg->focus_r)));   // thins out onto the place
            float step = 9.0f * s, off = fmodf((float)(now / 0.06), 9.0f) * s;
            draw_hatch(&P, reg->lay_unit, step, off, white(0.22f * G.hl * (0.25f + 0.75f * hatch_k)));
            draw_layer_frac(&P, reg->lay_unit, 2.2f * lwp, white(G.hl), G.hl);
        }
    } else {
        GuiText t = { gui_s(14), GW_MEDIUM, white(0.5f), GUI_CENTER, 0.2f, true, false, 0 };
        gd_text(&t, cx, cy, "Globe data missing");
    }
    draw_lock(&P, (float)now);
    G.last_verts = gd_layer_verts(GL_OFFSCREEN);
    gd_offscreen_end();
    service_loads();
    // On screen: the offscreen image (the panels drawn after it blur it).
    gd_alpha_push(alpha);
    gd_image(gui_rect(0, 0, sw, sh), G.target, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
    gd_alpha_pop();
    G.last_ms = (float)((platform_time() - t_start) * 1000.0);
    PROF_END();
}

const char *globe_phase_text(void) {
    return G.phase == 1 ? "Travelling" : G.phase == 2 ? "Locked" : "Standby";
}

const char *globe_credits(void) {
    return "Imagery: Copernicus Sentinel-2 (modified), NASA Blue Marble, Natural Earth  ·  Height: AWS Terrain Tiles (SRTM, ETOPO1, NED)  ·  Boundaries: Natural Earth, "
           "geoBoundaries (CC BY 4.0)";
}

// ------------------------------------------------------------------ lifecycle

static void cmd_dump(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    int loaded = 0;
    for (int i = 0; i < MAX_TILE_SLOTS; i++) loaded += G.slots[i].entry >= 0;
    log_info("mm_globe_dump: ok %d phase %s map %s lon %.4f lat %.4f radius %.1f hl %.2f pin %.2f tiles %d/%d levels 0..%d layers %d elev %.0f",
             G.ok ? 1 : 0, globe_phase_text(), G.sel_map, G.cur.lon, G.cur.lat, G.cur.radius, (double)G.hl, (double)G.pin, loaded, G.tile_count, G.max_level,
             G.layer_count, G.elev_ok ? (double)G.elev : -99999.0);
    const Region *r = G.sel_region >= 0 ? &G.regions[G.sel_region] : NULL;
    if (r)
        log_info("mm_globe_dump: region %s layers admin %d detail %d unit %d masks %d %d %d, offscreen %u / %u verts, %.2f ms", r->id, r->lay_admin, r->lay_detail, r->lay_unit,
                 r->lay_mask[0], r->lay_mask[1], r->lay_mask[2], G.last_verts, gd_layer_verts_cap(GL_OFFSCREEN), (double)G.last_ms);
}

void globe_register(void) {
    if (G.registered) return;
    G.registered = true;
    G.sel_region = -1;
    G.cv_rotate = cvar_reg_bool("mm_globe_rotate", true, CVAR_ARCHIVE | CVAR_EDITABLE, "Maps tab globe: turn slowly until a map is picked");
    G.cv_texture = cvar_reg_float("mm_globe_texture", 0.55f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "Maps tab globe: texture opacity (grey satellite / relief)");
    G.cv_lines = cvar_reg_float("mm_globe_lines", 1.0f, 0.5f, 2.5f, CVAR_ARCHIVE | CVAR_EDITABLE, "Maps tab globe: line weight (coasts, borders, outlines)");
    G.cv_travel = cvar_reg_float("mm_globe_travel", 1.8f, 0.6f, 4.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "Maps tab globe: seconds of the flight's first leg (into a region)");
    G.cv_detail = cvar_reg_float("mm_globe_detail", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "Maps tab globe: lakes, rivers, counties / communes (0 off .. 1)");
    cmd_register("mm_globe_dump", cmd_dump, "log the Maps tab globe's state (scripted tests)");
    cmd_register("mm_globe_finish", cmd_finish, "Maps tab globe: jump to the end of the current flight (scripted screenshots)");
}

void globe_shutdown(void) {
    for (int i = 0; i < MAX_TILE_SLOTS; i++)
        if (G.slots[i].entry >= 0) render_texture_destroy(G.slots[i].tex);
    if (G.target) render_ui3d_target_destroy(G.target);
    for (int i = 0; i < G.height_count; i++) stbi_image_free(G.heights[i].px);
    free(G.tiles);
    free(G.tile_slot);
    free(G.hash);
    free(G.vec);
    bool reg = G.registered;
    Cvar *c[5] = { G.cv_rotate, G.cv_texture, G.cv_lines, G.cv_travel, G.cv_detail };
    memset(&G, 0, sizeof(G));
    G.registered = reg;
    G.cv_rotate = c[0];
    G.cv_texture = c[1];
    G.cv_lines = c[2];
    G.cv_travel = c[3];
    G.cv_detail = c[4];
    G.sel_region = -1;
}
