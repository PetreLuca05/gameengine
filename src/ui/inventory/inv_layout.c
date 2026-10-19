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
    o->chip_gap = 6 * s;
    float g = o->gap;
    // Header: the title centred; the category bar right of it when the half beside the title holds it
    // (~700 design px), else on a second row. The divider is the header's bottom edge.
    float title_half = 90 * s, row_h = 44 * s, cats_h = 26 * s;
    float beside = S.w * 0.5f - title_half - g;
    if (beside >= 700 * s) {
        o->header = R(S.x, S.y, S.w, row_h);
        o->cats = R(S.x + S.w - beside, S.y + (row_h - cats_h) * 0.5f, beside, cats_h);
    } else {
        o->header = R(S.x, S.y, S.w, row_h + cats_h + g * 0.5f);
        o->cats = R(S.x, S.y + row_h, S.w, cats_h);
    }
    float by = o->header.y + o->header.h + g;
    // Columns: the grid panel ~30 % (ten cells) and its gear column on the left, the weapon slots
    // ~24 % on the right, the character the middle (at least 300 px).
    float lw = clampf_(S.w * 0.24f, 280 * s, 460 * s), rw = clampf_(S.w * 0.30f, 320 * s, 600 * s), gw = clampf_(rw * 0.22f, 72 * s, 132 * s);
    float cw = S.w - lw - rw - gw - 3 * g;
    if (cw < 300 * s) {
        float k = (S.w - 3 * g - 300 * s) / (lw + rw + gw);
        lw *= k;
        rw *= k;
        gw *= k;
        cw = 300 * s;
    }
    // The grid panel and the gear column go down to the bottom of the safe area; the footer runs
    // under the centre and the weapon slots.
    float bottom = S.y + S.h;
    o->panel = R(S.x, by, rw, bottom - by);
    o->gear = R(S.x + rw + g, by, gw, bottom - by);
    float fx = o->gear.x + gw + g;
    o->footer = R(fx, bottom - 24 * s, S.x + S.w - fx, 24 * s);
    float bh = o->footer.y - g - by;
    o->center = R(fx, by, cw, bh);
    o->slots = R(S.x + S.w - lw, by, lw, bh);
    // One cell size everywhere: ten cells (or the widest grid) across the grid panel. Tall grids
    // scroll; the cells never shrink to fit the height.
    int cols = DG_MAX(INV_UI_GRID_COLS, DG_MAX(in->grid_cols, in->loot ? in->loot_cols : 0));
    o->cell = DG_MIN(58 * s, (rw - 2 * o->pad) / (float)cols);
    float cell = o->cell;
    // Right: primary + its chips, secondary + its chips, holster. The chips are squares across the
    // slot's width; the weapon slots are as tall as the column allows (a live preview each).
    o->chip = (lw - (float)(ATT_SLOTS - 1) * o->chip_gap) / (float)ATT_SLOTS;
    float chips = INV_UI_WEAPON_ROWS * (o->chip + 1.5f * g) + 4 * g;
    float weap_h = clampf_((bh - chips) / 2.85f, 64 * s, 210 * s);
    float x = o->slots.x, y = o->slots.y;
    static const int ws[3] = { EQ_PRIMARY, EQ_SECONDARY, EQ_HOLSTER };
    for (int i = 0; i < 3; i++) {
        float h = i == 2 ? weap_h * 0.85f : weap_h;
        o->equip[ws[i]] = R(x, y, lw, h);
        y += h + g * 0.5f;
        if (i < INV_UI_WEAPON_ROWS) {
            o->att_row[i] = R(x, y, lw, o->chip);
            y += o->chip + g;
        }
    }
    // The gear column: helmet, body armor, backpack, square, stacked from the top.
    static const int gs[3] = { EQ_HEADWEAR, EQ_BODY_ARMOR, EQ_BACKPACK };
    for (int i = 0; i < 3; i++) o->equip[gs[i]] = R(o->gear.x, by + (float)i * (gw + g), gw, gw);
    // Centre: the character fills it; the discard zone along its bottom.
    o->player = o->center;
    float discard_h = 30 * s;
    o->discard = R(o->center.x + cw * 0.3f, o->center.y + bh - discard_h, cw * 0.4f, discard_h);
    // The grid panel: (scrolling) the world container above the player's grid.
    o->view = R(o->panel.x + o->pad, o->panel.y + o->pad * 0.5f, rw - 2 * o->pad, o->panel.h - o->pad);
    float cy = o->view.y;
    if (in->loot) {
        float eq = in->loot_body ? 1.6f * cell + g : 0.0f;
        int lr = DG_MAX(in->loot_rows, 1);
        float th = o->title_h + eq + (float)lr * cell + g + 34 * s;   // title, equipment, grid, Take all
        o->loot = R(o->view.x, cy, o->view.w, th);
        cy += th + 1.5f * g;
    }
    o->grid = R(o->view.x, cy, o->view.w, o->title_h + (float)DG_MAX(in->grid_rows, 1) * cell);
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
