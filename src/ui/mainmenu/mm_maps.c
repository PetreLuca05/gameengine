// Maps (docs/MENU_LOBBY.md "Maps", "Time, season and weather"): every assets/maps/*.json with its
// thumbnail and facts, in two groups: Development (Play > Trials: the bar at the bottom, the party's
// map) and Chapter 1 (Play > Story > Chapter select: this view); a click on a group opens its maps in a
// list rising above it. Selecting a map sets the next lobby's map and the background; the session
// look is picked here too: the season (calendar day, temperature and which weather it brings:
// autumn rain, winter snow), the start time and the start weather (one of the season's presets or
// a pick), and whether the weather changes during the game (the season-biased scheduler).
// Behind the panels: the globe (src/ui/globe, docs/MENU_GLOBE.md) flies to the selected map's place.
#include "mm_internal.h"
#include "ui/globe/globe.h"
#include "world/season.h"
#include "game/game_internal.h"
#include "world/weather.h"
#include "game/game_story.h"
#include <math.h>
#include <stdio.h>
#include <string.h>


// The bottom bar's groups: the story chapter's maps, every other map under Development.
enum { MG_DEV, MG_CHAPTER1, MG_COUNT };
static const char *const k_group_names[MG_COUNT] = { "Development", "Chapter 1: The Beginning" };

static int map_group(const MapInfo *m) { return m->chapter[0] ? MG_CHAPTER1 : MG_DEV; }

// A season in a few words, from its weights: "mostly rain", "mostly snow", "mostly fair" ...
// Story chapter maps start their chapter (docs/STORY.md); every other map hosts a lobby.
static void host_map(const MapInfo *m) {
    if (m && m->chapter[0]) mm_play_story(m->chapter, NULL);   // the chapter for the whole party
    else if (m) { mm_select_map(m->id); mm_play_start(); }   // the party's game on this map
}

static void season_summary(const SeasonDef *sd, char *out, int size) {
    float rain = 0, snow = 0, fair = 0, fog = 0, total = 0;
    for (int i = 0; i < sd->weather_count; i++) {
        const char *w = sd->weather[i];
        float k = sd->weight[i];
        total += k;
        bool precip = strstr(w, "rain") || strstr(w, "storm") || strstr(w, "snow") || strstr(w, "blizzard");
        bool cold = sd->temp_offset <= -8.0f;
        if (precip && (cold || strstr(w, "snow") || strstr(w, "blizzard"))) snow += k;
        else if (precip) rain += k;
        else if (!strcmp(w, "fog")) fog += k;
        else if (!strcmp(w, "clear") || !strcmp(w, "little_cloudy")) fair += k;
    }
    if (total <= 0) { snprintf(out, (size_t)size, "-"); return; }
    const char *what = snow > total * 0.4f ? "Mostly snow" : rain > total * 0.4f ? "Mostly rain" : fair > total * 0.5f ? "Mostly fair" : rain > fair ? "Showers" : "Changeable";
    snprintf(out, (size_t)size, "%s  ·  %+.0f C%s", what, (double)sd->temp_offset, fog > total * 0.15f ? "  ·  foggy" : "");
}

