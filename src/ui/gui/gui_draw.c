#include "gui_draw.h"
#include "gui_theme.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/profiler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Our own copy of stb_truetype (Nuklear embeds another one): static, so the two never clash.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define ATLAS 2048
#define FONT_BASE 44.0f          // SDF bake size (px)
#define FONT_PAD 6               // SDF spread (px at the bake size)
#define MAX_GLYPHS 1024
#define LAYER_VERTS 98304
#define LAYER_INDICES 196608
#define LAYER_CMDS 2048
#define CLIP_STACK 16
#define ALPHA_STACK 16

typedef struct Glyph {
    u16 x, y, w, h;              // atlas pixels (0 size: blank, e.g. space)
    float xoff, yoff, adv;       // bake pixels
} Glyph;

typedef struct Font {
    stbtt_fontinfo info;
    unsigned char *data;
    float scale;                 // font units -> bake pixels
    float ascent, descent, line_gap;
    short map[256];              // codepoint -> glyph (-1 missing)
    int extra_cp[32];
    short extra_glyph[32];
    int extra_count;
    float kern[95][95];          // ASCII pairs (bake pixels)
} Font;

typedef struct Layer {
    UiVertex *v;
    u32 vc;
    u32 *i;
    u32 ic;
    UiDrawCmd cmd[LAYER_CMDS];
    u32 cc;
} Layer;

static struct {
    bool ok;
    Font fonts[GW_COUNT];
    Glyph glyphs[MAX_GLYPHS];
    int glyph_count;
    u8 *pixels;                  // CPU atlas (kept for font reloads)
    int shelf_x, shelf_y, shelf_h;
    TextureId atlas;
    Layer layers[GL_COUNT];
    int layer;
    int sw, sh;
    GuiRect clip[CLIP_STACK];
    int clip_n;
    float alpha[ALPHA_STACK];
    int alpha_n;
    TextureId backdrop;
    int stat_cmds, stat_verts;
} D;

// Atlas regions (texels)
#define WHITE_UV_X 2
#define CIRCLE_X 8
#define CIRCLE_Y 0
#define CIRCLE_SZ 64
#define CIRCLE_R 28.0f
#define CIRCLE_SPREAD 4.0f
#define SHADOW_X 80
#define SHADOW_Y 0
#define SHADOW_SZ 64
#define SHADOW_B 22

static vec2 auv(float x, float y) { return v2(x / (float)ATLAS, y / (float)ATLAS); }

// ------------------------------------------------------------------ atlas build

static bool pack(int w, int h, int *ox, int *oy) {
    if (D.shelf_x + w + 1 > ATLAS) { D.shelf_x = 0; D.shelf_y += D.shelf_h + 1; D.shelf_h = 0; }
    if (D.shelf_y + h + 1 > ATLAS) return false;
    *ox = D.shelf_x;
    *oy = D.shelf_y;
    D.shelf_x += w + 1;
    if (h > D.shelf_h) D.shelf_h = h;
    return true;
}

static void put(int x, int y, u8 a) {
    u8 *p = D.pixels + ((size_t)y * ATLAS + (size_t)x) * 4;
    p[0] = p[1] = p[2] = 255;
    p[3] = a;
}

static void build_shapes(void) {
    for (int y = 0; y < 6; y++) for (int x = 0; x < 6; x++) put(x, y, 255);
    // SDF circle (alpha = 0.5 at the edge, +-CIRCLE_SPREAD texels to 1 / 0).
    for (int y = 0; y < CIRCLE_SZ; y++)
        for (int x = 0; x < CIRCLE_SZ; x++) {
            float dx = (float)x + 0.5f - CIRCLE_SZ * 0.5f, dy = (float)y + 0.5f - CIRCLE_SZ * 0.5f;
            float d = sqrtf(dx * dx + dy * dy);
            float a = 0.5f + (CIRCLE_R - d) / (2.0f * CIRCLE_SPREAD);
            put(CIRCLE_X + x, CIRCLE_Y + y, (u8)(saturate(a) * 255.0f + 0.5f));
        }
    // Soft shadow 9-slice: a rounded box [B, SZ - B] with a Gaussian-ish falloff outside.
    for (int y = 0; y < SHADOW_SZ; y++)
        for (int x = 0; x < SHADOW_SZ; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float lo = (float)SHADOW_B, hi = (float)(SHADOW_SZ - SHADOW_B), r = 4.0f;
            float qx = fmaxf(fmaxf(lo + r - px, px - (hi - r)), 0.0f), qy = fmaxf(fmaxf(lo + r - py, py - (hi - r)), 0.0f);
            float d = sqrtf(qx * qx + qy * qy) - r;
            float a = d <= 0 ? 1.0f : expf(-(d * d) / (2.0f * 7.0f * 7.0f));
            put(SHADOW_X + x, SHADOW_Y + y, (u8)(saturate(a) * 255.0f + 0.5f));
        }
    D.shelf_x = 0;
    D.shelf_y = 72;
    D.shelf_h = 0;
}

