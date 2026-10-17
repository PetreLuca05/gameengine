// HUD vector icons (src/ui/hud/hud_icons.h). Coordinates are in a unit box (-1..1, y down).
#include "hud_icons.h"
#include "ui/gui/gui_widgets.h"
#include <math.h>
#include <string.h>

typedef struct IconCtx { float cx, cy, s, t; u32 c; } IconCtx;

static void L(const IconCtx *k, float x0, float y0, float x1, float y1) {
    gd_line(k->cx + x0 * k->s, k->cy + y0 * k->s, k->cx + x1 * k->s, k->cy + y1 * k->s, k->t, k->c);
}
static void poly(const IconCtx *k, const float *p, int n, bool closed) {
    for (int i = 0; i + 1 < n; i++) L(k, p[i * 2], p[i * 2 + 1], p[i * 2 + 2], p[i * 2 + 3]);
    if (closed) L(k, p[(n - 1) * 2], p[(n - 1) * 2 + 1], p[0], p[1]);
}
static void ring(const IconCtx *k, float x, float y, float r) { gd_ring(k->cx + x * k->s, k->cy + y * k->s, r * k->s, k->t, 1.0f, k->c); }
static void dot(const IconCtx *k, float x, float y, float r) { gd_circle(k->cx + x * k->s, k->cy + y * k->s, r * k->s, k->c); }

static void i_generic(const IconCtx *k) {   // map pin
    static const float p[] = { 0, 0.95f, -0.55f, 0.05f, -0.6f, -0.35f, -0.3f, -0.85f, 0.3f, -0.85f, 0.6f, -0.35f, 0.55f, 0.05f };
    poly(k, p, 7, true);
    dot(k, 0, -0.35f, 0.2f);
}
static void i_enemy(const IconCtx *k) {     // crosshair
    ring(k, 0, 0, 0.6f);
    L(k, 0, -0.95f, 0, -0.3f); L(k, 0, 0.3f, 0, 0.95f); L(k, -0.95f, 0, -0.3f, 0); L(k, 0.3f, 0, 0.95f, 0);
    dot(k, 0, 0, 0.1f);
}
static void i_loot(const IconCtx *k) {      // crate
    static const float p[] = { -0.8f, -0.45f, 0.8f, -0.45f, 0.8f, 0.75f, -0.8f, 0.75f };
    poly(k, p, 4, true);
    L(k, -0.8f, -0.45f, -0.5f, -0.8f); L(k, -0.5f, -0.8f, 0.5f, -0.8f); L(k, 0.5f, -0.8f, 0.8f, -0.45f);
    L(k, -0.2f, -0.45f, -0.2f, 0.1f); L(k, 0.2f, -0.45f, 0.2f, 0.1f); L(k, -0.2f, 0.1f, 0.2f, 0.1f);
}
static void i_danger(const IconCtx *k) {    // warning triangle
    static const float p[] = { 0, -0.9f, 0.95f, 0.8f, -0.95f, 0.8f };
    poly(k, p, 3, true);
    L(k, 0, -0.3f, 0, 0.25f);
    dot(k, 0, 0.52f, 0.09f);
}
static void i_go(const IconCtx *k) {        // flag
    L(k, -0.6f, -0.9f, -0.6f, 0.95f);
    static const float p[] = { -0.6f, -0.85f, 0.75f, -0.55f, -0.6f, -0.1f };
    poly(k, p, 3, false);
}
static void i_help(const IconCtx *k) {      // medical cross in a circle
    ring(k, 0, 0, 0.9f);
    float a = 0.18f, b = 0.55f;
    const float p[] = { -a, -b, a, -b, a, -a, b, -a, b, a, a, a, a, b, -a, b, -a, a, -b, a, -b, -a, -a, -a };
    poly(k, p, 12, true);
}
static void i_vehicle(const IconCtx *k) {   // car side
    static const float p[] = { -0.95f, 0.35f, -0.95f, -0.05f, -0.55f, -0.15f, -0.3f, -0.55f, 0.35f, -0.55f, 0.6f, -0.15f, 0.95f, -0.05f, 0.95f, 0.35f };
    poly(k, p, 8, true);
    ring(k, -0.5f, 0.45f, 0.2f);
    ring(k, 0.5f, 0.45f, 0.2f);
}
static void i_defend(const IconCtx *k) {    // shield
    static const float p[] = { 0, -0.95f, 0.8f, -0.65f, 0.75f, 0.1f, 0, 0.95f, -0.75f, 0.1f, -0.8f, -0.65f };
    poly(k, p, 6, true);
    L(k, 0, -0.6f, 0, 0.55f);
}
static void door_frame(const IconCtx *k) {
    static const float p[] = { -0.7f, 0.95f, -0.7f, -0.95f, 0.5f, -0.95f, 0.5f, 0.95f };
    poly(k, p, 4, false);
}
static void i_door_open(const IconCtx *k) {
    door_frame(k);
    static const float p[] = { -0.7f, -0.95f, 0.05f, -0.65f, 0.05f, 0.95f, -0.7f, 0.95f };
    poly(k, p, 3, false);
    dot(k, -0.1f, 0.05f, 0.07f);
    L(k, 0.65f, 0, 0.95f, 0); L(k, 0.95f, 0, 0.82f, -0.13f); L(k, 0.95f, 0, 0.82f, 0.13f);
}
static void i_door_close(const IconCtx *k) {
    door_frame(k);
    dot(k, 0.3f, 0.05f, 0.07f);
}
static void i_door_slam(const IconCtx *k) {
    door_frame(k);
    dot(k, 0.3f, 0.05f, 0.07f);
    L(k, 0.7f, -0.5f, 0.95f, -0.7f); L(k, 0.72f, 0, 1.0f, 0); L(k, 0.7f, 0.5f, 0.95f, 0.7f);
}
static void i_peek(const IconCtx *k) {      // eye
    int n = 10;
    for (int i = 0; i < n; i++) {
        float a0 = DG_PI * (float)i / (float)n, a1 = DG_PI * (float)(i + 1) / (float)n;
        L(k, -cosf(a0) * 0.95f, -sinf(a0) * 0.55f, -cosf(a1) * 0.95f, -sinf(a1) * 0.55f);
        L(k, -cosf(a0) * 0.95f, sinf(a0) * 0.55f, -cosf(a1) * 0.95f, sinf(a1) * 0.55f);
    }
    ring(k, 0, 0, 0.3f);
    dot(k, 0, 0, 0.12f);
}
static void i_skull(const IconCtx *k) {
    ring(k, 0, -0.2f, 0.62f);
    static const float p[] = { -0.35f, 0.3f, -0.35f, 0.8f, 0.35f, 0.8f, 0.35f, 0.3f };
    poly(k, p, 4, false);
    dot(k, -0.25f, -0.2f, 0.15f);
    dot(k, 0.25f, -0.2f, 0.15f);
}
static void i_headshot(const IconCtx *k) {
    ring(k, 0, 0, 0.75f);
    dot(k, 0, 0, 0.22f);
}
static void i_zombie(const IconCtx *k) {    // reaching hand
    ring(k, 0, -0.55f, 0.32f);
    L(k, 0, -0.2f, 0, 0.4f); L(k, 0, 0.4f, -0.4f, 0.95f); L(k, 0, 0.4f, 0.4f, 0.95f);
    L(k, 0, -0.05f, 0.85f, -0.1f); L(k, 0, 0.1f, 0.85f, 0.05f);
}
static void i_player(const IconCtx *k) {
    ring(k, 0, -0.5f, 0.33f);
    static const float p[] = { -0.7f, 0.9f, -0.6f, 0.2f, 0, -0.05f, 0.6f, 0.2f, 0.7f, 0.9f };
    poly(k, p, 5, false);
}
static void i_grenade(const IconCtx *k) {
    ring(k, 0, 0.2f, 0.62f);
    L(k, -0.2f, -0.45f, 0.25f, -0.45f); L(k, 0.25f, -0.45f, 0.25f, -0.75f); L(k, 0.25f, -0.75f, -0.2f, -0.75f);
    L(k, 0.25f, -0.6f, 0.7f, -0.35f);
}
static void i_flare(const IconCtx *k) {
    L(k, -0.15f, 0.95f, -0.15f, -0.3f); L(k, 0.15f, 0.95f, 0.15f, -0.3f); L(k, -0.15f, 0.95f, 0.15f, 0.95f);
    L(k, 0, -0.5f, 0, -0.95f); L(k, -0.4f, -0.45f, -0.65f, -0.75f); L(k, 0.4f, -0.45f, 0.65f, -0.75f);
}
static void i_molotov(const IconCtx *k) {
    static const float p[] = { -0.15f, -0.55f, -0.15f, -0.25f, -0.5f, 0.05f, -0.5f, 0.95f, 0.5f, 0.95f, 0.5f, 0.05f, 0.15f, -0.25f, 0.15f, -0.55f };
    poly(k, p, 8, false);
    L(k, 0, -0.6f, -0.15f, -0.95f); L(k, 0, -0.6f, 0.15f, -0.9f);
}
static void i_heal(const IconCtx *k) { i_help(k); }

