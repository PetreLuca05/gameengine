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
    // Columns: the slots ~24 %, the grids ~30 % (ten cells), the character the middle (at least 300 px).
    float lw = clampf_(S.w * 0.24f, 280 * s, 460 * s), rw = clampf_(S.w * 0.30f, 320 * s, 600 * s);
    float cw = S.w - lw - rw - 2 * g;
    if (cw < 300 * s) {
        float k = (S.w - 2 * g - 300 * s) / (lw + rw);
        lw *= k;
        rw *= k;
        cw = 300 * s;
    }
    o->left = R(S.x, by, lw, bh);
    o->right = R(S.x + S.w - rw, by, rw, bh);
    o->center = R(S.x + lw + g, by, cw, bh);
    // One cell size everywhere: ten cells (or the widest grid) across the right column. Tall grids
    // scroll; the cells never shrink to fit the height.
    int cols = DG_MAX(INV_UI_GRID_COLS, DG_MAX(in->grid_cols, in->loot ? in->loot_cols : 0));
    o->cell = DG_MIN(58 * s, (rw - 2 * o->pad) / (float)cols);
    float cell = o->cell;
    // Left: primary + its chips, secondary + its chips, holster, the gear row, the inspect panel.
    float x = o->left.x, w = o->left.w, y = o->left.y;
    float weap_h = clampf_(1.9f * cell, 56 * s, 112 * s), chip = clampf_(0.82f * cell, 30 * s, 48 * s);
    static const int ws[3] = { EQ_PRIMARY, EQ_SECONDARY, EQ_HOLSTER };
    for (int i = 0; i < 3; i++) {
        float h = i == 2 ? weap_h * 0.8f : weap_h;
        o->equip[ws[i]] = R(x, y, w, h);
        y += h + g * 0.5f;
        if (i < INV_UI_WEAPON_ROWS) {
            o->att_row[i] = R(x, y, w, chip);
            y += chip + g;
        } else {
            y += g * 0.5f;
        }
    }
    float q = DG_MIN((w - 2 * g) / 3.0f, 2.2f * cell);
    static const int gs[3] = { EQ_HEADWEAR, EQ_BODY_ARMOR, EQ_BACKPACK };
    for (int i = 0; i < 3; i++) o->equip[gs[i]] = R(x + (float)i * (q + g), y, q, q);
    y += q + g;
    o->inspect = R(x, y, w, DG_MAX(0.0f, o->left.y + o->left.h - y));
    // Centre: the character fills it; the discard zone along its bottom.
    o->player = o->center;
    float discard_h = 30 * s;
    o->discard = R(o->center.x + cw * 0.3f, o->center.y + bh - discard_h, cw * 0.4f, discard_h);
    // Right: the category bar, then (scrolling) the world container above the player's grid.
    o->cats = R(o->right.x, o->right.y, rw, 2 * 24 * s + g * 0.5f);
    float vy = o->cats.y + o->cats.h + g;
    o->view = R(o->right.x, vy, rw, o->right.y + o->right.h - vy);
    float cy = o->view.y;
    if (in->loot) {
        float eq = in->loot_body ? 1.6f * cell + g : 0.0f;
        int lr = DG_MAX(in->loot_rows, 1);
        float th = o->title_h + eq + (float)lr * cell + o->pad + 34 * s;   // title, equipment, grid, Take all
        o->loot = R(o->right.x, cy, rw, th);
        cy += th + g;
    }
    o->grid = R(o->right.x, cy, rw, o->title_h + (float)DG_MAX(in->grid_rows, 1) * cell + o->pad);
    cy += o->grid.h;
    o->content_h = cy - o->view.y;
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
