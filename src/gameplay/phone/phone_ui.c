// Phone UI (docs/PHONE.md):
//   * the in-game phone screen UI, drawn on the CPU into an RGBA overlay (status bar, shutter,
//     zoom, record, gallery, pairing QR) that phone_tonemap.comp composites over the camera image;
//   * the HUD while holding the phone (key hints, the pairing panel with the QR code, toasts);
//   * the phone model (body, live screen, lens) for hands.c.
#include "phone_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "input/input.h"
#include "render/phone_view.h"
#include "render/mesh_gen.h"
#include "net/http_ws/hws_server.h"
#include "ui/gui/gui.h"
#include "ui/gui/gui_draw.h"
#include "ui/gui/gui_theme.h"
#include "ui/ui.h"
#include "gameplay/items.h"
#include "game/game.h"
#include "stb_image.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>

// ------------------------------------------------------------------ CPU canvas

typedef struct Canvas { u8 *px; int w, h; float s; } Canvas;

typedef struct FontBake { bool ok; float size; stbtt_bakedchar ch[96]; u8 atlas[512 * 512]; } FontBake;
static struct { bool tried; FontBake f[3]; } UF;   // 0 small, 1 body, 2 large (design px at 640 wide)
static const float k_font_px[3] = { 24, 32, 56 };

static void fonts_load(void) {
    if (UF.tried) return;
    UF.tried = true;
    char p[512];
    platform_data_path("assets/ui/fonts/Barlow-SemiBold.ttf", p, sizeof(p));
    size_t n;
    u8 *ttf = (u8 *)platform_read_file(p, &n);
    if (!ttf) { log_warn("phone: font %s missing (screen text off)", p); return; }
    for (int i = 0; i < 3; i++) {
        UF.f[i].size = k_font_px[i];
        UF.f[i].ok = stbtt_BakeFontBitmap(ttf, 0, k_font_px[i], UF.f[i].atlas, 512, 512, 32, 96, UF.f[i].ch) > 0;
    }
    free(ttf);
}

static void blend(u8 *d, u32 c, float a) {
    a *= (float)(c >> 24) / 255.0f;
    if (a <= 0.0f) return;
    float r = (float)(c & 255), g = (float)((c >> 8) & 255), b = (float)((c >> 16) & 255);
    float da = d[3] / 255.0f, oa = a + da * (1.0f - a);
    if (oa <= 1e-5f) return;
    d[0] = (u8)((r * a + d[0] * da * (1.0f - a)) / oa);
    d[1] = (u8)((g * a + d[1] * da * (1.0f - a)) / oa);
    d[2] = (u8)((b * a + d[2] * da * (1.0f - a)) / oa);
    d[3] = (u8)(oa * 255.0f + 0.5f);
}

static void c_rect(Canvas *c, float x, float y, float w, float h, u32 col) {
    int x0 = DG_MAX(0, (int)(x * c->s)), y0 = DG_MAX(0, (int)(y * c->s)), x1 = DG_MIN(c->w, (int)((x + w) * c->s + 0.5f)), y1 = DG_MIN(c->h, (int)((y + h) * c->s + 0.5f));
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) blend(c->px + ((size_t)yy * (size_t)c->w + (size_t)xx) * 4, col, 1.0f);
}

// Rounded rectangle / circle / ring with 1 px anti-aliasing (signed distance).
static void c_round(Canvas *c, float x, float y, float w, float h, float r, u32 col, float ring) {
    float S = c->s;
    x *= S; y *= S; w *= S; h *= S; r *= S; ring *= S;
    int x0 = DG_MAX(0, (int)x - 1), y0 = DG_MAX(0, (int)y - 1), x1 = DG_MIN(c->w, (int)(x + w) + 2), y1 = DG_MIN(c->h, (int)(y + h) + 2);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - r, hy = h * 0.5f - r;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float px = fabsf((float)xx + 0.5f - cx) - hx, py = fabsf((float)yy + 0.5f - cy) - hy;
            float ox = fmaxf(px, 0.0f), oy = fmaxf(py, 0.0f);
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(px, py), 0.0f) - r;
            if (ring > 0) d = fabsf(d + ring * 0.5f) - ring * 0.5f;
            float a = saturate(0.5f - d);
            if (a > 0) blend(c->px + ((size_t)yy * (size_t)c->w + (size_t)xx) * 4, col, a);
        }
}

