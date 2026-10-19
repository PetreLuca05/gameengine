// Inventory character preview and worn gear in the game, dev side (docs/CHARACTERS.md "Character
// preview", "Worn gear"):
//   char_preview_show [0|1|2] [w h] [yaw_deg] [held item id]   1: the preview over the screen every
//                                               frame, over the HUD, on the blurred dark backdrop the
//                                               inventory uses (w h 0: the screen's height, bottom-aligned);
//                                               2: rendered every frame but not shown (A/B of the main
//                                               view); 3: shown, the pose held still (image compares);
//                                               reloads assets/ui/char_preview.json
//   char_worn <head|-> [chest|-] [back|-] [slot]  worn gear item ids on a player (default: your own; '-' or
//                                               'none' empties a slot), kept every frame; logs the fits
//   char_preview_check                          next frame: the live player's CharacterInst, its skeleton
//                                               instance and poses and the render camera before / after
//                                               the preview call must be bit-identical (PASS / FAIL)
#include "game_internal.h"
#include "gameplay/characters.h"
#include "gameplay/characters_internal.h"
#include "render/render.h"
#include "ui/ui.h"
#include "ui/gui/gui_draw.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void char_preview_rig_reload(void);
void char_preview_info(char *out, size_t n);

static struct {
    int show;                         // 0 off, 1 shown, 2 rendered only
    int w, h;                         // 0: from the screen height
    float yaw;
    char held[32];                    // held item id ("" = empty hands)
    bool check;
    bool worn_set[MAX_PLAYERS];
    u16 worn[MAX_PLAYERS][CWORN_COUNT];
} CPD;

static void cmd_show(int argc, const char **argv) {
    CPD.show = argc > 1 ? DG_CLAMP(atoi(argv[1]), 0, 3) : !CPD.show;
    char_preview_set_frozen(CPD.show == 3);
    CPD.w = argc > 3 ? DG_MAX(atoi(argv[2]), 0) : 0;
    CPD.h = argc > 3 ? DG_MAX(atoi(argv[3]), 0) : 0;
    CPD.yaw = argc > 4 ? (float)atof(argv[4]) * DG_DEG2RAD : 0.0f;
    dg_strcpy(CPD.held, sizeof(CPD.held), argc > 5 ? argv[5] : "");
    char_preview_rig_reload();
    log_info("char_preview_show %d", CPD.show);
}

static void cmd_worn(int argc, const char **argv) {
    World *w = game_world();
    Game *g = game();
    if (!w || !g || !w->items || argc < 2) { log_info("usage: char_worn <head|-> [chest|-] [back|-] [slot]"); return; }
    int slot = argc > 4 ? atoi(argv[4]) : g->local_slot;
    if (slot < 0 || slot >= MAX_PLAYERS) return;
    u16 worn[CWORN_COUNT] = { 0 };
    for (int k = 0; k < CWORN_COUNT && k + 1 < argc; k++) {
        const char *id = argv[k + 1];
        if (!strcmp(id, "-") || !strcmp(id, "none")) continue;
        worn[k] = item_find(w->items, id);
        if (!worn[k]) log_warn("char_worn: no item '%s'", id);
    }
    memcpy(CPD.worn[slot], worn, sizeof(worn));
    CPD.worn_set[slot] = true;
    chars_set_worn(w, slot, worn);
    static const char *names[CWORN_COUNT] = { "head", "chest", "back" };
    const CharacterInst *c = w->players[slot].active ? char_get(w, w->players[slot].ent) : NULL;
    for (int k = 0; k < CWORN_COUNT; k++) {
        char fit[320];
        if (worn[k] && chars_worn_fit(w, c, worn[k], k, fit, sizeof(fit))) log_info("char_worn: slot %d %s: %s", slot, names[k], fit);
    }
}

static void cmd_check(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    CPD.check = true;
}