// Exact 1D squared distance transform (Felzenszwalb & Huttenlocher), in place over n samples.
static void edt_1d(float *f, int n, float *d, int *v, float *z) {
    int k = 0;
    v[0] = 0;
    z[0] = -1e20f;
    z[1] = 1e20f;
    for (int q = 1; q < n; q++) {
        float s = ((f[q] + (float)(q * q)) - (f[v[k]] + (float)(v[k] * v[k]))) / (float)(2 * q - 2 * v[k]);
        while (s <= z[k]) {
            k--;
            s = ((f[q] + (float)(q * q)) - (f[v[k]] + (float)(v[k] * v[k]))) / (float)(2 * q - 2 * v[k]);
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (float)q) k++;
        d[q] = (float)((q - v[k]) * (q - v[k])) + f[v[k]];
    }
    for (int q = 0; q < n; q++) f[q] = d[q];
}

static void edt_2d(float *g, int w, int h) {
    int n = DG_MAX(w, h);
    float *f = (float *)malloc(sizeof(float) * n), *d = (float *)malloc(sizeof(float) * n), *z = (float *)malloc(sizeof(float) * (n + 1));
    int *v = (int *)malloc(sizeof(int) * n);
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) f[y] = g[y * w + x];
        edt_1d(f, h, d, v, z);
        for (int y = 0; y < h; y++) g[y * w + x] = f[y];
    }
    for (int y = 0; y < h; y++) edt_1d(g + y * w, w, d, v, z);
    free(f); free(d); free(z); free(v);
}

#define SDF_SS 4   // supersampling of the coverage bitmap the distance field is computed from

// SDF from a rasterized coverage bitmap (non-zero winding, so overlapping contours - common in
// static instances of variable fonts like Barlow - stay solid; stbtt's analytic SDF would carve
// seams along them), via an exact Euclidean distance transform at SDF_SS x resolution.
static unsigned char *glyph_sdf(Font *f, int gi, int *ow, int *oh, int *oxo, int *oyo) {
    float hs = f->scale * SDF_SS;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&f->info, gi, hs, hs, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return NULL;
    int P = FONT_PAD * SDF_SS;
    // Output grid aligned on SDF_SS source pixels.
    int ox0 = (int)floorf((float)(x0 - P) / SDF_SS), oy0 = (int)floorf((float)(y0 - P) / SDF_SS);
    int ox1 = (int)ceilf((float)(x1 + P) / SDF_SS), oy1 = (int)ceilf((float)(y1 + P) / SDF_SS);
    int W = (ox1 - ox0) * SDF_SS, H = (oy1 - oy0) * SDF_SS;
    unsigned char *bm = (unsigned char *)calloc((size_t)W * H, 1);
    int bx = x0 - ox0 * SDF_SS, by = y0 - oy0 * SDF_SS;
    stbtt_MakeGlyphBitmap(&f->info, bm + by * W + bx, x1 - x0, y1 - y0, W, hs, hs, gi);
    float *in = (float *)malloc(sizeof(float) * W * H), *out = (float *)malloc(sizeof(float) * W * H);
    for (int i = 0; i < W * H; i++) {
        bool inside = bm[i] >= 128;
        out[i] = inside ? 0.0f : 1e20f;   // distance to the nearest inside pixel
        in[i] = inside ? 1e20f : 0.0f;    // distance to the nearest outside pixel
    }
    edt_2d(out, W, H);
    edt_2d(in, W, H);
    int w = ox1 - ox0, h = oy1 - oy0;
    unsigned char *sdf = (unsigned char *)malloc((size_t)w * h);
    float scale = 128.0f / (float)FONT_PAD;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            // Average the signed distance over the SDF_SS x SDF_SS block (centre-weighted enough).
            float acc = 0;
            for (int sy = 1; sy < SDF_SS - 1 + (SDF_SS < 3); sy++)
                for (int sx = 1; sx < SDF_SS - 1 + (SDF_SS < 3); sx++) {
                    int i = (y * SDF_SS + sy) * W + x * SDF_SS + sx;
                    float d = in[i] > 0 ? (sqrtf(in[i]) - 0.5f) : -(sqrtf(out[i]) - 0.5f);
                    acc += d;
                }
            int nsub = (SDF_SS - 2 + (SDF_SS < 3)) * (SDF_SS - 2 + (SDF_SS < 3));
            float d = acc / (float)nsub / SDF_SS;   // output pixels, + inside
            float v = 128.0f + d * scale;
            sdf[y * w + x] = (unsigned char)DG_CLAMP((int)(v + 0.5f), 0, 255);
        }
    free(bm); free(in); free(out);
    *ow = w;
    *oh = h;
    *oxo = ox0;
    *oyo = oy0;
    return sdf;
}

