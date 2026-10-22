// Player tab (docs/MENU_LOBBY.md): the character is the menu background (mm_scene.c; a drag over the
// preview area turns it), name, stats, loadout, and the Phone card (mm_phone.c).
#include "mm_internal.h"
#include "game/game.h"
#include "gameplay/items.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "gameplay/stats/stats.h"
#include "ui/ui_chart.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Mouse / pad turn over a rect: drag rotates (eases back on release), the right stick too.
static void orbit_input(GuiRect r) {
    MainMenu *M = mm();
    GuiId id = gui_id("mm_preview");
    GuiInteract in = gui_interact(id, r, 0);
    if (in.pressed) gui_set_active(id);
    static vec2 last;
    if (gui_active() == id) {
        if (!gui_mouse_down(0)) gui_set_active(0);
        else {
            vec2 m = gui_mouse();
            M->orbit_yaw -= (m.x - last.x) * 0.008f;
        }
    }
    last = gui_mouse();
    vec2 st = gui_pad_stick();
    if (gui_nav_mode() && fabsf(st.x) > 0.01f) M->orbit_yaw -= st.x * gui_dt() * 2.0f;
}

void mm_player_draw(GuiRect content) {
    MainMenu *M = mm();
    MmLayout *L = &M->player;
    mm_layout_solve(L, content);
    GuiRect area = mm_node(L, "preview_area");
    M->preview_rect = area;
    orbit_input(area);
    // Preview bar: a hint.
    GuiRect bar = mm_node(L, "preview_bar");
    gw_label(bar, "Drag to rotate", gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT);

    GuiRect info = mm_node(L, "info");
    mm_panel(info);
    GuiRect in = gui_rect_inset(info, gui_s(TH->pad));
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    const Profile *p = lobby_profile();
    GuiRect head = gc_row(&c, TOK("mm_title_h"));
    float av = TOK("mm_avatar");
    gw_avatar(gui_rect(head.x, head.y + (head.h - av) * 0.5f, av, av), M->avatar, lobby_local_name(), lobby_steam_running() ? TH->info : TH->type_misc);
    gw_label(gui_rect(head.x + av + TOK("mm_gap") * 1.5f, head.y, head.w - av, head.h), lobby_local_name(), gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT);
    if (!lobby_steam_running()) {
        // LAN name (the profile; cl_name in game).
        GuiRect nr = gw_setting_row(&c, "LAN name", "Your name on the local network.", gui_id("mm_lan_name"));
        Game *g = game();
        static char name[32];
        static bool init;
        if (!init && g) { dg_strcpy(name, sizeof(name), g->cl_name->str); init = true; }
        if (gw_textfield(gui_id("mm_lan_name_f"), nr, name, (int)sizeof(name), "Survivor") && name[0] && g) {
            cvar_set_cv(g->cl_name, name);
            dg_strcpy(lobby_profile()->lan_name, sizeof(lobby_profile()->lan_name), name);
            lobby_profile_changed();
        }
    }
    c.y += TOK("mm_gap");
    gw_section(gc_row(&c, TOK("mm_row_h")), "Stats");
    // The player statistics (docs/STATS.md); the Stats tab has the rest.
    const PlayerStats *ps = &stats_get()->life;
    char v[48];
    mm_kv(&c, "Kills", chart_fmt_int(v, sizeof(v), ps->kills));
    snprintf(v, sizeof(v), "%.2f", stats_kd(ps));
    mm_kv(&c, "K/D", v);
    mm_kv(&c, "Matches", chart_fmt_int(v, sizeof(v), ps->matches_played));
    mm_kv(&c, "Play time", chart_fmt_duration(v, sizeof(v), ps->time_played_sec));
    c.y += TOK("mm_gap");
    gw_section(gc_row(&c, TOK("mm_row_h")), "Loadout");
    const CosmeticDB *db = lobby_cosmetics();
    for (int s = 0; s < COS_SLOTS; s++) {
        const CosmeticItem *it = cosmetics_find(db, p->loadout.equipped[s]);
        if (!it) continue;
        mm_kv(&c, cosmetics_slot_name(s), it->name);
    }
    // The held item (what the hold point shows on the preview).
    Player *lp = game_local_player();
    if (lp) {
        int slot = lp->sim.wpn.slot;
        const InvSlot *is = slot >= 0 && slot < INV_SEL_COUNT ? inv_sel_c(&lp->inv, slot) : NULL;
        const ItemDef *def = is && is->item && g_engine.items ? item_def(g_engine.items, is->item) : NULL;
        mm_kv(&c, "In hand", def ? def->name : "Nothing");
    }
    if (gw_button(gui_id("mm_to_cos"), gc_row(&c, TOK("mm_button_h")), "Change cosmetics", GB_SECONDARY, true)) { M->tab = MT_COSMETICS; gui_set_focus(0); }
    // The Phone card (docs/PHONE.md "Phone flow"): pairing happens here, not when the phone comes out.
    mainmenu_phone_card(mm_node(L, "phone"));
}
