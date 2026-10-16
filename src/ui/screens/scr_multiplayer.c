// Multiplayer modal: host on LAN / Steam, direct connect, LAN server browser.
#include "screens.h"
#include "ui/menus.h"
#include "core/cvar.h"
#include "game/game.h"
#include "net/net.h"
#include <stdio.h>
#include <string.h>

#define TH (gui_theme())

static char g_addr[64] = "127.0.0.1:27015";

bool multiplayer_draw(GuiRect r) {
    NetRole role = net_role();
    char sub[128];
    snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", role == NET_HOST ? "Hosting" : role == NET_CLIENT ? "Connected" : "Offline", net_transport_name());
    GuiRect c = screens_modal_frame(r, "Multiplayer", sub);
    GuiCursor cur = gc_make(c, gui_s(6));
    gw_section(gc_row(&cur, gui_s(26)), "Host");
    GuiRect row = gc_row(&cur, gui_s(TH->button_h + 6));
    GuiRect a, b;
    gui_split_h(row, 0.5f, gui_s(10), &a, &b);
    if (gw_button(gui_id("mp_host_lan"), a, role == NET_HOST ? "LAN hosting active" : "Host LAN game", GB_PRIMARY, role != NET_HOST)) {
        if (game()->mode != GM_HOST) game_host(game()->g_map->str);
        net_host(TRANSPORT_LAN, (u16)cvar_find("net_port")->i);
    }
    bool steam = net_steam_available();
    if (gw_button(gui_id("mp_host_steam"), b, steam ? "Host Steam lobby" : "Steam unavailable", GB_SECONDARY, steam)) {
        if (game()->mode != GM_HOST) game_host(game()->g_map->str);
        net_host(TRANSPORT_STEAM, 0);
    }
    cur.y += gui_s(8);
    gw_section(gc_row(&cur, gui_s(26)), "Direct connect");
    row = gc_row(&cur, gui_s(TH->button_h + 6));
    gui_split_h(row, 0.72f, gui_s(10), &a, &b);
    bool enter = gw_textfield(gui_id("mp_addr"), a, g_addr, (int)sizeof(g_addr), "ip:port");
    if (gw_button(gui_id("mp_connect"), b, "Connect", GB_PRIMARY, g_addr[0] != 0) || enter) {
        char addr[64];
        dg_strcpy(addr, sizeof(addr), g_addr);
        menus_close_all();
        net_connect(TRANSPORT_LAN, addr);
        return false;
    }
    cur.y += gui_s(8);
    GuiRect hdr = gc_row(&cur, gui_s(26));
    gw_section(gui_rect(hdr.x, hdr.y, hdr.w - gui_s(110), hdr.h), "LAN games");
    if (gw_button(gui_id("mp_refresh"), gui_rect(hdr.x + hdr.w - gui_s(100), hdr.y, gui_s(100), hdr.h), "Refresh", GB_GHOST, true)) net_lan_refresh();
    const LanServer *servers;
    int n = net_lan_servers(&servers);
    if (!n) gw_label(gc_row(&cur, gui_s(30)), "No LAN games found (broadcast on port 27016).", gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    for (int i = 0; i < n; i++) {
        GuiRect sr = gc_row(&cur, gui_s(40));
        GuiId id = gui_idi("mp_server", i);
        GuiInteract in = gui_interact(id, sr, GUI_FOCUSABLE);
        if (in.hot) gd_round(sr, gui_s(TH->radius), TH->hover);
        if (in.focused) gw_focus_ring(sr, gui_s(TH->radius), 1);
        char line[160];
        snprintf(line, sizeof(line), "%s", servers[i].name);
        gw_label(gui_rect(sr.x + gui_s(10), sr.y, sr.w * 0.45f, sr.h), line, gui_s(TH->t_body), GW_MEDIUM, TH->text, GUI_LEFT);
        snprintf(line, sizeof(line), "%s  \xC2\xB7  %d/%d  \xC2\xB7  %.0f ms", servers[i].map, servers[i].players, servers[i].max_players, servers[i].ping_ms);
        gw_label(gui_rect(sr.x + sr.w * 0.45f, sr.y, sr.w * 0.55f - gui_s(10), sr.h), line, gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_RIGHT);
        if (in.clicked || in.double_clicked) {
            char addr[64];
            dg_strcpy(addr, sizeof(addr), servers[i].address);
            menus_close_all();
            net_connect(TRANSPORT_LAN, addr);
            return false;
        }
    }
    screens_footer_hint(gui_rect(c.x, c.y + c.h - gui_s(18), c.w, gui_s(18)), "Esc / B  back");
    return false;
}