static int bake_glyph(Font *f, int cp) {
    int gi = stbtt_FindGlyphIndex(&f->info, cp);
    if (!gi && cp != ' ') return -1;
    if (D.glyph_count >= MAX_GLYPHS) return -1;
    Glyph *g = &D.glyphs[D.glyph_count];
    memset(g, 0, sizeof(*g));
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    g->adv = (float)adv * f->scale;
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char *sdf = glyph_sdf(f, gi, &w, &h, &xo, &yo);
    if (sdf && w > 0 && h > 0) {
        int ax, ay;
        if (!pack(w, h, &ax, &ay)) { free(sdf); log_warn("ui: font atlas full"); return -1; }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) put(ax + x, ay + y, sdf[y * w + x]);
        g->x = (u16)ax;
        g->y = (u16)ay;
        g->w = (u16)w;
        g->h = (u16)h;
        g->xoff = (float)xo;
        g->yoff = (float)yo;
    }
    free(sdf);
    return D.glyph_count++;
}

static bool load_font(Font *f, const char *path) {
    char full[512];
    snprintf(full, sizeof(full), "%s%s", platform_base_dir(), path);
    size_t size = 0;
    f->data = (unsigned char *)platform_read_file(full, &size);
    if (!f->data || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        log_error("ui: cannot load font %s", path);
        free(f->data);
        f->data = NULL;
        return false;
    }
    f->scale = stbtt_ScaleForPixelHeight(&f->info, FONT_BASE);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    f->ascent = (float)asc * f->scale;
    f->descent = (float)desc * f->scale;
    f->line_gap = (float)gap * f->scale;
    for (int c = 0; c < 256; c++) f->map[c] = -1;
    for (int c = 32; c < 256; c++) {
        if (c >= 127 && c < 160) continue;
        f->map[c] = (short)bake_glyph(f, c);
    }
    static const int extras[] = { 0x2022, 0x2026, 0x2014, 0x2013, 0x2018, 0x2019, 0x201C, 0x201D, 0x2190, 0x2191, 0x2192, 0x2193, 0x25B2, 0x25BC, 0x2715, 0x2713, 0x221E };
    f->extra_count = 0;
    for (int i = 0; i < (int)ARRAY_COUNT(extras); i++) {
        int g = bake_glyph(f, extras[i]);
        if (g < 0) continue;
        f->extra_cp[f->extra_count] = extras[i];
        f->extra_glyph[f->extra_count] = (short)g;
        f->extra_count++;
    }
    for (int a = 0; a < 95; a++)
        for (int b = 0; b < 95; b++) f->kern[a][b] = (float)stbtt_GetCodepointKernAdvance(&f->info, 32 + a, 32 + b) * f->scale;
    return true;
}

static void free_fonts(void) {
    for (int i = 0; i < GW_COUNT; i++) { free(D.fonts[i].data); D.fonts[i].data = NULL; }
}

static bool build_atlas(void) {
    const GuiTheme *th = gui_theme();
    double t0 = platform_time();
    if (!D.pixels) D.pixels = (u8 *)malloc((size_t)ATLAS * ATLAS * 4);
    memset(D.pixels, 0, (size_t)ATLAS * ATLAS * 4);
    for (size_t i = 0; i < (size_t)ATLAS * ATLAS; i++) { D.pixels[i * 4] = D.pixels[i * 4 + 1] = D.pixels[i * 4 + 2] = 255; }
    D.glyph_count = 0;
    build_shapes();
    free_fonts();
    const char *paths[GW_COUNT] = { th->font_regular, th->font_medium, th->font_semibold };
    bool ok = true;
    for (int i = 0; i < GW_COUNT; i++) ok &= load_font(&D.fonts[i], paths[i]);
    if (D.atlas) render_texture_destroy(D.atlas);
    D.atlas = render_texture_create(ATLAS, ATLAS, D.pixels, false, false);
    log_info("ui: SDF font atlas %d glyphs, %d px used, %.0f ms", D.glyph_count, D.shelf_y + D.shelf_h, (platform_time() - t0) * 1000.0);
    return ok && D.atlas;
}

bool gd_init(void) {
    for (int l = 0; l < GL_COUNT; l++) {
        D.layers[l].v = (UiVertex *)malloc(sizeof(UiVertex) * LAYER_VERTS);
        D.layers[l].i = (u32 *)malloc(sizeof(u32) * LAYER_INDICES);
    }
    D.ok = build_atlas();
    return D.ok;
}

bool gd_reload_fonts(void) { return build_atlas(); }

void gd_shutdown(void) {
    free_fonts();
    for (int l = 0; l < GL_COUNT; l++) { free(D.layers[l].v); free(D.layers[l].i); }
    free(D.pixels);
    if (D.atlas) render_texture_destroy(D.atlas);
    memset(&D, 0, sizeof(D));
}

TextureId gd_atlas(void) { return D.atlas; }