static void c_circle(Canvas *c, float cx, float cy, float r, u32 col, float ring) { c_round(c, cx - r, cy - r, r * 2, r * 2, r, col, ring); }

static float c_text_w(int f, const char *s) {
    if (!UF.f[f].ok) return 0;
    float x = 0, y = 0;
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || (unsigned char)*s >= 128) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(UF.f[f].ch, 512, 512, *s - 32, &x, &y, &q, 1);
    }
    return x;
}

// align: 0 left, 1 centre, 2 right; y = baseline (design px).
static void c_text(Canvas *c, int f, float x, float y, int align, u32 col, const char *s) {
    if (!UF.f[f].ok) return;
    const FontBake *fb = &UF.f[f];
    // The fonts are baked for 640 px wide screens: scale glyphs with the canvas.
    float k = c->s;
    float tw = c_text_w(f, s);
    float px = x * k - (align == 1 ? tw * 0.5f : (align == 2 ? tw : 0.0f)) * k, py = y * k;
    float bx = 0, by = 0;
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || (unsigned char)*s >= 128) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(fb->ch, 512, 512, *s - 32, &bx, &by, &q, 1);
        int gw = (int)((q.x1 - q.x0) * k + 0.5f), gh = (int)((q.y1 - q.y0) * k + 0.5f);
        int ox = (int)(px + q.x0 * k), oy = (int)(py + q.y0 * k);
        for (int yy = 0; yy < gh; yy++) {
            int ty = oy + yy;
            if (ty < 0 || ty >= c->h) continue;
            float v = q.t0 + (q.t1 - q.t0) * ((float)yy + 0.5f) / (float)DG_MAX(gh, 1);
            int ay = DG_CLAMP((int)(v * 512.0f), 0, 511);
            for (int xx = 0; xx < gw; xx++) {
                int tx = ox + xx;
                if (tx < 0 || tx >= c->w) continue;
                float u = q.s0 + (q.s1 - q.s0) * ((float)xx + 0.5f) / (float)DG_MAX(gw, 1);
                int ax = DG_CLAMP((int)(u * 512.0f), 0, 511);
                float a = fb->atlas[ay * 512 + ax] / 255.0f;
                if (a > 0.0f) blend(c->px + ((size_t)ty * (size_t)c->w + (size_t)tx) * 4, col, a);
            }
        }
    }
}

// An RGBA image (sw x sh) fitted into the rect (letterboxed, design px).
static void c_image(Canvas *c, const u8 *img, int sw, int sh, float x, float y, float w, float h, float alpha) {
    int x0 = (int)(x * c->s), y0 = (int)(y * c->s), dw = (int)(w * c->s), dh = (int)(h * c->s);
    for (int yy = 0; yy < dh; yy++) {
        int ty = y0 + yy;
        if (ty < 0 || ty >= c->h) continue;
        int sy = DG_MIN(sh - 1, yy * sh / DG_MAX(dh, 1));
        for (int xx = 0; xx < dw; xx++) {
            int tx = x0 + xx;
            if (tx < 0 || tx >= c->w) continue;
            int sx = DG_MIN(sw - 1, xx * sw / DG_MAX(dw, 1));
            const u8 *s = img + ((size_t)sy * (size_t)sw + (size_t)sx) * 4;
            u32 col = (u32)s[0] | (u32)s[1] << 8 | (u32)s[2] << 16 | 0xFF000000u;
            blend(c->px + ((size_t)ty * (size_t)c->w + (size_t)tx) * 4, col, alpha * s[3] / 255.0f);
        }
    }
}

#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)

