// Loading screen (docs/MENU_LOBBY.md "Ready & start") and credits.
//   loading: the map's thumbnail and name, the session look (season, time, weather), a progress bar
//            (map load, then chunk streaming around the spawn), a tip;
//   credits: assets/ui/credits.txt (tools/gen_credits.py: the team + every third-party library and
//            its licence from third_party/), scrolling by itself, wheel / drag / keys take over.
#include "mm_internal.h"
#include "core/platform.h"
#include "world/season.h"
#include "game/game_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mm_loading_draw(void) {
    const Lobby *L = lobby_get();
    MmLayout *Ly = &mm()->loading;
    gd_rect(gui_rect(0, 0, gui_w(), gui_h()), UI_RGBA(6, 7, 8, 255));   // the world below is being replaced
    mm_layout_solve(Ly, gui_rect(0, 0, gui_w(), gui_h()));
    const MapInfo *mi = lobby_map_info(L->settings.map);
    GuiRect thumb = mm_node(Ly, "thumb");
    mm_thumb(thumb, mi ? mi->thumbnail : "");
    gd_rect_grad(gui_rect(thumb.x, thumb.y + thumb.h * 0.55f, thumb.w, thumb.h * 0.45f), UI_RGBA(0, 0, 0, 0), UI_RGBA(0, 0, 0, 160));
    GuiRect nr = mm_node(Ly, "name");
    GuiText t = { gui_s(TH->t_large), GW_SEMIBOLD, TH->text, GUI_LEFT, 0.04f, true, true, 0 };
    gd_text(&t, nr.x, nr.y + (nr.h - gd_line_height(t.size)) * 0.5f, mi ? mi->name : L->settings.map);
    const SeasonTable *st = weather_seasons();
    char look[96];
    snprintf(look, sizeof(look), "%s  ·  %02d:00  ·  %s", st->s[DG_CLAMP(L->settings.season, 0, SEASON_COUNT - 1)].name, L->settings.start_hour,
             L->settings.weather[0] ? L->settings.weather : "season weather");
    gw_label(nr, look, gui_s(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_RIGHT);
    // Progress.
    GuiRect bar = mm_node(Ly, "bar");
    float p = gui_tween(gui_id("mm_load_p"), lobby_load_progress(), 0.25f);
    gd_round(bar, bar.h * 0.5f, UI_RGBA(255, 255, 255, 24));
    gd_round(gui_rect(bar.x, bar.y, bar.w * saturate(p), bar.h), bar.h * 0.5f, TH->accent);
    const char *status = p < 0.38f ? (L->is_host ? "Loading the map..." : "Waiting for the host to load the map...") : (p < 0.99f ? "Streaming the world around you..." : "Spawning...");
    GuiRect sr = mm_node(Ly, "status");
    gw_label(sr, status, gui_s(TH->t_small + 1), GW_MEDIUM, TH->text_dim, GUI_LEFT);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)(saturate(p) * 100.0f));
    gw_label(sr, pct, gui_s(TH->t_small + 1), GW_MEDIUM, TH->text_dim, GUI_RIGHT);
    GuiRect tip = mm_node(Ly, "tip");
    GuiText tt = { gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT, 0, false, false, 0 };
    gw_label_caps(gui_rect(tip.x, tip.y, tip.w, gd_line_height(gui_s(TH->t_label))), "Tip", gui_s(TH->t_label), TH->accent, GUI_LEFT);
    gd_text_wrap(&tt, tip.x, tip.y + gd_line_height(gui_s(TH->t_label)) + gui_s(TH->gap * 0.5f), tip.w, lobby_load_tip(), true);
}

// ------------------------------------------------------------------ credits

static char *g_credits;
static GuiScroll g_cs;
static double g_last_input;

void mm_credits_draw(GuiRect content) {
    MmLayout *Ly = &mm()->credits;
    mm_layout_solve(Ly, content);
    GuiRect r = mm_node(Ly, "credits");
    if (!g_credits) {
        char buf[512];
        g_credits = platform_read_file(platform_data_path("assets/ui/credits.txt", buf, sizeof(buf)), NULL);
        if (!g_credits) { g_credits = (char *)malloc(64); dg_strcpy(g_credits, 64, "Credits missing (tools/gen_credits.py)."); }
    }
    mm_panel(r);
    GuiRect view = gui_rect_inset(r, gui_s(TH->pad));
    view.w -= TOK("mm_gap");
    if (gui_wheel() != 0 || gui_nav(NAV_UP) || gui_nav(NAV_DOWN) || gui_active() == (g_cs.id ^ 0x5C5Cu)) g_last_input = gui_time();
    if (gui_nav(NAV_DOWN)) { g_cs.target += TOK("mm_row_h") * 3; gui_nav_consume(NAV_DOWN); }
    if (gui_nav(NAV_UP)) { g_cs.target -= TOK("mm_row_h") * 3; gui_nav_consume(NAV_UP); }
    // Rolls by itself unless the reader took over in the last 4 s; back to the top at the end.
    if (gui_time() - g_last_input > 4.0) {
        g_cs.target += gui_dt() * TOK("mm_row_h");
        if (g_cs.content_h > view.h && g_cs.target >= g_cs.content_h - view.h + 1) g_cs.target = g_cs.offset = 0;
    }
    gw_scroll_begin(&g_cs, gui_id("mm_credits_scroll"), view);
    float y = gw_scroll_y(&g_cs), y0 = y;
    GuiText body = { gui_s(TH->t_body - 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
    GuiText head = { gui_s(TH->t_label + 1), GW_SEMIBOLD, TH->accent, GUI_LEFT, TH->tracking_caps * 1.5f, true, false, 0 };
    GuiText title = { gui_s(TH->t_title), GW_SEMIBOLD, TH->text, GUI_LEFT, 0.04f, true, false, 0 };
    const char *p = g_credits;
    char line[512];
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[--n] = 0;
        p = e ? e + 1 : p + strlen(p);
        if (!strncmp(line, "## ", 3)) { y += gui_s(TH->gap * 2); gd_text(&head, view.x, y, line + 3); y += gd_line_height(head.size) + gui_s(TH->gap * 0.5f); }
        else if (!strncmp(line, "# ", 2)) { gd_text(&title, view.x, y, line + 2); y += gd_line_height(title.size) + gui_s(TH->gap); }
        else if (!line[0]) y += gd_line_height(body.size) * 0.5f;
        else y += gd_text_wrap(&body, view.x, y, view.w, line, true);
    }
    gw_scroll_end(&g_cs, y - y0 + gui_s(TH->pad));
}