// ui_dump_atlas [path]: the SDF atlas alpha as a grey PNG (font / shape debugging).
#include "stb_image_write.h"
#include "core/cvar.h"
static void cmd_dump_atlas(int argc, const char **argv) {
    if (!D.pixels) return;
    const char *path = argc > 1 ? argv[1] : "ui_atlas.png";
    int h = D.shelf_y + D.shelf_h + 1;
    u8 *g = (u8 *)malloc((size_t)ATLAS * h);
    for (int i = 0; i < ATLAS * h; i++) g[i] = D.pixels[i * 4 + 3];
    stbi_write_png(path, ATLAS, h, 1, g, ATLAS);
    free(g);
    log_info("ui: atlas written to %s", path);
}
void gd_register_commands(void) { cmd_register("ui_dump_atlas", cmd_dump_atlas, "ui_dump_atlas [path]: write the UI SDF atlas (alpha) as a PNG"); }
int gd_stats_cmds(void) { return D.stat_cmds; }
int gd_stats_verts(void) { return D.stat_verts; }

// ------------------------------------------------------------------ draw list

void gd_begin(int w, int h) {
    D.sw = w;
    D.sh = h;
    for (int l = 0; l < GL_COUNT; l++) D.layers[l].vc = D.layers[l].ic = D.layers[l].cc = 0;
    D.layer = GL_BASE;
    D.clip_n = 1;
    D.clip[0] = gui_rect(0, 0, (float)w, (float)h);
    D.alpha_n = 1;
    D.alpha[0] = 1.0f;
    D.backdrop = 0;
}

void gd_set_layer(int layer) { D.layer = DG_CLAMP(layer, 0, GL_COUNT - 1); }
int gd_layer(void) { return D.layer; }

void gd_alpha_push(float a) {
    if (D.alpha_n < ALPHA_STACK) { D.alpha[D.alpha_n] = D.alpha[D.alpha_n - 1] * saturate(a); D.alpha_n++; }
}
void gd_alpha_pop(void) { if (D.alpha_n > 1) D.alpha_n--; }

static GuiRect rect_isect(GuiRect a, GuiRect b) {
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y), x1 = fminf(a.x + a.w, b.x + b.w), y1 = fminf(a.y + a.h, b.y + b.h);
    return gui_rect(x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0));
}

void gd_clip_push(GuiRect r) {
    if (D.clip_n < CLIP_STACK) { D.clip[D.clip_n] = rect_isect(D.clip[D.clip_n - 1], r); D.clip_n++; }
}
void gd_clip_pop(void) { if (D.clip_n > 1) D.clip_n--; }
GuiRect gd_clip(void) { return D.clip[D.clip_n - 1]; }

static u32 col(u32 c) {
    float a = D.alpha[D.alpha_n - 1];
    return a >= 0.999f ? c : gui_rgba_mul(c, a);
}

// Starts / continues the command for (texture, clip); returns the layer or NULL when full.
static Layer *prim(TextureId tex, u32 nv, u32 ni) {
    Layer *L = &D.layers[D.layer];
    if (L->vc + nv > LAYER_VERTS || L->ic + ni > LAYER_INDICES) return NULL;
    GuiRect c = D.clip[D.clip_n - 1];
    int cx = (int)floorf(c.x), cy = (int)floorf(c.y), cw = (int)ceilf(c.x + c.w) - cx, ch = (int)ceilf(c.y + c.h) - cy;
    UiDrawCmd *last = L->cc ? &L->cmd[L->cc - 1] : NULL;
    if (!last || last->texture != tex || last->clip_x != cx || last->clip_y != cy || last->clip_w != cw || last->clip_h != ch) {
        if (L->cc >= LAYER_CMDS) return NULL;
        UiDrawCmd *d = &L->cmd[L->cc++];
        memset(d, 0, sizeof(*d));
        d->index_offset = L->ic;
        d->texture = tex;
        d->clip_x = cx;
        d->clip_y = cy;
        d->clip_w = cw;
        d->clip_h = ch;
    }
    return L;
}

static void quad_uv(TextureId tex, float x0, float y0, float x1, float y1, vec2 uv0, vec2 uv1, u32 c) {
    if (x1 <= x0 || y1 <= y0) return;
    GuiRect cl = D.clip[D.clip_n - 1];
    if (x1 < cl.x || y1 < cl.y || x0 > cl.x + cl.w || y0 > cl.y + cl.h) return;
    Layer *L = prim(tex, 4, 6);
    if (!L) return;
    u32 b = L->vc;
    UiVertex *v = L->v + b;
    v[0] = (UiVertex){ v2(x0, y0), v2(uv0.x, uv0.y), c };
    v[1] = (UiVertex){ v2(x1, y0), v2(uv1.x, uv0.y), c };
    v[2] = (UiVertex){ v2(x1, y1), v2(uv1.x, uv1.y), c };
    v[3] = (UiVertex){ v2(x0, y1), v2(uv0.x, uv1.y), c };
    L->vc += 4;
    u32 *i = L->i + L->ic;
    i[0] = b; i[1] = b + 1; i[2] = b + 2; i[3] = b; i[4] = b + 2; i[5] = b + 3;
    L->ic += 6;
    L->cmd[L->cc - 1].index_count += 6;
}