void phone_ui_thumb(const u8 *rgba, int w, int h, int stride, u8 *out) {
    // Fit (cover) into PHONE_THUMB_W x PHONE_THUMB_H, box filtered.
    float sx = (float)w / PHONE_THUMB_W, sy = (float)h / PHONE_THUMB_H, s = fminf(sx, sy);
    float ox = ((float)w - s * PHONE_THUMB_W) * 0.5f, oy = ((float)h - s * PHONE_THUMB_H) * 0.5f;
    int k = DG_MAX(1, (int)s);
    for (int y = 0; y < PHONE_THUMB_H; y++)
        for (int x = 0; x < PHONE_THUMB_W; x++) {
            int x0 = (int)(ox + (float)x * s), y0 = (int)(oy + (float)y * s);
            u32 acc[3] = { 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < DG_MIN(h, y0 + k); yy += DG_MAX(1, k / 4))
                for (int xx = x0; xx < DG_MIN(w, x0 + k); xx += DG_MAX(1, k / 4)) {
                    const u8 *p = rgba + (size_t)yy * (size_t)stride + (size_t)xx * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; n++;
                }
            u8 *d = out + ((size_t)y * PHONE_THUMB_W + (size_t)x) * 4;
            for (int c = 0; c < 3; c++) d[c] = (u8)(n ? acc[c] / n : 0);
            d[3] = 255;
        }
}

// Gallery thumbnails: at most one decoded per screen rebuild (JPEG decode is a few ms).
static PhoneThumb *thumb_for(PhoneState *s, const PhoneGalleryItem *it, bool *loaded_one) {
    for (int i = 0; i < PHONE_GALLERY_THUMBS; i++)
        if (s->thumbs[i].path[0] && !strcmp(s->thumbs[i].path, it->path)) return s->thumbs[i].loaded ? &s->thumbs[i] : NULL;
    if (*loaded_one) return NULL;
    *loaded_one = true;
    int slot = PHONE_GALLERY_THUMBS - 1;
    for (int i = 0; i < PHONE_GALLERY_THUMBS; i++)
        if (!s->thumbs[i].path[0]) { slot = i; break; }
    PhoneThumb *t = &s->thumbs[slot];
    memset(t, 0, sizeof(*t));
    dg_strcpy(t->path, sizeof(t->path), it->path);
    t->video = it->video;
    t->seconds = it->meta.seconds;
    int w, h, n;
    u8 *px = stbi_load(it->thumb, &w, &h, &n, 4);
    if (px) {
        phone_ui_thumb(px, w, h, w * 4, t->px);
        stbi_image_free(px);
        t->loaded = true;
    }
    return t->loaded ? t : NULL;
}

// ------------------------------------------------------------------ screen UI

static const char *mmss(double s, char *b, size_t n) {
    int t = (int)s;
    snprintf(b, n, "%d:%02d", t / 60, t % 60);
    return b;
}

static void status_bar(PhoneState *s, Canvas *c, float W) {
    // Top band, clock from the day / night cycle, battery, link state, recording.
    for (int i = 0; i < 70; i++) c_rect(c, 0, (float)i, W, 1, RGBA(0, 0, 0, (int)(110.0f * (1.0f - (float)i / 70.0f))));
    World *w = game_world();
    char clock[16];
    float h = w ? world_time_hours(w) : 12.0f;
    snprintf(clock, sizeof(clock), "%02d:%02d", (int)h % 24, (int)((h - floorf(h)) * 60.0f));
    c_text(c, 1, 26, 40, 0, RGBA(255, 255, 255, 240), clock);
    // Battery (cosmetic, drains with the in-game day).
    float bat = w ? 1.0f - fmodf(h / 24.0f, 1.0f) * 0.6f : 0.8f;
    float bx = W - 74;
    c_round(c, bx, 20, 44, 22, 5, RGBA(255, 255, 255, 220), 2.5f);
    c_rect(c, bx + 44, 27, 4, 8, RGBA(255, 255, 255, 200));
    c_round(c, bx + 4, 24, 36 * bat, 14, 3, bat < 0.25f ? RGBA(255, 80, 70, 230) : RGBA(255, 255, 255, 230), 0);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(bat * 100.0f));
    c_text(c, 0, bx - 10, 38, 2, RGBA(255, 255, 255, 220), pct);
    // Link to the real phone.
    if (s->conn >= 0) {
        c_round(c, W * 0.5f + 110, 18, 70, 26, 13, RGBA(80, 200, 120, 200), 0);
        c_text(c, 0, W * 0.5f + 145, 38, 1, RGBA(10, 30, 15, 255), "LINK");
    }
    if (s->rec) {
        char t[24];
        c_round(c, W * 0.5f - 80, 16, 160, 32, 16, RGBA(220, 30, 30, 220), 0);
        c_circle(c, W * 0.5f - 58, 32, 7, RGBA(255, 255, 255, 255), 0);
        c_text(c, 1, W * 0.5f + 8, 42, 1, RGBA(255, 255, 255, 255), mmss(phone_recorder_seconds(s->rec), t, sizeof(t)));
    }
}

