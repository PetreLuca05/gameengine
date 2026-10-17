// Cosmetics tab (docs/MENU_LOBBY.md): categories (the character's part slots + weapon skins), the
// owned / locked items as inventory slots (gw_slot) with their rarity, equip / unequip; the
// character in the middle wears the choice at once (cl_skin / cl_parts -> the replicated part
// selection). Unlocks are data (assets/cosmetics/*.json "unlock") plus the profile; Steam Inventory
// is a hook (cosmetics_steam_owns).
#include "mm_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static GuiScroll g_grid_scroll;

static u32 rarity_color(int r) { return TH->rarity[DG_CLAMP(r, 0, 4)]; }

static int slot_item_count(const CosmeticDB *db, int slot) {
    int n = 0;
    for (int i = 0; i < db->count; i++) if (db->items[i].slot == slot) n++;
    return n;
}

static void equip_toggle(const CosmeticItem *it) {
    Profile *p = lobby_profile();
    const CosmeticDB *db = lobby_cosmetics();
    if (cosmetics_is_equipped(&p->loadout, it->id)) cosmetics_unequip(&p->loadout, it->slot);
    else if (!cosmetics_equip(db, &p->loadout, &p->unlocks, it->id)) { gui_sound(TH->snd_error); return; }
    gui_sound(TH->snd_equip);
    lobby_profile_changed();
}