static void quad_grad(float x0, float y0, float x1, float y1, u32 c00, u32 c10, u32 c11, u32 c01) {
    if (x1 <= x0 || y1 <= y0) return;
    Layer *L = prim(D.atlas, 4, 6);
    if (!L) return;
    vec2 uv = auv(WHITE_UV_X + 0.5f, WHITE_UV_X + 0.5f);
    u32 b = L->vc;
    UiVertex *v = L->v + b;
    v[0] = (UiVertex){ v2(x0, y0), uv, c00 };
    v[1] = (UiVertex){ v2(x1, y0), uv, c10 };
    v[2] = (UiVertex){ v2(x1, y1), uv, c11 };
    v[3] = (UiVertex){ v2(x0, y1), uv, c01 };
    L->vc += 4;
    u32 *i = L->i + L->ic;
    i[0] = b; i[1] = b + 1; i[2] = b + 2; i[3] = b; i[4] = b + 2; i[5] = b + 3;
    L->ic += 6;
    L->cmd[L->cc - 1].index_count += 6;
}

void gd_rect(GuiRect r, u32 c) {
    if ((c >> 24) == 0) return;
    vec2 uv = auv(WHITE_UV_X + 0.5f, WHITE_UV_X + 0.5f);
    quad_uv(D.atlas, r.x, r.y, r.x + r.w, r.y + r.h, uv, uv, col(c));
}

void gd_rect_grad(GuiRect r, u32 top, u32 bottom) { quad_grad(r.x, r.y, r.x + r.w, r.y + r.h, col(top), col(top), col(bottom), col(bottom)); }
void gd_rect_hgrad(GuiRect r, u32 left, u32 right) { quad_grad(r.x, r.y, r.x + r.w, r.y + r.h, col(left), col(right), col(right), col(left)); }

void gd_border(GuiRect r, float t, u32 c) {
    gd_rect(gui_rect(r.x, r.y, r.w, t), c);
    gd_rect(gui_rect(r.x, r.y + r.h - t, r.w, t), c);
    gd_rect(gui_rect(r.x, r.y + t, t, r.h - 2 * t), c);
    gd_rect(gui_rect(r.x + r.w - t, r.y + t, t, r.h - 2 * t), c);
}

// 9-slice of the SDF circle: corners are its quadrants (radius px on screen), edges and centre
// sample the inside / edge texels. SDF mode (u + 2): crisp at any radius.
void gd_round(GuiRect r, float radius, u32 c) {
    if ((c >> 24) == 0 || r.w <= 0 || r.h <= 0) return;
    radius = fminf(radius, fminf(r.w, r.h) * 0.5f);
    if (radius < 0.75f) { gd_rect(r, c); return; }
    c = col(c);
    float e = CIRCLE_SZ * 0.5f - CIRCLE_R;   // texel inset of the circle's bounding box
    float u0 = CIRCLE_X + e, u1 = CIRCLE_X + CIRCLE_SZ * 0.5f, u2 = CIRCLE_X + CIRCLE_SZ - e;
    float v0 = CIRCLE_Y + e, v1 = CIRCLE_Y + CIRCLE_SZ * 0.5f, v2_ = CIRCLE_Y + CIRCLE_SZ - e;
    float xs[4] = { r.x, r.x + radius, r.x + r.w - radius, r.x + r.w };
    float ys[4] = { r.y, r.y + radius, r.y + r.h - radius, r.y + r.h };
    float us[4] = { u0, u1, u1, u2 }, vs[4] = { v0, v1, v1, v2_ };
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            vec2 a = auv(us[i], vs[j]), b = auv(us[i + 1], vs[j + 1]);
            a.x += 2.0f;
            b.x += 2.0f;
            quad_uv(D.atlas, xs[i], ys[j], xs[i + 1], ys[j + 1], a, b, c);
        }
}