static void camera_ui(PhoneState *s, Canvas *c, float W, float H) {
    // Rule of thirds.
    for (int i = 1; i < 3; i++) {
        c_rect(c, W * (float)i / 3.0f, 70, 1.5f, H - 300, RGBA(255, 255, 255, 34));
        c_rect(c, 0, H * (float)i / 3.0f, W, 1.5f, RGBA(255, 255, 255, 34));
    }
    // Bottom band: gallery thumbnail, shutter, record.
    for (int i = 0; i < 230; i++) c_rect(c, 0, H - (float)i, W, 1, RGBA(0, 0, 0, (int)(120.0f * (1.0f - (float)i / 230.0f))));
    float cy = H - 110;
    c_circle(c, W * 0.5f, cy, 54, RGBA(255, 255, 255, 240), 7);
    if (s->rec) c_round(c, W * 0.5f - 24, cy - 24, 48, 48, 9, RGBA(230, 40, 40, 255), 0);
    else c_circle(c, W * 0.5f, cy, 42, RGBA(255, 255, 255, 230), 0);
    float rx = W - 110;
    c_circle(c, rx, cy, 30, RGBA(255, 255, 255, 220), 4);
    c_circle(c, rx, cy, 18, RGBA(230, 40, 40, 255), 0);
    if (s->have_last_thumb) {
        c_round(c, 44, cy - 46, 52, 92, 8, RGBA(255, 255, 255, 230), 0);
        c_image(c, s->last_thumb, PHONE_THUMB_W, PHONE_THUMB_H, 47, cy - 43, 46, 86, 1.0f);
    } else {
        c_round(c, 44, cy - 46, 52, 92, 8, RGBA(255, 255, 255, 160), 3);
    }
    // Zoom chips + value.
    static const float presets[3] = { 1, 2, 5 };
    float zy = H - 250;
    for (int i = 0; i < 3; i++) {
        float x = W * 0.5f + (float)(i - 1) * 84;
        bool on = fabsf(s->zoom - presets[i]) < 0.05f;
        c_circle(c, x, zy, 30, on ? RGBA(255, 214, 10, 235) : RGBA(0, 0, 0, 120), 0);
        char t[16];
        snprintf(t, sizeof(t), "%gx", presets[i]);
        c_text(c, 0, x, zy + 9, 1, on ? RGBA(20, 20, 20, 255) : RGBA(255, 255, 255, 235), t);
    }
    bool preset = false;
    for (int i = 0; i < 3; i++) preset |= fabsf(s->zoom - presets[i]) < 0.05f;
    if (!preset) {
        char t[12];
        snprintf(t, sizeof(t), "%.1fx", s->zoom);
        c_round(c, W * 0.5f - 46, zy - 80, 92, 40, 20, RGBA(0, 0, 0, 150), 0);
        c_text(c, 1, W * 0.5f, zy - 49, 1, RGBA(255, 214, 10, 255), t);
    }
    // Vertical zoom slider (log scale 1..8x) on the right edge.
    float top = 140, bot = H - 330, x = W - 34;
    c_round(c, x - 3, top, 6, bot - top, 3, RGBA(255, 255, 255, 90), 0);
    float t = logf(fmaxf(s->zoom, 1.0f)) / logf(8.0f);
    float ky = bot - t * (bot - top);
    c_circle(c, x, ky, 14, RGBA(255, 214, 10, 240), 0);
    // Orientation source.
    const char *src = s->fake ? "FAKE GYRO" : (s->driven ? "GYRO" : NULL);
    if (src) {
        c_round(c, 20, 76, c_text_w(0, src) + 24, 30, 15, RGBA(0, 0, 0, 120), 0);
        c_text(c, 0, 32, 98, 0, RGBA(255, 255, 255, 220), src);
    }
}

