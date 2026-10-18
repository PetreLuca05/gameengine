#include "inv_ui.h"
#include <string.h>

void inv_container_extent(const InvContainerDef *cd, int *cols, int *rows) {
    int c = 0, r = 0;
    for (int i = 0; cd && i < cd->comp_count; i++) {
        c = DG_MAX(c, cd->comp[i].at_x + cd->comp[i].w);
        r = DG_MAX(r, cd->comp[i].at_y + cd->comp[i].h);
    }
    *cols = DG_MAX(1, c);
    *rows = DG_MAX(1, r);
}

static GuiRect R(float x, float y, float w, float h) { GuiRect r = { x, y, w, h }; return r; }
static float clampf_(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void inv_layout(const InvLayoutIn *in, InvLayout *o) {
    memset(o, 0, sizeof(*o));
    float s = in->scale > 0 ? in->scale : 1.0f;
    GuiRect S = in->safe;
    o->pad = 12 * s;
    o->gap = 10 * s;
    o->title_h = 26 * s;
    float g = o->gap;
    o->header = R(S.x, S.y, S.w, 44 * s);
    o->footer = R(S.x, S.y + S.h - 24 * s, S.w, 24 * s);
    float by = o->header.y + o->header.h + g, bh = o->footer.y - g - by;
    // Columns: the grids and the hotbar take ~30 % each, the player the middle (at least 300 px).
    float side = clampf_(S.w * 0.3f, 260 * s, 560 * s);
    float center_w = S.w - 2 * side - 2 * g;
    if (center_w < 300 * s) {
        side = (S.w - 2 * g - 300 * s) * 0.5f;
        center_w = 300 * s;
    }
    o->left = R(S.x, by, side, bh);
    o->right = R(S.x + S.w - side, by, side, bh);
    o->center = R(S.x + side + g, by, center_w, bh);
    // One cell size everywhere: the widest player grid fits the left column, the loot grid the right.
    int cols = 6;
    for (int i = 0; i < in->panel_n && i < INV_UI_MAX_PANELS; i++) cols = DG_MAX(cols, in->panel[i].cols);
    float cell = DG_MIN(58 * s, (side - 2 * o->pad) / (float)cols);
    if (in->loot) cell = DG_MIN(cell, (side - 2 * o->pad) / (float)DG_MAX(4, in->loot_cols));
    o->cell = cell;
    // Left: the player's grids stacked; the column scrolls when they don't fit.
    float y = o->left.y;
    for (int i = 0; i < in->panel_n && i < INV_UI_MAX_PANELS; i++) {
        float h = o->title_h + (float)in->panel[i].rows * cell + o->pad;
        o->panel[i] = R(o->left.x, y, side, h);
        y += h + g;
    }
    o->left_content_h = in->panel_n ? y - g - o->left.y : 0;
    // Centre: the discard zone at the bottom, the weapon slots above it, the flanks around the player.
    float discard_h = 30 * s;
    o->discard = R(o->center.x + center_w * 0.3f, o->center.y + bh - discard_h, center_w * 0.4f, discard_h);
    float weap_h = clampf_(1.9f * cell, 48 * s, 120 * s);
    float wy = o->discard.y - g - weap_h;
    static const float wt[4] = { 4.0f, 4.0f, 2.2f, 1.6f };
    static const int ws[4] = { EQ_PRIMARY, EQ_SECONDARY, EQ_HOLSTER, EQ_MELEE };
    float wsum = wt[0] + wt[1] + wt[2] + wt[3], x = o->center.x, avail = center_w - 3 * g;
    for (int i = 0; i < 4; i++) {
        float w = avail * wt[i] / wsum;
        o->equip[ws[i]] = R(x, wy, w, weap_h);
        x += w + g;
    }
    float top = o->center.y, ph = wy - g - top;
    float q = DG_MIN(2.1f * cell, (ph - 4 * g) / 5.0f);
    q = DG_MIN(q, (center_w - 2 * g) * 0.22f);
    static const int lf[5] = { EQ_HEADWEAR, EQ_EARPIECE, EQ_FACE_COVER, EQ_EYEWEAR, EQ_ARMBAND };
    static const int rf[4] = { EQ_BODY_ARMOR, EQ_RIG, EQ_BACKPACK, EQ_SECURE };
    for (int i = 0; i < 5; i++) o->equip[lf[i]] = R(o->center.x, top + (float)i * (q + g), q, q);
    for (int i = 0; i < 4; i++) o->equip[rf[i]] = R(o->center.x + center_w - q, top + (float)i * (q + g), q, q);
    o->player = R(o->center.x + q + g, top, center_w - 2 * (q + g), ph);
    // Right: the hotbar (INV_HOTBAR large tiles; a row of squares with a world container open, the loot below).
    float hy = o->right.y + o->title_h;
    if (!in->loot) {
        float th = DG_MIN((o->right.h - o->title_h - (INV_HOTBAR - 1) * g) / (float)INV_HOTBAR, 2.6f * cell);
        for (int i = 0; i < INV_HOTBAR; i++) o->hot[i] = R(o->right.x, hy + (float)i * (th + g), side, th);
    } else {
        o->hot_compact = true;
        float sq = DG_MIN((side - (INV_HOTBAR - 1) * g) / (float)INV_HOTBAR, 1.6f * cell);
        for (int i = 0; i < INV_HOTBAR; i++) o->hot[i] = R(o->right.x + (float)i * (sq + g), hy, sq, sq);
        float ly = hy + sq + 1.5f * g;
        o->loot = R(o->right.x, ly, side, o->right.y + o->right.h - ly);
    }
}

int inv_nav_step(const GuiRect *rects, int n, int cur, int dir) {
    if (cur < 0 || cur >= n) return n > 0 ? 0 : -1;
    const GuiRect *c = &rects[cur];
    float cx = c->x + c->w * 0.5f, cy = c->y + c->h * 0.5f;
    int best = -1;
    float best_score = 1e30f;
    for (int i = 0; i < n; i++) {
        if (i == cur) continue;
        const GuiRect *r = &rects[i];
        float rx = r->x + r->w * 0.5f, ry = r->y + r->h * 0.5f;
        float along, side;
        switch (dir) {
        case 0: along = cx - rx; side = ry - cy; break;
        case 1: along = rx - cx; side = ry - cy; break;
        case 2: along = cy - ry; side = rx - cx; break;
        default: along = ry - cy; side = rx - cx; break;
        }
        if (along <= 0.5f) continue;
        float score = along + 2.0f * (side < 0 ? -side : side);
        if (score < best_score) { best_score = score; best = i; }
    }
    return best;
}