// Outline of a rounded rect: straight edges + quarter arcs (works over translucent fills).
void gd_round_outline(GuiRect r, float radius, float t, u32 c) {
    if ((c >> 24) == 0 || t <= 0) return;
    // Whole pixels: the edges come out equally thick and sharp (fractional layouts / tweens blurred one
    // side and not the other).
    float x0 = floorf(r.x + 0.5f), y0 = floorf(r.y + 0.5f), x1 = floorf(r.x + r.w + 0.5f), y1 = floorf(r.y + r.h + 0.5f);
    r = gui_rect(x0, y0, x1 - x0, y1 - y0);
    t = fmaxf(1.0f, floorf(t + 0.5f));
    radius = fminf(radius, fminf(r.w, r.h) * 0.5f);
    // Below ~5 px the SDF circle is too small to round cleanly: a square, non-overlapping border.
    if (radius < 5.0f) { gd_border(r, t, c); return; }
    gd_rect(gui_rect(r.x + radius, r.y, r.w - 2 * radius, t), c);
    gd_rect(gui_rect(r.x + radius, r.y + r.h - t, r.w - 2 * radius, t), c);
    gd_rect(gui_rect(r.x, r.y + radius, t, r.h - 2 * radius), c);
    gd_rect(gui_rect(r.x + r.w - t, r.y + radius, t, r.h - 2 * radius), c);
    // Corners: quadrants of the SDF circle in ring mode (ui.frag: 6 + 2k + u, k = thickness in half
    // pixels), anti-aliased inside and out like gd_round (the old line-segment arcs stair-stepped).
    c = col(c);
    float k = fminf(floorf(t * 2.0f + 0.5f), 60.0f);
    float e = CIRCLE_SZ * 0.5f - CIRCLE_R;
    float u0 = CIRCLE_X + e, u1 = CIRCLE_X + CIRCLE_SZ * 0.5f, u2 = CIRCLE_X + CIRCLE_SZ - e;
    float v0 = CIRCLE_Y + e, v1 = CIRCLE_Y + CIRCLE_SZ * 0.5f, v2_ = CIRCLE_Y + CIRCLE_SZ - e;
    float xs[2][2] = { { r.x, r.x + radius }, { r.x + r.w - radius, r.x + r.w } };
    float ys[2][2] = { { r.y, r.y + radius }, { r.y + r.h - radius, r.y + r.h } };
    float us[2][2] = { { u0, u1 }, { u1, u2 } }, vs[2][2] = { { v0, v1 }, { v1, v2_ } };
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec2 a = auv(us[i][0], vs[j][0]), b = auv(us[i][1], vs[j][1]);
            a.x += 6.0f + 2.0f * k;
            b.x += 6.0f + 2.0f * k;
            quad_uv(D.atlas, xs[i][0], ys[j][0], xs[i][1], ys[j][1], a, b, c);
        }
}

void gd_round_border(GuiRect r, float radius, float t, u32 fill, u32 border) {
    if ((border >> 24) && t > 0) {
        if ((fill >> 24) >= 250) {
            // Opaque fill: the border colour underneath, then the fill inset (exact, anti-aliased).
            gd_round(r, radius, border);
            gd_round(gui_rect_inset(r, t), fmaxf(0.0f, radius - t), fill);
        } else {
            gd_round(gui_rect_inset(r, t), fmaxf(0.0f, radius - t), fill);
            gd_round_outline(r, radius, t, border);
        }
    } else {
        gd_round(r, radius, fill);
    }
}

void gd_shadow(GuiRect r, float radius, float spread, u32 c) {
    if ((c >> 24) == 0) return;
    c = col(c);
    (void)radius;
    GuiRect o = { r.x - spread, r.y - spread + spread * 0.35f, r.w + 2 * spread, r.h + 2 * spread };
    float xs[4] = { o.x, o.x + spread * 1.6f, o.x + o.w - spread * 1.6f, o.x + o.w };
    float ys[4] = { o.y, o.y + spread * 1.6f, o.y + o.h - spread * 1.6f, o.y + o.h };
    if (xs[1] > xs[2]) xs[1] = xs[2] = o.x + o.w * 0.5f;
    if (ys[1] > ys[2]) ys[1] = ys[2] = o.y + o.h * 0.5f;
    float us[4] = { SHADOW_X, SHADOW_X + SHADOW_B + 4, SHADOW_X + SHADOW_SZ - SHADOW_B - 4, SHADOW_X + SHADOW_SZ };
    float vs[4] = { SHADOW_Y, SHADOW_Y + SHADOW_B + 4, SHADOW_Y + SHADOW_SZ - SHADOW_B - 4, SHADOW_Y + SHADOW_SZ };
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) quad_uv(D.atlas, xs[i], ys[j], xs[i + 1], ys[j + 1], auv(us[i], vs[j]), auv(us[i + 1], vs[j + 1]), c);
}

void gd_circle(float cx, float cy, float radius, u32 c) {
    gd_round(gui_rect(cx - radius, cy - radius, radius * 2, radius * 2), radius, c);
}