static void pair_ui(PhoneState *s, Canvas *c, float W, float H) {
    c_rect(c, 0, 0, W, H, RGBA(14, 17, 22, 255));
    c_text(c, 2, W * 0.5f, 170, 1, RGBA(255, 255, 255, 255), s->conn >= 0 ? "Paired" : "Pair phone");
    if (s->conn >= 0) {
        c_circle(c, W * 0.5f, H * 0.42f, 90, RGBA(80, 200, 120, 255), 10);
        c_text(c, 1, W * 0.5f, H * 0.42f + 200, 1, RGBA(200, 210, 220, 255), "Use the page on your phone");
        return;
    }
    if (!s->token[0] || s->token_left <= 0) {
        c_text(c, 1, W * 0.5f, H * 0.45f, 1, RGBA(200, 210, 220, 255), s->server_err[0] ? "Server unavailable" : "Code expired");
        c_text(c, 0, W * 0.5f, H * 0.45f + 48, 1, RGBA(150, 160, 170, 255), s->server_err[0] ? s->server_err : "Press P for a new code");
        return;
    }
    // QR modules (quiet zone 2) in a white card.
    int n = s->qr.size;
    float side = W - 120, cell = side / (float)(n + 4), x0 = (W - side) * 0.5f, y0 = 230;
    c_round(c, x0 - 10, y0 - 10, side + 20, side + 20, 22, RGBA(255, 255, 255, 255), 0);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (s->qr.mod[y * n + x]) c_rect(c, x0 + (float)(x + 2) * cell, y0 + (float)(y + 2) * cell, cell + 0.6f, cell + 0.6f, RGBA(0, 0, 0, 255));
    float ty = y0 + side + 70;
    c_text(c, 1, W * 0.5f, ty, 1, RGBA(230, 235, 240, 255), "Scan with the phone camera");
    char t[96], tm[16];
    snprintf(t, sizeof(t), "or open  %s", s->ip_text);
    c_text(c, 0, W * 0.5f, ty + 50, 1, RGBA(160, 170, 180, 255), t);
    snprintf(t, sizeof(t), "Expires in %s", mmss(s->token_left, tm, sizeof(tm)));
    c_text(c, 0, W * 0.5f, ty + 90, 1, s->token_left < 20 ? RGBA(255, 150, 90, 255) : RGBA(160, 170, 180, 255), t);
}

static void gallery_ui(PhoneState *s, Canvas *c, float W, float H) {
    c_rect(c, 0, 0, W, H, RGBA(10, 12, 15, 255));
    char t[48];
    snprintf(t, sizeof(t), "Gallery  %d", s->gallery.count);
    c_text(c, 2, 34, 150, 0, RGBA(255, 255, 255, 255), t);
    bool loaded = false;
    float cw = (W - 40) / 3.0f, chh = cw * 16.0f / 9.0f;
    for (int i = 0; i < s->gallery.count && i < 9; i++) {
        float x = 20 + (float)(i % 3) * cw, y = 190 + (float)(i / 3) * (chh + 12);
        if (y + chh > H) break;
        PhoneThumb *th = thumb_for(s, &s->gallery.items[i], &loaded);
        if (th) c_image(c, th->px, PHONE_THUMB_W, PHONE_THUMB_H, x + 4, y, cw - 8, chh, 1.0f);
        else c_rect(c, x + 4, y, cw - 8, chh, RGBA(40, 44, 50, 255));
        if (s->gallery.items[i].video) {
            c_circle(c, x + cw * 0.5f, y + chh * 0.5f, 26, RGBA(0, 0, 0, 140), 0);
            c_circle(c, x + cw * 0.5f + 3, y + chh * 0.5f, 10, RGBA(255, 255, 255, 230), 0);
            char d[16];
            c_text(c, 0, x + cw - 14, y + chh - 12, 2, RGBA(255, 255, 255, 240), mmss(s->gallery.items[i].meta.seconds, d, sizeof(d)));
        }
    }
    if (!s->gallery.count) c_text(c, 1, W * 0.5f, H * 0.5f, 1, RGBA(150, 160, 170, 255), "No photos yet");
}

