// Weapon bench (world object): free attachment changes for the equipped weapon, one row per slot
// (cycle with the arrows or < >), effective stats against the bare weapon. RPC_ATTACH (host
// validated). Attachments from the inventory are mounted by drag and drop in the inventory.
#include "screens.h"
#include "core/engine.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "gameplay/attachments.h"
#include "player/camera.h"
#include <math.h>
#include <stdio.h>

#define TH (gui_theme())

static void stat_row(GuiCursor *c, const char *label, const char *value, float delta_pct) {
    GuiRect r = gc_row(c, gui_s(26));
    gw_label(gui_rect(r.x, r.y, r.w * 0.5f, r.h), label, gui_s(TH->t_body - 1), GW_REGULAR, TH->text_dim, GUI_LEFT);
    u32 col = TH->text;
    char buf[96];
    if (fabsf(delta_pct) > 0.5f) {
        col = delta_pct > 0 ? TH->ok : TH->danger;
        snprintf(buf, sizeof(buf), "%s  (%+.0f%%)", value, delta_pct);
    } else {
        snprintf(buf, sizeof(buf), "%s", value);
    }
    gw_label(gui_rect(r.x + r.w * 0.5f, r.y, r.w * 0.5f, r.h), buf, gui_s(TH->t_body - 1), GW_MEDIUM, col, GUI_RIGHT);
}

bool bench_draw(GuiRect r) {
    Game *g = game();
    World *w = g ? g->world : NULL;
    Player *p = game_local_player();
    if (!p || !w) return true;
    if (ent_valid(w, g->bench_ent) && v3_dist(p->sim.move.pos, w->xf[ENT_INDEX(g->bench_ent)].pos) > 3.5f) return true;   // walked away
    int inv_slot = DG_CLAMP((int)p->sim.wpn.slot, 0, INV_HOTBAR - 1);
    const InvSlot *s = inv_hot_c(&p->inv, inv_slot);
    const WeaponDef *wd = item_weapon(w->items, s->item);
    GuiRect c = screens_modal_frame(r, "Weapon bench", wd ? item_def(w->items, s->item)->name : NULL);
    GuiCursor cur = gc_make(c, gui_s(4));
    if (!wd || wd->kind == WPN_MELEE) {
        gw_label(gc_row(&cur, gui_s(30)), "Equip a gun (1-6) to fit attachments.", gui_s(TH->t_body), GW_REGULAR, TH->warning, GUI_LEFT);
        return false;
    }
    gw_section(gc_row(&cur, gui_s(26)), "Attachments");
    for (int k = 0; k < ATT_SLOTS; k++) {
        if (!wd->att_slots[k].present) continue;
        u8 opts[MAX_ATTACHMENTS + 1];
        int n = 0, sel = 0;
        opts[n++] = 0;
        for (int i = 1; i <= w->items->attachment_count; i++) {
            const AttachmentDef *a = &w->items->attachments[i];
            if (a->slot == k && attach_allowed(wd, a)) {
                if (s->att[k] == i) sel = n;
                opts[n++] = (u8)i;
            }
        }
        const char *names[MAX_ATTACHMENTS + 1];
        for (int i = 0; i < n; i++) names[i] = opts[i] ? attach_def(w->items, opts[i])->name : "None";
        char label[48];
        snprintf(label, sizeof(label), "%c%s", attach_slot_name(k)[0] - 32, attach_slot_name(k) + 1);
        GuiRect rr = gw_setting_row(&cur, label, NULL, gui_idi("bench_row", k));
        rr.y += gui_s(3);
        rr.h -= gui_s(6);
        if (gw_dropdown(gui_idi("bench_dd", k), rr, names, n, &sel)) game_rpc(RPC_ATTACH, inv_slot, k, opts[sel]);
    }
    WeaponDef eff;
    attach_weapon_effective(w->items, wd, s->att, &eff);
    const AttachmentDef *optic = attach_optic(w->items, s->att);
    cur.y += gui_s(10);
    gw_section(gc_row(&cur, gui_s(26)), "Effective stats");
    char v[64];
    snprintf(v, sizeof(v), "%s, %.1fx", optic ? optic->name : "Iron sights", optic ? optic->zoom : 75.0f / fmaxf(1, wd->ads_fov));
    stat_row(&cur, "Sight", v, 0);
    snprintf(v, sizeof(v), "%d", eff.mag_size);
    stat_row(&cur, "Magazine", v, wd->mag_size ? 100.0f * (float)(eff.mag_size - wd->mag_size) / (float)wd->mag_size : 0);
    snprintf(v, sizeof(v), "%.2f s", eff.recoil.ads_time);
    stat_row(&cur, "Aim time", v, -100.0f * (eff.recoil.ads_time - wd->recoil.ads_time) / fmaxf(1e-4f, wd->recoil.ads_time));
    snprintf(v, sizeof(v), "%.0f%%", 100.0f * eff.recoil.v_per / fmaxf(1e-4f, wd->recoil.v_per));
    stat_row(&cur, "Vertical recoil", v, -100.0f * (eff.recoil.v_per - wd->recoil.v_per) / fmaxf(1e-4f, wd->recoil.v_per));
    snprintf(v, sizeof(v), "%.0f%%", 100.0f * eff.recoil.h_per / fmaxf(1e-4f, wd->recoil.h_per));
    stat_row(&cur, "Horizontal recoil", v, -100.0f * (eff.recoil.h_per - wd->recoil.h_per) / fmaxf(1e-4f, wd->recoil.h_per));
    snprintf(v, sizeof(v), "%.1f", eff.damage);
    stat_row(&cur, "Damage", v, 100.0f * (eff.damage - wd->damage) / fmaxf(1e-4f, wd->damage));
    snprintf(v, sizeof(v), "%.0f m", eff.range);
    stat_row(&cur, "Range", v, 100.0f * (eff.range - wd->range) / fmaxf(1e-4f, wd->range));
    screens_footer_hint(gui_rect(c.x, c.y + c.h - gui_s(18), c.w, gui_s(18)), "Esc / B  close");
    return false;
}