void gd_line(float x0, float y0, float x1, float y1, float t, u32 c) {
    vec2 d = v2(x1 - x0, y1 - y0);
    float l = sqrtf(d.x * d.x + d.y * d.y);
    if (l < 1e-4f || (c >> 24) == 0) return;
    Layer *L = prim(D.atlas, 8, 18);
    if (!L) return;
    c = col(c);
    // Anti-aliased: a solid core plus a 1 px feather on each side that fades to transparent (the
    // HUD's vector icons / rings were stair-stepped). Lines thinner than the feather fade in alpha.
    vec2 u = v2(d.x / l, d.y / l), nn = v2(-u.y, u.x);
    float core = fmaxf(0.0f, t * 0.5f - 0.5f), outer = core + 1.0f;
    u32 ce = c;
    if (t < 1.0f) {
        u32 a = (u32)((float)(c >> 24) * fmaxf(t, 0.25f));
        ce = (c & 0x00FFFFFFu) | (a << 24);
    }
    u32 c0 = ce & 0x00FFFFFFu;   // transparent edge
    vec2 e = v2(u.x * 0.5f, u.y * 0.5f);   // half a pixel past each end
    vec2 p0 = v2(x0 - e.x, y0 - e.y), p1 = v2(x1 + e.x, y1 + e.y);
    vec2 uv = auv(WHITE_UV_X + 0.5f, WHITE_UV_X + 0.5f);
    u32 b = L->vc;
    UiVertex *v = L->v + b;
    float o[4] = { outer, core, -core, -outer };
    u32 cs[4] = { c0, ce, ce, c0 };
    for (int k = 0; k < 4; k++) {
        v[k] = (UiVertex){ v2(p0.x + nn.x * o[k], p0.y + nn.y * o[k]), uv, cs[k] };
        v[4 + k] = (UiVertex){ v2(p1.x + nn.x * o[k], p1.y + nn.y * o[k]), uv, cs[k] };
    }
    L->vc += 8;
    u32 *i = L->i + L->ic;
    for (int k = 0; k < 3; k++) {
        u32 a0 = b + (u32)k, a1 = b + (u32)k + 1, b0 = b + 4 + (u32)k, b1 = b + 4 + (u32)k + 1;
        i[k * 6 + 0] = a0; i[k * 6 + 1] = b0; i[k * 6 + 2] = b1;
        i[k * 6 + 3] = a0; i[k * 6 + 4] = b1; i[k * 6 + 5] = a1;
    }
    L->ic += 18;
    L->cmd[L->cc - 1].index_count += 18;
}

void gd_ring(float cx, float cy, float radius, float t, float frac, u32 c) {
    frac = saturate(frac);
    int seg = DG_MAX(6, (int)(48 * frac));
    float a0 = -DG_PI * 0.5f;
    for (int s = 0; s < seg; s++) {
        float ta = a0 + (2.0f * DG_PI) * frac * (float)s / (float)seg, tb = a0 + (2.0f * DG_PI) * frac * (float)(s + 1) / (float)seg;
        gd_line(cx + cosf(ta) * radius, cy + sinf(ta) * radius, cx + cosf(tb) * radius, cy + sinf(tb) * radius, t, c);
    }
}

void gd_image(GuiRect r, TextureId tex, vec4 uv, u32 tint) {
    if (!tex) return;
    quad_uv(tex, r.x, r.y, r.x + r.w, r.y + r.h, v2(uv.x, uv.y), v2(uv.z, uv.w), col(tint));
}

// The image's alpha filled with one colour (ui.frag: uv.y + 2 selects the silhouette mode).
void gd_image_mask(GuiRect r, TextureId tex, vec4 uv, u32 color) {
    if (!tex) return;
    quad_uv(tex, r.x, r.y, r.x + r.w, r.y + r.h, v2(uv.x, uv.y + 2.0f), v2(uv.z, uv.w + 2.0f), col(color));
}

void gd_backdrop(GuiRect r, u32 tint) {
    if (gui_theme()->blur <= 0.0f) { gd_rect(r, UI_RGBA(12, 13, 15, 235)); return; }
    if (!D.backdrop) D.backdrop = render_ui_backdrop();
    quad_uv(D.backdrop, r.x, r.y, r.x + r.w, r.y + r.h, v2(-1, 0), v2(-1, 0), col(tint));
}

// ------------------------------------------------------------------ text

static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p;
    if (c < 0x80) { *s += 1; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { *s += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { *s += 3; return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { *s += 4; return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); }
    *s += 1;
    return '?';
}

static int upper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    return c;
}

static const Glyph *glyph(const Font *f, int cp) {
    int g = -1;
    if (cp >= 0 && cp < 256) g = f->map[cp];
    else for (int i = 0; i < f->extra_count; i++) if (f->extra_cp[i] == cp) { g = f->extra_glyph[i]; break; }
    if (g < 0) g = f->map['?'];
    return g >= 0 ? &D.glyphs[g] : NULL;
}

static float kern(const Font *f, int a, int b) {
    if (a < 32 || a > 126 || b < 32 || b > 126) return 0.0f;
    return f->kern[a - 32][b - 32];
}

float gd_line_height(float size) { return size * 1.25f; }

// Advance of s (pixels) up to n bytes (n < 0: all).
static float measure(const GuiText *t, const char *s, int n) {
    const Font *f = &D.fonts[DG_CLAMP(t->weight, 0, GW_COUNT - 1)];
    if (!f->data || !s) return 0;
    float k = t->size / FONT_BASE, w = 0, track = t->tracking * t->size;
    int prev = 0;
    const char *end = n >= 0 ? s + n : NULL;
    while (*s && (!end || s < end)) {
        int c = utf8_next(&s);
        if (t->caps) c = upper(c);
        const Glyph *g = glyph(f, c);
        if (!g) continue;
        if (prev) w += kern(f, prev, c) * k + track;
        w += g->adv * k;
        prev = c;
    }
    return w;
}