typedef struct IconEntry { const char *name; void (*fn)(const IconCtx *); } IconEntry;
static const IconEntry k_icons[] = {
    { "generic", i_generic }, { "enemy", i_enemy }, { "loot", i_loot }, { "danger", i_danger },
    { "go", i_go }, { "help", i_help }, { "vehicle", i_vehicle }, { "defend", i_defend },
    { "door_open", i_door_open }, { "door_close", i_door_close }, { "door_slam", i_door_slam }, { "peek", i_peek },
    { "skull", i_skull }, { "headshot", i_headshot }, { "zombie", i_zombie }, { "player", i_player },
    { "grenade", i_grenade }, { "flare", i_flare }, { "molotov", i_molotov }, { "heal", i_heal },
};

static const IconEntry *find(const char *name) {
    for (int i = 0; i < (int)ARRAY_COUNT(k_icons); i++) if (!strcmp(k_icons[i].name, name)) return &k_icons[i];
    return NULL;
}

bool hud_icon_exists(const char *name) { return name && find(name) != NULL; }

bool hud_icon(const char *name, float cx, float cy, float size, u32 color) {
    const IconEntry *e = name ? find(name) : NULL;
    if (!e) return false;
    IconCtx k = { cx, cy, size * 0.5f, fmaxf(1.2f, size * 0.085f), color };
    e->fn(&k);
    return true;
}
