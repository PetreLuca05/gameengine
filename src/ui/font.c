#include "font.h"
#include "core/engine.h"
#include "core/log.h"
#include "nuklear_config.h"
#include <stdlib.h>

typedef struct FontSystem {
    struct nk_font_atlas atlas;
    struct nk_font *fonts[FONT_COUNT];
    struct nk_draw_null_texture null_tex;
    TextureId tex;
    int atlas_w, atlas_h;
} FontSystem;

#define FS (g_engine.fonts)

bool font_init(float ui_scale) {
    FS = (FontSystem *)calloc(1, sizeof(FontSystem));
    FontSystem *f = FS;
    nk_font_atlas_init_default(&f->atlas);
    nk_font_atlas_begin(&f->atlas);
    float sizes[FONT_COUNT] = { 13.0f, 16.0f, 26.0f, 48.0f };
    for (int i = 0; i < FONT_COUNT; i++) {
        struct nk_font_config cfg = nk_font_config(sizes[i] * ui_scale);
        cfg.oversample_h = 2;
        cfg.oversample_v = 2;
        cfg.pixel_snap = false;
        f->fonts[i] = nk_font_atlas_add_default(&f->atlas, sizes[i] * ui_scale, &cfg);
    }
    const void *pixels = nk_font_atlas_bake(&f->atlas, &f->atlas_w, &f->atlas_h, NK_FONT_ATLAS_RGBA32);
    if (!pixels) { log_error("font: atlas bake failed"); return false; }
    f->tex = render_texture_create(f->atlas_w, f->atlas_h, pixels, false, false);
    nk_font_atlas_end(&f->atlas, nk_handle_id((int)f->tex), &f->null_tex);
    render_set_text_atlas(f->tex);
    log_info("font: atlas %dx%d", f->atlas_w, f->atlas_h);
    return true;
}

void font_shutdown(void) {
    FontSystem *f = FS;
    if (!f) return;
    nk_font_atlas_clear(&f->atlas);
    free(f);
    FS = NULL;
}

const struct nk_user_font *font_nk(FontSize s) { return &FS->fonts[s]->handle; }
TextureId font_atlas_texture(void) { return FS->tex; }
vec2 font_white_uv(void) { return v2(FS->null_tex.uv.x, FS->null_tex.uv.y); }
float font_line_height(FontSize s) { return FS->fonts[s]->handle.height; }

float font_text_width(FontSize s, const char *text) {
    struct nk_font *font = FS->fonts[s];
    return font->handle.width(font->handle.userdata, font->handle.height, text, (int)strlen(text));
}

float font_layout_2d(FontSize s, float x, float y, const char *text, FontQuadFn fn, void *user) {
    struct nk_font *font = FS->fonts[s];
    float start = x;
    const char *p = text;
    while (*p) {
        nk_rune rune;
        int n = nk_utf_decode(p, &rune, 4);
        if (n <= 0) break;
        p += n;
        const struct nk_font_glyph *g = nk_font_find_glyph(font, rune);
        if (!g) continue;
        if (g->w > 0 && g->h > 0)
            fn(user, x + g->x0, y + g->y0, x + g->x1, y + g->y1, g->u0, g->v0, g->u1, g->v1);
        x += g->xadvance;
    }
    return x - start;
}

typedef struct Text3DCtx { vec3 origin, right, up; float scale; u32 color; TextVertex *v; int n; } Text3DCtx;

static void quad3d(void *user, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    Text3DCtx *c = (Text3DCtx *)user;
    if (!c->v) return;
    // 2D layout has y down; map to world with up axis.
    vec3 p00 = v3_add(c->origin, v3_add(v3_scale(c->right, x0 * c->scale), v3_scale(c->up, -y0 * c->scale)));
    vec3 p10 = v3_add(c->origin, v3_add(v3_scale(c->right, x1 * c->scale), v3_scale(c->up, -y0 * c->scale)));
    vec3 p11 = v3_add(c->origin, v3_add(v3_scale(c->right, x1 * c->scale), v3_scale(c->up, -y1 * c->scale)));
    vec3 p01 = v3_add(c->origin, v3_add(v3_scale(c->right, x0 * c->scale), v3_scale(c->up, -y1 * c->scale)));
    TextVertex *v = c->v + c->n * 6;
    v[0] = (TextVertex){ p00, { u0, v0 }, c->color };
    v[1] = (TextVertex){ p10, { u1, v0 }, c->color };
    v[2] = (TextVertex){ p11, { u1, v1 }, c->color };
    v[3] = (TextVertex){ p00, { u0, v0 }, c->color };
    v[4] = (TextVertex){ p11, { u1, v1 }, c->color };
    v[5] = (TextVertex){ p01, { u0, v1 }, c->color };
    c->n++;
}

static void count_quad(void *user, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)u0; (void)v0; (void)u1; (void)v1;
    (*(int *)user)++;
}

void font_text3d(vec3 origin, vec3 right, vec3 up, float height, u32 color, const char *text, bool centered) {
    if (!FS || !FS->tex) return;
    FontSize s = FONT_HUGE;
    float lh = font_line_height(s);
    float scale = height / lh;
    int glyphs = 0;
    font_layout_2d(s, 0, 0, text, count_quad, &glyphs);
    if (!glyphs) return;
    Text3DCtx c = { origin, right, up, scale, color, render_text_alloc((u32)glyphs * 6), 0 };
    if (!c.v) return;
    if (centered) {
        float w = font_text_width(s, text) * scale;
        c.origin = v3_add(origin, v3_add(v3_scale(right, -w * 0.5f), v3_scale(up, height * 0.5f)));
    }
    font_layout_2d(s, 0, 0, text, quad3d, &c);
}

void font_text3d_billboard(vec3 pos, vec3 cam_right, vec3 cam_up, float height, u32 color, const char *text) {
    font_text3d(pos, cam_right, cam_up, height, color, text, true);
}