float gd_text_width(const GuiText *t, const char *s) { return measure(t, s, -1); }

static void emit(const GuiText *t, float x, float y, const char *s, int n, bool shadow_pass) {
    const Font *f = &D.fonts[DG_CLAMP(t->weight, 0, GW_COUNT - 1)];
    float k = t->size / FONT_BASE, track = t->tracking * t->size;
    float lh = gd_line_height(t->size);
    float baseline = y + (lh - (f->ascent - f->descent) * k) * 0.5f + f->ascent * k;
    float off = shadow_pass ? fmaxf(1.0f, t->size * 0.07f) : 0.0f;
    u32 c = shadow_pass ? col(UI_RGBA(0, 0, 0, (int)(((t->color >> 24) & 255) * 0.75f))) : col(t->color);
    float ub = shadow_pass ? 4.0f : 2.0f;
    int prev = 0;
    const char *end = n >= 0 ? s + n : NULL;
    float px = x;
    while (*s && (!end || s < end)) {
        int cp = utf8_next(&s);
        if (t->caps) cp = upper(cp);
        const Glyph *g = glyph(f, cp);
        if (!g) continue;
        if (prev) px += kern(f, prev, cp) * k + track;
        if (g->w) {
            float x0 = px + g->xoff * k + off, y0 = baseline + g->yoff * k + off;
            vec2 a = auv(g->x, g->y), b = auv((float)g->x + g->w, (float)g->y + g->h);
            a.x += ub;
            b.x += ub;
            quad_uv(D.atlas, x0, y0, x0 + g->w * k, y0 + g->h * k, a, b, c);
        }
        px += g->adv * k;
        prev = cp;
    }
}

float gd_text(const GuiText *t, float x, float y, const char *s) {
    if (!s || !*s || !D.ok) return 0;
    prof_count(PC_UI_TEXT, 1);
    float w = measure(t, s, -1);
    int n = -1;
    char buf[512];
    if (t->max_w > 0 && w > t->max_w) {
        // Truncate with an ellipsis.
        GuiText tt = *t;
        float ell = measure(&tt, "\xE2\x80\xA6", -1);
        int len = (int)strlen(s), cut = len;
        while (cut > 0 && measure(&tt, s, cut) + ell > t->max_w) {
            cut--;
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        }
        snprintf(buf, sizeof(buf), "%.*s\xE2\x80\xA6", cut, s);
        s = buf;
        w = measure(t, s, -1);
    }
    if (t->align == GUI_CENTER) x -= w * 0.5f;
    else if (t->align == GUI_RIGHT) x -= w;
    x = floorf(x + 0.5f);
    y = floorf(y + 0.5f);
    if (t->shadow) emit(t, x, y, s, n, true);
    emit(t, x, y, s, n, false);
    return w;
}

float gd_text_simple(float x, float y, float size, int weight, u32 c, int align, const char *s) {
    GuiText t = { size, weight, c, align, 0, false, false, 0 };
    return gd_text(&t, x, y, s);
}

float gd_text_wrap(const GuiText *t, float x, float y, float w, const char *s, bool draw) {
    if (!s) return 0;
    float lh = gd_line_height(t->size), h = 0;
    const char *line = s;
    while (*line) {
        // Longest prefix that fits, broken at a space (or a newline).
        const char *p = line, *brk = NULL;
        while (*p && *p != '\n') {
            const char *q = p;
            utf8_next(&q);
            if (measure(t, line, (int)(q - line)) > w && brk) break;
            if (*p == ' ') brk = p;
            p = q;
        }
        const char *stop = (*p && *p != '\n' && brk) ? brk : p;
        if (draw) {
            GuiText tt = *t;
            tt.align = GUI_LEFT;
            tt.max_w = 0;
            // Centred / right-aligned paragraphs: each line inside [x, x + w).
            float lx = x;
            if (t->align != GUI_LEFT) {
                float lw = measure(t, line, (int)(stop - line));
                lx = t->align == GUI_CENTER ? x + (w - lw) * 0.5f : x + w - lw;
            }
            if (t->shadow) emit(&tt, floorf(lx), floorf(y + h), line, (int)(stop - line), true);
            emit(&tt, floorf(lx), floorf(y + h), line, (int)(stop - line), false);
        }
        h += lh;
        line = stop;
        while (*line == ' ') line++;
        if (*line == '\n') line++;
    }
    return h;
}

// ------------------------------------------------------------------ flush

void gd_flush(void) {
    D.stat_cmds = D.stat_verts = 0;
    for (int l = 0; l < GL_COUNT; l++) {
        Layer *L = &D.layers[l];
        if (!L->ic) continue;
        render_ui(L->v, L->vc, L->i, L->ic, L->cmd, L->cc);
        D.stat_cmds += (int)L->cc;
        D.stat_verts += (int)L->vc;
    }
}