bool mm_environment_rows(GuiCursor *c, LobbySettings *s, bool enabled) {
    const SeasonTable *st = weather_seasons();
    bool changed = false;
    gw_disabled(!enabled);
    // Season
    const char *seasons[SEASON_COUNT];
    for (int i = 0; i < SEASON_COUNT; i++) seasons[i] = st->s[i].name;
    int sel = DG_CLAMP(s->season, 0, SEASON_COUNT - 1);
    GuiRect r = gw_setting_row(c, "Season", "Sun path and day length, temperature and which weather comes: autumn brings rain, winter snow.", gui_id("mm_env_season"));
    if (gw_dropdown(gui_id("mm_env_season_dd"), r, seasons, SEASON_COUNT, &sel) && enabled) {
        s->season = (u8)sel;
        if (s->weather[0] && !season_allows(&st->s[sel], s->weather)) s->weather[0] = 0;   // not in this season
        changed = true;
    }
    const SeasonDef *sd = &st->s[DG_CLAMP(s->season, 0, SEASON_COUNT - 1)];
    // Start time
    float hour = (float)s->start_hour;
    r = gw_setting_row(c, "Start time", "Time of day when the game starts.", gui_id("mm_env_hour"));
    if (gw_slider(gui_id("mm_env_hour_s"), r, &hour, 0, 23, 1, "%02.0f:00") && enabled) { s->start_hour = (u8)hour; changed = true; }
    // Start weather: the season's presets (weight as a note) or the season's pick.
    const char *items[SEASON_MAX_WEATHER + 1];
    const char *notes[SEASON_MAX_WEATHER + 1];
    static char names[SEASON_MAX_WEATHER][24];
    items[0] = "Season weather (random)";
    notes[0] = "";
    float wmax = 0;
    for (int i = 0; i < sd->weather_count; i++) wmax = fmaxf(wmax, sd->weight[i]);
    int wsel = 0;
    for (int i = 0; i < sd->weather_count; i++) {
        // "light_rain" -> "Light rain"
        dg_strcpy(names[i], sizeof(names[i]), sd->weather[i]);
        for (char *p = names[i]; *p; p++) if (*p == '_') *p = ' ';
        if (names[i][0] >= 'a' && names[i][0] <= 'z') names[i][0] = (char)(names[i][0] - 32);
        items[i + 1] = names[i];
        float k = wmax > 0 ? sd->weight[i] / wmax : 0;
        notes[i + 1] = k > 0.75f ? "often" : (k > 0.35f ? "sometimes" : "rare");
        if (!strcmp(s->weather, sd->weather[i])) wsel = i + 1;
    }
    r = gw_setting_row(c, "Start weather", "The weather at the start. It changes during the game when dynamic weather is on.", gui_id("mm_env_wx"));
    if (gw_dropdown_ex(gui_id("mm_env_wx_dd"), r, items, sd->weather_count + 1, &wsel, NULL, notes) && enabled) {
        dg_strcpy(s->weather, sizeof(s->weather), wsel ? sd->weather[wsel - 1] : "");
        changed = true;
    }
    // Dynamic weather
    bool dyn = s->dynamic_weather;
    r = gw_setting_row(c, "Dynamic weather", "The weather keeps changing the way the season does (autumn: rain, winter: snow).", gui_id("mm_env_dyn"));
    if (gw_toggle(gui_id("mm_env_dyn_t"), r, &dyn) && enabled) { s->dynamic_weather = dyn; changed = true; }
    gw_disabled(false);
    char sum[96];
    season_summary(sd, sum, sizeof(sum));
    GuiRect sr = gc_row(c, TOK("mm_row_h"));
    gw_label(sr, sum, gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT);
    return changed;
}

// The globe fills the screen behind the tab (drawn before the menu's shading and the panels, which
// blur it); centred between the list and the details, dragged there.
void mm_maps_background(GuiRect content, float alpha) {
    MainMenu *M = mm();
    MmLayout *L = &M->maps;
    mm_layout_solve(L, content);
    globe_select(lobby_map_info(M->map));
    GuiRect sp = mm_node(L, "spacer");
    globe_draw(sp.x + sp.w * 0.5f, content.y + content.h * 0.5f, sp, alpha);
}

