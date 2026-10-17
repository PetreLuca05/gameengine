// Player tab (docs/MENU_LOBBY.md): the 3D preview is the menu scene (mm_scene.c frames the real
// character - shared skeleton, equipped cosmetics, idle animation, the item on its hold point -
// inside the preview rect; drag to orbit, wheel to zoom), name, stats, loadout, and the "Preview in
// map lighting" toggle (the selected map's season / time / weather, else neutral daylight).
#include "mm_internal.h"
#include "game/game.h"
#include "gameplay/items.h"
#include "core/cvar.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Mouse / pad orbit over a rect: drag rotates, wheel zooms, the right stick or Q-less arrows too.
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
            M->orbit_pitch = clampf(M->orbit_pitch + (m.y - last.y) * 0.004f, -0.6f, 0.5f);
        }
    }
    last = gui_mouse();
    if (gui_rect_contains(r, gui_mouse().x, gui_mouse().y) && gui_wheel() != 0) {
        M->orbit_zoom = clampf(M->orbit_zoom * (1.0f + gui_wheel() * 0.1f), 0.6f, 2.6f);
        gui_wheel_consume();
    }
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
    // Preview bar: the lighting toggle + a hint.
    GuiRect bar = mm_node(L, "preview_bar"), a, b;
    gui_split_h(bar, 0.5f, TOK("mm_gap"), &a, &b);
    bool tod = M->preview_tod;
    if (gw_toggle(gui_id("mm_preview_tod"), gui_rect(a.x, a.y, gui_s(TH->button_h * 3), a.h), &tod)) { M->preview_tod = tod; M->env_dirty = true; }
    gw_label(gui_rect(a.x + gui_s(TH->button_h * 3.2f), a.y, a.w, a.h), "Preview in map lighting", gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT);
    gw_label(b, "Drag to rotate  ·  wheel to zoom", gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT);

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
    char v[48];
    snprintf(v, sizeof(v), "%u", p->kills);
    mm_kv(&c, "Kills", v);
    snprintf(v, sizeof(v), "%u", p->deaths);
    mm_kv(&c, "Deaths", v);
    snprintf(v, sizeof(v), "%u", p->matches);
    mm_kv(&c, "Matches", v);
    snprintf(v, sizeof(v), "%.1f h", p->playtime_s / 3600.0);
    mm_kv(&c, "Play time", v);
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
        const InvSlot *is = slot >= 0 && slot < INV_SLOTS ? &lp->inv.slots[slot] : NULL;
        const ItemDef *def = is && is->item && g_engine.items ? item_def(g_engine.items, is->item) : NULL;
        mm_kv(&c, "In hand", def ? def->name : "Nothing");
    }
    if (gw_button(gui_id("mm_to_cos"), gc_row(&c, TOK("mm_button_h")), "Change cosmetics", GB_SECONDARY, true)) { M->tab = MT_COSMETICS; gui_set_focus(0); }
}