u32 phone_ui_hash(const PhoneState *s) {
    World *w = game_world();
    u32 h = 2166136261u;
    u32 v[16] = { (u32)s->screen, (u32)lrintf(s->zoom * 50.0f), s->rec ? (u32)phone_recorder_seconds(s->rec) + 1 : 0, (u32)(s->conn >= 0), (u32)s->token_left,
                  (u32)s->landscape, (u32)s->gallery.count, w ? (u32)(world_time_hours(w) * 60.0f) : 0, (u32)s->have_last_thumb, (u32)(s->server_err[0] != 0),
                  (u32)s->driven, (u32)s->fake, (u32)(s->token[0] != 0), 0, 0, 0 };
    for (int i = 0; i < 16; i++) { h ^= v[i]; h *= 16777619u; }
    return h;
}

void phone_ui_build_screen(PhoneState *s, u8 *rgba, int w, int h) {
    fonts_load();
    memset(rgba, 0, (size_t)w * (size_t)h * 4);
    // Landscape: the UI is drawn upright for the rotated phone (top of the screen to the left
    // when rolled +90 deg) into a landscape canvas, then rotated into the portrait screen.
    bool land = s->landscape && s->screen == PSCR_CAMERA;
    int cw = land ? h : w, ch = land ? w : h;
    u8 *buf = land ? (u8 *)calloc((size_t)cw * (size_t)ch, 4) : rgba;
    if (!buf) return;
    Canvas c = { buf, cw, ch, (float)DG_MIN(cw, ch) / 640.0f };
    float W = (float)cw / c.s, H = (float)ch / c.s;
    switch (s->screen) {
    case PSCR_PAIR: pair_ui(s, &c, W, H); break;
    case PSCR_GALLERY: gallery_ui(s, &c, W, H); break;
    default: camera_ui(s, &c, W, H); break;
    }
    status_bar(s, &c, W);
    if (land) {
        bool neg = phone_euler_ypr(s->rel).z < 0 && s->driven;
        for (int ly = 0; ly < ch; ly++)
            for (int lx = 0; lx < cw; lx++) {
                int px = neg ? ly : w - 1 - ly, py = neg ? h - 1 - lx : lx;
                memcpy(rgba + ((size_t)py * (size_t)w + (size_t)px) * 4, buf + ((size_t)ly * (size_t)cw + (size_t)lx) * 4, 4);
            }
        free(buf);
    }
}

// ------------------------------------------------------------------ HUD

static float key_hint(float x, float y, Action a, const char *fallback, const char *what) {
    const GuiTheme *th = gui_theme();
    char key[32];
    binding_name(input_get_binding(a, 0), key, sizeof(key));
    if (!key[0] || !strcmp(key, "?")) dg_strcpy(key, sizeof(key), fallback);
    GuiText t = { gui_s(th->t_tiny), GW_SEMIBOLD, th->text_dim, GUI_RIGHT, th->tracking_caps * 1.3f, true, true, 0 };
    float tw = gd_text_width(&t, what);
    gd_text(&t, x, y + gui_s(3), what);
    GuiText kt = { gui_s(th->t_tiny), GW_SEMIBOLD, th->text_dark, GUI_CENTER, 0, false, false, 0 };
    float ks = gui_s(18), kw = fmaxf(ks, gd_text_width(&kt, key) + gui_s(8));
    GuiRect kr = { x - tw - gui_s(6) - kw, y, kw, ks };
    gd_round(kr, gui_s(3), UI_RGBA(236, 236, 230, 225));
    gd_text(&kt, kr.x + kr.w * 0.5f, kr.y + (ks - gd_line_height(kt.size)) * 0.5f, key);
    return kr.x - gui_s(14);
}