void mm_cosmetics_draw(GuiRect content) {
    MainMenu *M = mm();
    MmLayout *L = &M->cosmetics;
    mm_layout_solve(L, content);
    const CosmeticDB *db = lobby_cosmetics();
    Profile *p = lobby_profile();
    GuiRect prev = mm_node(L, "preview");
    M->preview_rect = prev;
    // Orbit over the free middle (the same control as the Player tab).
    {
        GuiId id = gui_id("mm_cos_preview");
        GuiInteract in = gui_interact(id, prev, 0);
        static vec2 last;
        if (in.pressed) gui_set_active(id);
        if (gui_active() == id) {
            if (!gui_mouse_down(0)) gui_set_active(0);
            else M->orbit_yaw -= (gui_mouse().x - last.x) * 0.008f;
        }
        last = gui_mouse();
    }

    // Categories.
    GuiRect cats = mm_node(L, "cats");
    GuiCursor c = gc_make(cats, TOK("mm_gap_sm"));
    mm_title(gc_row(&c, TOK("mm_title_h")), "Cosmetics");
    float t = gui_anim_t(gui_id("mm_tab_in"));
    int k = 0;
    for (int s = 0; s < COS_SLOTS; s++) {
        if (!slot_item_count(db, s)) continue;
        float appear = saturate((t - (float)k++ * TH->menu_stagger) / fmaxf(0.01f, TH->menu_in));
        if (gw_menu_item(gui_idi("mm_cos_cat", s), gc_row(&c, TOK("mm_item_h")), cosmetics_slot_name(s), M->cos_slot == s, appear, true) && M->cos_slot != s) {
            M->cos_slot = s;
            M->cos_selected = -1;
            g_grid_scroll.target = g_grid_scroll.offset = 0;
        }
    }
    if (!slot_item_count(db, M->cos_slot)) for (int s = 0; s < COS_SLOTS; s++) if (slot_item_count(db, s)) { M->cos_slot = s; break; }

    // Grid.
    GuiRect grid = mm_node(L, "grid");
    mm_panel(grid);
    GuiRect gin = gui_rect_inset(grid, gui_s(TH->pad));
    gw_section(gui_rect(gin.x, gin.y, gin.w, TOK("mm_row_h")), cosmetics_slot_name(M->cos_slot));
    GuiRect view = { gin.x, gin.y + TOK("mm_row_h") + TOK("mm_gap"), gin.w - TOK("mm_gap"), gin.h - TOK("mm_row_h") - TOK("mm_gap") };
    float ss = gui_s(TH->slot), sg = gui_s(TH->slot_gap);
    int cols = DG_MAX(1, (int)((view.w + sg) / (ss + sg)));
    gw_scroll_begin(&g_grid_scroll, gui_id("mm_cos_grid"), view);
    float y0 = gw_scroll_y(&g_grid_scroll);
    int n = 0;
    for (int i = 0; i < db->count; i++) {
        const CosmeticItem *it = &db->items[i];
        if (it->slot != M->cos_slot) continue;
        GuiRect r = { view.x + (float)(n % cols) * (ss + sg), y0 + (float)(n / cols) * (ss + sg), ss, ss };
        n++;
        bool owned = cosmetics_owned(it, &p->unlocks), eq = cosmetics_is_equipped(&p->loadout, it->id);
        GuiId id = gui_idi("mm_cos_item", i);
        GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE);
        GuiSlotView v;
        memset(&v, 0, sizeof(v));
        v.icon = mm_texture(it->icon);
        v.uv = v4(0, 0, 1, 1);
        v.tint = it->tint;
        v.name = it->name;
        v.ammo_frac = -1;
        v.selected = M->cos_selected == i || eq;
        v.hot = in.hot;
        v.focus = in.focused;
        v.dim = !owned;
        gw_slot(r, &v);
        // Rarity: a hairline under the slot; equipped: a dot in the corner.
        gd_rect(gui_rect(r.x + gui_s(TH->radius), r.y + r.h - gui_s(TH->accent_bar), r.w - gui_s(TH->radius) * 2, gui_s(TH->accent_bar) * 0.66f), rarity_color(it->rarity));
        if (eq) gd_circle(r.x + r.w - gui_s(TH->gap), r.y + gui_s(TH->gap), gui_s(TH->radius), TH->accent);
        if (!owned) gw_label(gui_rect(r.x, r.y + gui_s(TH->gap * 0.5f), r.w - gui_s(TH->gap * 0.5f), TOK("lb_icon")), "LOCKED", gui_s(TH->t_tiny) * 0.8f, GW_SEMIBOLD, TH->text_faint, GUI_RIGHT);
        if (in.clicked) M->cos_selected = i;
        if (in.double_clicked && owned) equip_toggle(it);
        if (in.hovered) gui_tooltip(id, it->name);
    }
    gw_scroll_end(&g_grid_scroll, (float)((n + cols - 1) / cols) * (ss + sg));

    // Detail.
    GuiRect d = mm_node(L, "detail");
    mm_panel(d);
    GuiRect din = gui_rect_inset(d, gui_s(TH->pad));
    GuiCursor dc = gc_make(din, TOK("mm_gap_sm"));
    const CosmeticItem *it = M->cos_selected >= 0 && M->cos_selected < db->count ? &db->items[M->cos_selected] : NULL;
    if (!it || it->slot != M->cos_slot) {
        it = cosmetics_find(db, p->loadout.equipped[M->cos_slot]);
    }
    if (it) {
        bool owned = cosmetics_owned(it, &p->unlocks), eq = cosmetics_is_equipped(&p->loadout, it->id);
        gw_label(gc_row(&dc, TOK("mm_title_h")), it->name, gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT);
        GuiRect rr = gc_row(&dc, TOK("mm_row_h"));
        gw_label_caps(rr, cosmetics_rarity_name(it->rarity), gui_s(TH->t_label + 1), rarity_color(it->rarity), GUI_LEFT);
        mm_kv(&dc, "Slot", cosmetics_slot_name(it->slot));
        mm_kv(&dc, "Status", eq ? "Equipped" : (owned ? "Owned" : (it->unlock == CU_STEAM ? "Steam item (not owned)" : "Locked")));
        if (it->slot == COS_SLOT_WEAPON) gw_label(gc_row(&dc, TOK("mm_row_h")), "Weapon skins are not rendered yet.", gui_s(TH->t_small), GW_REGULAR, TH->warning, GUI_LEFT);
        if (it->description[0]) {
            GuiText b = { gui_s(TH->t_body - 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
            dc.y += TOK("mm_gap");
            dc.y += gd_text_wrap(&b, din.x, dc.y, din.w, it->description, true) + TOK("mm_gap");
        }
        GuiRect br = { din.x, din.y + din.h - TOK("mm_button_h"), din.w, TOK("mm_button_h") };
        if (gw_button(gui_id("mm_cos_equip"), br, eq ? "Unequip" : "Equip", eq ? GB_SECONDARY : GB_PRIMARY, owned)) equip_toggle(it);
    } else {
        gw_label(gc_row(&dc, TOK("mm_row_h")), "Nothing equipped in this slot", gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    }
    mm_footer_hint(gui_rect(content.x, content.y + content.h, content.w, TOK("mm_row_h")), "Enter / A  select     double click  equip     drag the character to turn it");
}