// One map card: the big thumbnail, the name under it; selected: an accent frame. A click selects,
// a double click starts.
static void map_card(int i, const MapInfo *m, GuiRect r) {
    MainMenu *M = mm();
    GuiId id = gui_idi("mm_map_card", i);
    GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE);
    bool sel = !strcmp(M->map, m->id);
    float h = gui_tween(id, in.hot || sel ? 1.0f : 0.0f, TH->hover_t);
    GuiRect th = { r.x, r.y, r.w, r.w / 1.7778f };
    gd_backdrop(r, UI_RGBA(16, 18, 22, 140));
    mm_thumb(th, m->thumbnail);
    if (!sel && h < 1.0f) gd_rect(th, gui_rgba_mul(UI_RGBA(0, 0, 0, 255), 0.35f * (1.0f - h)));   // the others dimmed
    GuiText t = { gui_s(TH->t_body), GW_SEMIBOLD, sel ? TH->text : gui_rgba_lerp(TH->text_dim, TH->text, h), GUI_LEFT, 0, false, true, r.w - gui_s(12) };
    float ny = th.y + th.h + gui_s(8);
    gd_text(&t, r.x + gui_s(8), ny, m->name);
    char sub[64], size[32];
    maplist_size_text(m, size, sizeof(size));
    snprintf(sub, sizeof(sub), "%s  ·  %s", m->biome, size);
    GuiText st = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT, 0, false, true, r.w - gui_s(12) };
    gd_text(&st, r.x + gui_s(8), ny + gd_line_height(t.size) + gui_s(2), sub);
    if (sel) gd_round_outline(r, gui_s(TH->radius), fmaxf(1.0f, gui_s(2.0f)), TH->accent);
    else if (h > 0.01f) gd_round_outline(r, gui_s(TH->radius), 1.0f, gui_rgba_mul(TH->accent, h * 0.6f));
    if (in.focused) gw_focus_ring(r, gui_s(TH->radius), 1.0f);
    if (in.clicked && !sel) { mm_select_map(m->id); gui_sound(TH->snd_click); }
    if (in.double_clicked) host_map(m);
}

// The group's maps as cards in a row across `r`, scrolled sideways by the wheel when they overflow.
static void map_cards(GuiRect r, int group) {
    static float scroll[MG_COUNT], target[MG_COUNT];
    const MapList *ml = lobby_maps();
    float cw = TOK("mm_map_card_w"), gap = TOK("mm_gap_lg");
    int n = 0;
    for (int i = 0; i < ml->count; i++) n += map_group(&ml->maps[i]) == group;
    float total = (float)n * cw + (float)(n > 0 ? n - 1 : 0) * gap;
    float most = fmaxf(0.0f, total - r.w);
    if (gui_rect_contains(r, gui_mouse().x, gui_mouse().y) && gui_wheel() != 0) {
        target[group] = clampf(target[group] - gui_wheel() * (cw + gap), 0.0f, most);
        gui_wheel_consume();
    }
    target[group] = clampf(target[group], 0.0f, most);
    scroll[group] += (target[group] - scroll[group]) * saturate(gui_dt() * 12.0f);
    float t = gui_anim_t(gui_id("mm_tab_in"));
    gd_clip_push(gui_rect(r.x, r.y - gui_s(4), r.w, r.h + gui_s(8)));
    int k = 0;
    for (int i = 0; i < ml->count; i++) {
        const MapInfo *m = &ml->maps[i];
        if (map_group(m) != group) continue;
        float a = gui_ease_out(saturate((t - (float)k * TH->menu_stagger) / fmaxf(0.01f, TH->menu_in)));
        GuiRect c = { r.x + (float)k * (cw + gap) - scroll[group], r.y + (1.0f - a) * gui_s(16), cw, r.h };
        k++;
        if (c.x + c.w < r.x || c.x > r.x + r.w) continue;
        gd_alpha_push(a);
        map_card(i, m, c);
        gd_alpha_pop();
    }
    gd_clip_pop();
}