void phone_ui_hud(PhoneState *s) {
    if (!s->holding) return;
    const GuiTheme *th = gui_theme();
    float W = ui_width(), H = ui_height();
    // Key hints (bottom right, like the vehicle HUD).
    float x = W - gui_s(36), y = H - gui_s(130);
    if (s->camera) {
        x = key_hint(x, y, ACT_PHONE_GALLERY, "J", "Gallery");
        x = key_hint(x, y, ACT_RELOAD, "R", s->rec ? "Stop" : "Record");
        x = key_hint(x, y, ACT_ATTACK, "LMB", "Photo");
        GuiText t = { gui_s(th->t_tiny), GW_SEMIBOLD, th->text_dim, GUI_RIGHT, th->tracking_caps * 1.3f, true, true, 0 };
        gd_text(&t, x + gui_s(8), y + gui_s(3), "Wheel  Zoom");
    } else {
        x = key_hint(x, y, ACT_PHONE_PAIR, "P", s->conn >= 0 ? "Phone linked" : "Pair phone");
        x = key_hint(x, y, ACT_AIM, "RMB", "Camera");
    }
    // Pairing panel: the QR code big enough to scan from the monitor.
    if (s->screen == PSCR_PAIR && s->conn < 0 && s->token[0] && s->token_left > 0 && s->qr_tex) {
        float side = gui_s(300), pw = side + gui_s(48), ph = side + gui_s(190);
        GuiRect p = { W - pw - gui_s(40), H * 0.5f - ph * 0.5f, pw, ph };
        gd_shadow(p, gui_s(10), gui_s(18), UI_RGBA(0, 0, 0, 120));
        gd_round(p, gui_s(10), th->hud_panel);
        GuiText tt = { gui_s(th->t_label), GW_SEMIBOLD, th->text, GUI_LEFT, th->tracking_caps, true, true, 0 };
        gd_text(&tt, p.x + gui_s(24), p.y + gui_s(18), "Pair your phone");
        GuiRect q = { p.x + gui_s(24), p.y + gui_s(56), side, side };
        gd_image(q, s->qr_tex, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
        GuiText bt = { gui_s(th->t_small), GW_MEDIUM, th->text_dim, GUI_LEFT, 0, false, false, 0 };
        char line[96], tm[16];
        snprintf(line, sizeof(line), "or open  %s", s->ip_text);
        gd_text(&bt, p.x + gui_s(24), q.y + side + gui_s(14), line);
        snprintf(line, sizeof(line), "Same Wi-Fi \xC2\xB7 expires in %s", mmss(s->token_left, tm, sizeof(tm)));
        gd_text(&bt, p.x + gui_s(24), q.y + side + gui_s(38), line);
        gd_text(&bt, p.x + gui_s(24), q.y + side + gui_s(62), "Certificate warning: open details, then visit");
        gd_text(&bt, p.x + gui_s(24), q.y + side + gui_s(84), "(local self-signed certificate, docs/PHONE.md)");
    } else if (s->screen == PSCR_PAIR && s->server_err[0]) {
        GuiText bt = { gui_s(th->t_small), GW_MEDIUM, th->danger, GUI_RIGHT, 0, false, false, 0 };
        gd_text(&bt, W - gui_s(40), H * 0.5f, s->server_err);
    }
    // Toast (2.5 s).
    if (s->toast[0] && g_engine.real_time - s->toast_t < 2.5) {
        float a = (float)saturate((2.5 - (g_engine.real_time - s->toast_t)) / 0.4);
        GuiText tt = { gui_s(th->t_small), GW_SEMIBOLD, gui_rgba_mul(th->text, a), GUI_CENTER, 0, false, true, 0 };
        float tw = gd_text_width(&tt, s->toast);
        GuiRect r = { W * 0.5f - tw * 0.5f - gui_s(16), H * 0.18f, tw + gui_s(32), gui_s(34) };
        gd_round(r, gui_s(17), gui_rgba_mul(th->hud_panel, a));
        gd_text(&tt, W * 0.5f, r.y + gui_s(7), s->toast);
    }
    // Recording.
    if (s->rec) {
        char t[32], tm[16];
        snprintf(t, sizeof(t), "REC  %s", mmss(phone_recorder_seconds(s->rec), tm, sizeof(tm)));
        GuiText rt = { gui_s(th->t_small), GW_SEMIBOLD, UI_RGBA(255, 80, 70, 255), GUI_LEFT, th->tracking_caps, true, true, 0 };
        float blink = fmodf((float)g_engine.real_time, 1.0f) < 0.6f ? 1.0f : 0.25f;
        gd_circle(W * 0.5f - gui_s(48), gui_s(46), gui_s(7), gui_rgba_mul(UI_RGBA(255, 60, 50, 255), blink));
        gd_text(&rt, W * 0.5f - gui_s(34), gui_s(36), t);
    }
}

// ------------------------------------------------------------------ the phone model

static void ensure_meshes(PhoneState *s) {
    if (s->cube) return;
    MeshBuilder mb;
    mb_init(&mb);
    mb_box(&mb, v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 0xffffffffu);
    s->cube = mb_upload(&mb);
    mb_reset(&mb);
    // Unit quad in XY facing +Z; v = 0 at the top (+Y) so image row 0 is the top of the screen.
    vec3 n = v3(0, 0, 1);
    u32 a = mb_vertex(&mb, v3(-0.5f, -0.5f, 0), n, v2(0, 1), 0xffffffffu);
    u32 b = mb_vertex(&mb, v3(0.5f, -0.5f, 0), n, v2(1, 1), 0xffffffffu);
    u32 c = mb_vertex(&mb, v3(0.5f, 0.5f, 0), n, v2(1, 0), 0xffffffffu);
    u32 d = mb_vertex(&mb, v3(-0.5f, 0.5f, 0), n, v2(0, 0), 0xffffffffu);
    mb_tri(&mb, a, b, c);
    mb_tri(&mb, a, c, d);
    s->quad = mb_upload(&mb);
    mb_free(&mb);
}

static void draw_part(MeshId mesh, mat4 m, Material mat, bool viewmodel) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.mesh = mesh;
    d.model = m;
    d.mat = mat;
    if (viewmodel) render_draw_viewmodel(&d);
    else render_draw(&d);
}