void char_preview_register(void) {
    cmd_register("char_preview_show", cmd_show, "char_preview_show [0|1|2|3] [w h] [yaw_deg] [held item id]: the inventory character preview over the screen (2: rendered, not shown; 3: shown, pose held still); reloads assets/ui/char_preview.json");
    cmd_register("char_worn", cmd_worn, "char_worn <head|-> [chest|-] [back|-] [slot]: worn gear item ids drawn on a player's character (default your own; '-' / none empties a slot), logs the fits");
    cmd_register("char_preview_check", cmd_check, "char_preview_check: next frame, the live player's character state before / after the preview must be identical (PASS / FAIL)");
}

static u64 fnv(u64 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// Everything of the live character the preview could touch: the instance (runtime, flags, item state,
// part cache), its skeleton instance (pose version, palette) and pose arrays, and the render camera.
static u64 live_hash(const CharacterInst *c, u32 *pose_version) {
    u64 h = 1469598103934665603ull;
    vec3 cp;
    float fov;
    render_camera_info(&cp, &fov);
    h = fnv(h, &cp, sizeof(cp));
    h = fnv(h, &fov, sizeof(fov));
    if (!c) return h;
    h = fnv(h, c, sizeof(*c));
    if (c->si) h = fnv(h, c->si, sizeof(*c->si));
    if (c->pose_prev) h = fnv(h, c->pose_prev, sizeof(Xform) * ANIM_MAX_BONES);
    if (c->pose_cur) h = fnv(h, c->pose_cur, sizeof(Xform) * ANIM_MAX_BONES);
    if (c->pose_draw) h = fnv(h, c->pose_draw, sizeof(Xform) * ANIM_MAX_BONES);
    *pose_version = c->si ? c->si->pose_version : 0;
    return h;
}

void char_preview_dev_draw(void) {
    World *w = game_world();
    Game *g = game();
    if (!w || !g) return;
    for (int s = 0; s < MAX_PLAYERS; s++)
        if (CPD.worn_set[s]) chars_set_worn(w, s, CPD.worn[s]);
    if (!CPD.show && !CPD.check) return;
    // Default: the screen's height (the head top ~21 % down, the bottom edge at mid-thigh, like the
    // inventory reference shot), bottom-aligned and centred.
    float W = ui_width(), H = ui_height();
    int ph = CPD.h ? CPD.h : (int)H, pw = CPD.w ? CPD.w : (int)lrintf((float)ph * 0.85f);
    CharPreviewIn in;
    memset(&in, 0, sizeof(in));
    in.w = pw;
    in.h = ph;
    in.yaw = CPD.yaw;
    in.held_item = CPD.held[0] && w->items ? item_find(w->items, CPD.held) : 0;
    memcpy(in.worn, CPD.worn[g->local_slot], sizeof(in.worn));
    const Player *lp = game_local_player();
    const CharacterInst *live = lp ? char_get(w, lp->ent) : NULL;
    u32 v0 = 0, v1 = 0;
    u64 h0 = CPD.check ? live_hash(live, &v0) : 0;
    TextureId tex = chars_preview(w, g->local_slot, &in);
    if (CPD.check) {
        u64 h1 = live_hash(live, &v1);
        char info[256];
        char_preview_info(info, sizeof(info));
        log_info("char_preview_check: %s: live player %s (state hash %016llx -> %016llx, pose_version %u -> %u, item %s), preview texture %u, %s",
                 h0 == h1 && live ? "PASS" : "FAIL", live ? "unchanged" : "missing", (unsigned long long)h0, (unsigned long long)h1, v0, v1,
                 live && live->item_valid ? "held" : "none", tex, info);
        CPD.check = false;
    }
    if ((CPD.show != 1 && CPD.show != 3) || !tex) return;
    gd_backdrop((GuiRect){ 0, 0, W, H }, UI_RGBA(40, 42, 44, 255));
    gd_rect((GuiRect){ 0, 0, W, H }, UI_RGBA(18, 19, 20, 150));
    gd_image((GuiRect){ floorf((W - (float)pw) * 0.5f), H - (float)ph, (float)pw, (float)ph }, tex, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
}