// Play > Enroll (trials: the development maps, Start = the party's game) / Story > Chapter select (the
// story maps, Play this chapter): the maps on the globe (the bar: the group's entry, Back), the details.
void mm_maps_draw(GuiRect content, bool trials) {
    MainMenu *M = mm();
    MmLayout *L = &M->maps;
    mm_layout_solve(L, content);
    map_cards(mm_node(L, "cards"), trials ? MG_DEV : MG_CHAPTER1);   // Back: the tab bar's (the quit button's place)

    // Details + session look.
    const MapInfo *mi = lobby_map_info(M->map);
    GuiRect d = mm_node(L, "detail");
    mm_panel(d);
    GuiRect in = gui_rect_inset(d, gui_s(TH->pad));
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    if (!mi) return;
    mm_thumb(gc_row(&c, in.w / 1.7778f), mi->thumbnail);
    gw_label(gc_row(&c, TOK("mm_title_h")), mi->name, gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT);
    char v[64];
    maplist_size_text(mi, v, sizeof(v));
    mm_kv(&c, "Size", v);
    mm_kv(&c, "Biome", mi->biome);
    snprintf(v, sizeof(v), "%d - %d", mi->players_min, mi->players_max);
    mm_kv(&c, "Recommended players", v);
    GuiText b = { gui_s(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
    c.y += TOK("mm_gap");
    c.y += gd_text_wrap(&b, in.x, c.y, in.w, mi->description, true) + TOK("mm_gap");
    gw_section(gc_row(&c, TOK("mm_row_h")), "Session");
    LobbySettings s;
    lobby_settings_default(&s);
    s.season = (u8)M->season;
    s.start_hour = (u8)M->hour;
    dg_strcpy(s.weather, sizeof(s.weather), M->weather);
    s.dynamic_weather = M->dynamic_weather;
    if (mm_environment_rows(&c, &s, true)) {
        M->season = s.season;
        M->hour = s.start_hour;
        dg_strcpy(M->weather, sizeof(M->weather), s.weather);
        M->dynamic_weather = s.dynamic_weather;
        M->env_dirty = true;
        // The next lobby starts with this look.
        Profile *p = lobby_profile();
        p->lobby.season = s.season;
        p->lobby.start_hour = s.start_hour;
        dg_strcpy(p->lobby.weather, sizeof(p->lobby.weather), s.weather);
        p->lobby.dynamic_weather = s.dynamic_weather;
        mm_party_sync();
    }
    // Trials: the party's game settings (the host edits them).
    if (trials && lobby_active()) {
        const Lobby *lb = lobby_get();
        bool host = lobby_is_host();
        LobbySettings ls = lb->settings;
        bool ch = false;
        gw_disabled(!host);
        const char *diff[LD_COUNT];
        for (int i = 0; i < LD_COUNT; i++) diff[i] = lobby_difficulty_name(i);
        int d = ls.difficulty;
        GuiRect rr = gw_setting_row(&c, "Difficulty", NULL, gui_id("mp_diff"));
        if (gw_dropdown(gui_id("mp_diff_dd"), rr, diff, LD_COUNT, &d)) { ls.difficulty = (u8)d; ch = true; }
        float mp = (float)ls.max_players;
        rr = gw_setting_row(&c, "Max players", NULL, gui_id("mp_max"));
        if (gw_slider(gui_id("mp_max_s"), rr, &mp, 1, LOBBY_MAX_MEMBERS, 1, "%.0f")) { ls.max_players = (u8)mp; ch = true; }
        gw_disabled(false);
        if (ch && host) lobby_set_settings(&ls);
    }
    GuiRect br = { in.x, in.y + in.h - TOK("mm_button_h"), in.w, TOK("mm_button_h") };
    const MapInfo *sel = lobby_map_info(M->map);
    const char *label = sel && sel->chapter[0] ? "Play this chapter" : "Start";
    bool can = !trials || !lobby_active() || lobby_is_host();
    if (c.y + TOK("mm_gap") < br.y && gw_button(gui_id("mm_maps_host"), br, label, GB_PRIMARY, can)) host_map(sel);
    char hint[160];
    snprintf(hint, sizeof(hint), "Enter / A  select     double click  host     drag  turn the globe     %s", globe_phase_text());
    mm_footer_hint(gui_rect(content.x, content.y + content.h, content.w, TOK("mm_row_h")), hint);
    // The globe's data sources, under it (between the panels).
    GuiRect sp = mm_node(L, "spacer");
    GuiText ct = { gui_s(TH->t_tiny), GW_REGULAR, TH->text_faint, GUI_CENTER, 0, false, true, 0 };
    float pad = TOK("mm_gap_lg");
    float ch = gd_text_wrap(&ct, sp.x + pad, 0, sp.w - pad * 2, globe_credits(), false);
    ct.align = GUI_LEFT;
    gd_text_wrap(&ct, sp.x + pad, sp.y + sp.h - ch, sp.w - pad * 2, globe_credits(), true);
}