void phone_draw_held(const ItemDef *it, vec3 pos, quat rot, bool viewmodel, u32 flags) {
    PhoneState *s = phone_state();
    if (!s->init) return;
    ensure_meshes(s);
    // Body.
    Material body = material_color(v4(0.035f, 0.037f, 0.042f, 1));
    body.roughness = 0.32f;
    body.metallic = 0.65f;
    body.flags = flags;
    draw_part(s->cube, mat4_trs(v3_add(pos, quat_rotate(rot, it->cam.body_center)), rot, v3_scale(it->cam.body_half, 2.0f)), body, viewmodel);
    // Lens bump on the back.
    Material lens = material_color(v4(0.01f, 0.01f, 0.012f, 1));
    lens.roughness = 0.1f;
    lens.metallic = 0.2f;
    lens.flags = flags;
    vec3 lp = v3_add(pos, quat_rotate(rot, v3_add(it->cam.lens.t, v3(0, 0, -0.001f))));
    draw_part(s->cube, mat4_trs(lp, rot, v3(0.011f, 0.011f, 0.003f)), lens, viewmodel);
    // Screen: the live view of this phone (view 0 = ours, view 1 = the nearest other phone),
    // emissive like a display under the current exposure; dark when not rendered.
    if (!viewmodel && s->remote_count < (int)ARRAY_COUNT(s->remote)) {
        s->remote[s->remote_count].pos = pos;
        s->remote[s->remote_count].rot = rot;
        s->remote[s->remote_count].it = it;
        s->remote_count++;
    }
    TextureId tex = 0;
    if (viewmodel) tex = render_phone_screen(0);
    else if (s->remote_valid && v3_dist(pos, s->remote_lens) < 0.08f) tex = render_phone_screen(1);
    Material scr = material_color(tex ? v4(s->screen_k, s->screen_k, s->screen_k, 1) : v4(0.004f, 0.005f, 0.006f, 1));
    scr.flags = (tex ? (MAT_UNLIT | MAT_TEXTURED) : 0) | MAT_NO_SHADOW | (flags & MAT_VIEWMODEL);
    scr.texture = tex;
    scr.roughness = 0.15f;
    scr.tex_scale = 1.0f;
    vec3 sp = v3_add(pos, quat_rotate(rot, v3_add(it->cam.screen.t, v3(0, 0, 0.0003f))));
    draw_part(s->quad, mat4_trs(sp, quat_mul(rot, it->cam.screen.r), v3(it->cam.screen_half.x * 2.0f, it->cam.screen_half.y * 2.0f, 1.0f)), scr, viewmodel);
}

void phone_ui_shutdown(void) {
    PhoneState *s = phone_state();
    if (s->quad) render_mesh_destroy(s->quad);
    if (s->cube) render_mesh_destroy(s->cube);
    s->quad = s->cube = 0;
}

void phone_draw(void) {
    PhoneState *s = phone_state();
    if (s->init) phone_ui_hud(s);
}
