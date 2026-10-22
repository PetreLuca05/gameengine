// Stats tab (ui_stats_tab.h, docs/STATS.md "Stats tab").
//
//   [Overview  Performance  Weapons  Movement  History  Activity]          (LB / RB, Q / E, mouse)
//   [player v] [compare]                         [units v] [Export JSON] [Reset stats]
//   page (scrolls): cards / charts / tables
//
// Every number comes from the active stats (stats_get()); a remote player shows the public summary
// the host relays (game_stats_public). Derived series are rebuilt when the stats revision or an
// option changes; the revision the page shows follows the live one at most once a second.
#include "ui_stats_tab.h"
#include "ui_chart.h"
#include "screens/screens.h"
#include "game/game_stats.h"
#include "core/cvar.h"
#include "core/log.h"
#include "world/world.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH (gui_theme())

enum { SP_OVERVIEW, SP_PERFORMANCE, SP_WEAPONS, SP_MOVEMENT, SP_HISTORY, SP_ACTIVITY, SP_COUNT };
static const char *const k_pages[SP_COUNT] = { "Overview", "Performance", "Weapons", "Movement", "History", "Activity" };
static const char *const k_page_ids[SP_COUNT] = { "overview", "performance", "weapons", "movement", "history", "activity" };

enum { MET_KILLS, MET_DEATHS, MET_KD, MET_ACC, MET_DMG, MET_TAKEN, MET_REVIVES, MET_WAVES, MET_DIST, MET_COUNT };
static const char *const k_metrics[MET_COUNT] = { "Kills", "Deaths", "K/D", "Accuracy", "Damage dealt", "Damage taken", "Revives", "Waves", "Distance" };
static const char *const k_metrics2[MET_COUNT + 1] = { "Right axis: none", "Kills", "Deaths", "K/D", "Accuracy", "Damage dealt", "Damage taken", "Revives", "Waves", "Distance" };
enum { RG_10, RG_25, RG_50, RG_100, RG_30D, RG_ALL, RG_COUNT };
static const char *const k_ranges[RG_COUNT] = { "Last 10 matches", "Last 25 matches", "Last 50 matches", "Last 100 matches", "Last 30 days", "All time" };
static const char *const k_views[2] = { "Per match", "Per day" };
static const char *const k_units[2] = { "Metric", "Imperial" };
static const char *const k_months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const k_wdays[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *const k_results[MATCH_RESULT_COUNT] = { "Abandoned", "Win", "Loss" };

enum { WC_NAME, WC_KILLS, WC_SHOTS, WC_HITS, WC_ACC, WC_HS, WC_DMG, WC_TIME, WC_LONG, WC_COUNT };
static const char *const k_wcols[WC_COUNT] = { "Weapon", "Kills", "Shots", "Hits", "Acc.", "Headsh.", "Damage", "Equipped", "Longest" };

#define MAXP 400
#define NCARDS 13

static struct {
    bool inited;
    int page, player;
    bool compare;
    // performance
    int metric, metric2, range, view;
    bool ma;
    // weapons
    int sort_col;
    bool sort_asc;
    int wsel, wcmp_item;            // selected stats weapon slot (-1), compare dropdown item (0 none)
    // history
    int msel;                       // match index (oldest first), -1: the list
    // activity
    int act_mode;                   // 0 matches, 1 kills
    GuiScroll scroll[SP_COUNT + 1];
    float scroll_req;               // content y to scroll to once the page knows its height (< 0 none)
    int scroll_req_frames;
    float detail_y;                 // the weapon detail's content y (scrolled to when a weapon is picked)
    u32 rev, gen;
    double rev_time;
    char status[220];
    double status_time;
    char tip[400];
    // chart caches
    ChartCache spark[NCARDS + 4], perf, khist, scatter, wkills, wacc, wdk, wdt, wradar, wline, wdist, wstack, move, timeline, mdonut, cal, cradar;
    // performance series
    u32 perf_key;
    int pn;
    float py[MAXP], py2[MAXP];
    u8 pcls[MAXP];
    i32 pday[MAXP];
    int pmatch[MAXP];
    float hist[24];
    int hist_n, hist_w;
    float hist_mark;
    int sn;
    float sx[STATS_MAX_MATCHES], sy[STATS_MAX_MATCHES];
    u32 scol[STATS_MAX_MATCHES];
    int smatch[STATS_MAX_MATCHES];
} T;

// ------------------------------------------------------------------ helpers

static const PlayerStats *PS(void) { return &stats_get()->life; }
static bool imperial(void) { return game_stats_imperial(); }
static u32 mixk(u32 a, u32 b) { return (a * 2654435761u) ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2)); }
static u32 base_key(void) { return mixk(T.rev, mixk(T.gen, imperial() ? 7u : 3u)); }
static double dist_unit(void) { return imperial() ? 1609.344 : 1000.0; }
static const char *dist_unit_name(void) { return imperial() ? "mi" : "km"; }
static int nmatches(void) { return PS()->match_count; }
static const MatchRecord *match(int i) { return stats_match_at(PS(), i); }

static const char *fday(char *b, int n, i32 day) {
    int y, m, d;
    stats_day_ymd(day, &y, &m, &d);
    snprintf(b, (size_t)n, "%s %d", k_months[(m + 11) % 12], d);
    return b;
}

static const char *fday_full(char *b, int n, i32 day) {
    int y, m, d;
    stats_day_ymd(day, &y, &m, &d);
    snprintf(b, (size_t)n, "%s %d %s %d", k_wdays[stats_weekday(day)], d, k_months[(m + 11) % 12], y);
    return b;
}

static const char *ftime_of_day(char *b, int n, i64 t) {
    i64 s = t + stats_get()->utc_offset_sec;
    s = ((s % 86400) + 86400) % 86400;
    snprintf(b, (size_t)n, "%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60));
    return b;
}

static const char *fdist(char *b, int n, double m) { return chart_fmt_dist(b, n, m, imperial()); }

static void ui_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) {
    vec2 uv = gd_white_uv();
    UiVertex v[3] = { { v2(x0, y0), uv, c }, { v2(x1, y1), uv, c }, { v2(x2, y2), uv, c } };
    u32 idx[3] = { 0, 1, 2 };
    gd_tris(gd_atlas(), v, 3, idx, 3);
}

// Up / down arrow (delta vs the previous matches): green when it is good news.
static void delta_arrow(float x, float y, float s, bool up, bool good) {
    u32 c = good ? TH->ok : TH->danger;
    if (up) ui_tri(x, y + s, x + s, y + s, x + s * 0.5f, y, c);
    else ui_tri(x, y, x + s, y, x + s * 0.5f, y + s, c);
}

static void text(float x, float y, float size, int weight, u32 color, int align, const char *s) {
    GuiText t = { size, weight, color, align, 0, false, false, 0 };
    gd_text(&t, x, y, s);
}

static void text_w(float x, float y, float size, int weight, u32 color, int align, float max_w, const char *s) {
    GuiText t = { size, weight, color, align, 0, false, false, max_w };
    gd_text(&t, x, y, s);
}

static void caps(float x, float y, float size, u32 color, const char *s) {
    GuiText t = { size, GW_SEMIBOLD, color, GUI_LEFT, TH->tracking_caps, true, false, 0 };
    gd_text(&t, x, y, s);
}

static void card_bg(GuiRect r, bool hot) {
    gd_round_border(r, gui_s(TH->radius + 1), 1.0f, hot ? UI_RGBA(255, 255, 255, 14) : UI_RGBA(255, 255, 255, 7), TH->panel_border);
}

// A titled box for a chart; returns the chart's rect.
static GuiRect box(GuiRect r, const char *title) {
    card_bg(r, false);
    float pad = gui_s(10);
    caps(r.x + pad, r.y + gui_s(8), gui_s(TH->t_tiny + 1), TH->text_faint, title);
    float top = gui_s(8) + gd_line_height(gui_s(TH->t_tiny + 1)) + gui_s(6);
    return gui_rect(r.x + pad, r.y + top, r.w - pad * 2, r.h - top - pad);
}

static float section(GuiRect a, float y, const char *title) {
    gw_section(gui_rect(a.x, y, a.w, gui_s(TH->row_h)), title);
    return gui_s(TH->row_h) + gui_s(4);
}

static void set_tip(const char *s) { dg_strcpy(T.tip, sizeof(T.tip), s); }

static float empty_note(GuiRect a, float y) {
    if (nmatches() > 0) return 0;
    float h = gui_s(46);
    GuiRect r = { a.x, y, a.w, h };
    gd_round(r, gui_s(TH->radius), UI_RGBA(255, 255, 255, 6));
    text_w(r.x + gui_s(14), r.y + (h - gd_line_height(gui_s(TH->t_body))) * 0.5f, gui_s(TH->t_body), GW_REGULAR, TH->text_dim, GUI_LEFT, r.w - gui_s(28),
           "No matches recorded yet: finished matches fill these charts (developers: stats_fake 100 in the console).");
    return h + gui_s(10);
}

// ------------------------------------------------------------------ metrics

static float match_metric(const MatchRecord *m, int met) {
    switch (met) {
    case MET_KILLS: return (float)m->kills;
    case MET_DEATHS: return (float)m->deaths;
    case MET_KD: return stats_match_kd(m);
    case MET_ACC: return m->shots_fired ? stats_match_accuracy(m) * 100.0f : NAN;
    case MET_DMG: return m->damage_dealt;
    case MET_TAKEN: return m->damage_taken;
    case MET_REVIVES: return (float)m->revives;
    case MET_WAVES: return (float)m->waves_reached;
    case MET_DIST: return (float)(m->distance_m / dist_unit());
    default: return NAN;
    }
}

static const char *metric_fmt(int met, double v, char *b, int n) {
    if (isnan(v)) {
        dg_strcpy(b, (size_t)n, "-");
        return b;
    }
    switch (met) {
    case MET_KD: snprintf(b, (size_t)n, "%.2f", v); return b;
    case MET_ACC: return chart_fmt_pct(b, n, v / 100.0, 1);
    case MET_DIST: snprintf(b, (size_t)n, "%.2f %s", v, dist_unit_name()); return b;
    case MET_DMG: case MET_TAKEN: return chart_fmt_int(b, n, v);
    default: return chart_fmt_num(b, n, v, fabs(v - floor(v)) > 1e-3 ? 1 : 0);
    }
}

static const char *axis_fmt(double v, char *b, int n, void *user) {
    int met = (int)(intptr_t)user;
    switch (met) {
    case MET_KD: snprintf(b, (size_t)n, "%.1f", v); return b;
    case MET_ACC: snprintf(b, (size_t)n, "%.0f %%", v); return b;
    case MET_DIST: snprintf(b, (size_t)n, "%g %s", v, dist_unit_name()); return b;
    default: return chart_fmt_compact(b, n, v);
    }
}

static const char *axis_fmt2(double v, char *b, int n, void *user) {
    (void)user;
    return axis_fmt(v, b, n, (void *)(intptr_t)(T.metric2 - 1));
}

static const char *acc_axis_fmt(double v, char *b, int n, void *user) {
    (void)user;
    return axis_fmt(v, b, n, (void *)(intptr_t)MET_ACC);
}

// ------------------------------------------------------------------ performance data

static float day_metric(const DailyRecord *d, int met) {
    switch (met) {
    case MET_KILLS: return (float)d->kills;
    case MET_DEATHS: return (float)d->deaths;
    case MET_KD: return (float)d->kills / (float)DG_MAX(d->deaths, 1u);
    case MET_ACC: return d->shots_fired ? fminf(100.0f, 100.0f * (float)d->shots_hit / (float)d->shots_fired) : NAN;
    default: break;
    }
    // The rest from the day's match records still in the history.
    float sum = 0, mx = 0;
    int cnt = 0;
    for (int i = nmatches() - 1; i >= 0; i--) {
        const MatchRecord *m = match(i);
        i32 day = stats_day_of(m->timestamp);
        if (day < d->day) break;
        if (day != d->day) continue;
        float v = match_metric(m, met);
        if (isnan(v)) continue;
        sum += v;
        mx = fmaxf(mx, v);
        cnt++;
    }
    if (!cnt) return NAN;
    return met == MET_WAVES ? mx : sum;
}

static int range_first_match(void) {
    int nm = nmatches();
    switch (T.range) {
    case RG_10: return DG_MAX(0, nm - 10);
    case RG_25: return DG_MAX(0, nm - 25);
    case RG_50: return DG_MAX(0, nm - 50);
    case RG_100: return DG_MAX(0, nm - 100);
    case RG_30D: {
        i32 d0 = stats_day_of(stats_now()) - 29;
        for (int i = 0; i < nm; i++) if (stats_day_of(match(i)->timestamp) >= d0) return i;
        return nm;
    }
    default: return 0;
    }
}

static void perf_prepare(void) {
    u32 k = mixk(base_key(), (u32)(T.metric | T.metric2 << 4 | T.range << 8 | T.view << 12));
    if (k == T.perf_key) return;
    T.perf_key = k;
    const PlayerStats *ps = PS();
    int nm = nmatches(), first = range_first_match();
    T.pn = 0;
    if (T.view == 0) {
        for (int i = first; i < nm && T.pn < MAXP; i++) {
            const MatchRecord *m = match(i);
            int p = T.pn++;
            T.py[p] = match_metric(m, T.metric);
            T.py2[p] = T.metric2 ? match_metric(m, T.metric2 - 1) : NAN;
            T.pcls[p] = m->result == MATCH_WIN ? 1 : m->result == MATCH_LOSS ? 2 : 3;
            T.pday[p] = stats_day_of(m->timestamp);
            T.pmatch[p] = i;
        }
    } else {
        i32 d0 = T.range == RG_ALL ? -0x7FFFFFFF : T.range == RG_30D ? stats_day_of(stats_now()) - 29 : (first < nm ? stats_day_of(match(first)->timestamp) : 0x7FFFFFFF);
        for (int i = 0; i < ps->day_count && T.pn < MAXP; i++) {
            const DailyRecord *d = stats_day_at(ps, i);
            if (d->day < d0) continue;
            int p = T.pn++;
            T.py[p] = day_metric(d, T.metric);
            T.py2[p] = T.metric2 ? day_metric(d, T.metric2 - 1) : NAN;
            T.pcls[p] = 0;
            T.pday[p] = d->day;
            T.pmatch[p] = -1;
        }
    }
    // Kills per match histogram (the range's matches) with the last match marked.
    int kmax = 0;
    for (int i = first; i < nm; i++) kmax = DG_MAX(kmax, (int)match(i)->kills);
    T.hist_w = DG_MAX(1, (int)chart_nice_step((double)kmax + 1.0, 9));
    T.hist_n = DG_MIN(24, kmax / T.hist_w + 1);
    memset(T.hist, 0, sizeof(T.hist));
    for (int i = first; i < nm; i++) T.hist[DG_MIN(T.hist_n - 1, (int)match(i)->kills / T.hist_w)] += 1.0f;
    T.hist_mark = nm > first ? ((float)match(nm - 1)->kills + 0.5f) / (float)T.hist_w : -1.0f;
    if (nm <= first) T.hist_n = 0;
    // Accuracy vs kills, one dot per match.
    T.sn = 0;
    for (int i = first; i < nm; i++) {
        const MatchRecord *m = match(i);
        if (!m->shots_fired) continue;
        int s = T.sn++;
        T.sx[s] = stats_match_accuracy(m) * 100.0f;
        T.sy[s] = (float)m->kills;
        T.scol[s] = m->result == MATCH_WIN ? TH->ok : m->result == MATCH_LOSS ? TH->danger : TH->text_dim;
        T.smatch[s] = i;
    }
}

static const char *perf_xlabel(int i, char *b, int n, void *user) {
    (void)user;
    return i >= 0 && i < T.pn ? fday(b, n, T.pday[i]) : "";
}

static const char *hist_label(int i, char *b, int n, void *user) {
    (void)user;
    if (T.hist_w == 1) snprintf(b, (size_t)n, "%d", i);
    else snprintf(b, (size_t)n, "%d-%d", i * T.hist_w, (i + 1) * T.hist_w - 1);
    return b;
}

// ------------------------------------------------------------------ overview

typedef struct Card {
    const char *title;
    char value[48], sub[64];
    float series[10];
    int n;
    float gauge;                    // >= 0: a gauge card
    bool lower_better, has_delta;
    float delta;                    // relative change (last 10 vs the 10 before)
} Card;

static float card_match_value(int card, const MatchRecord *m, int fav, int idx) {
    switch (card) {
    case 0: return stats_match_kd(m);
    case 1: return (float)m->kills;
    case 2: return (float)m->deaths;
    case 3: return m->shots_fired ? stats_match_accuracy(m) : NAN;
    case 4: return m->kills ? (float)m->headshots / (float)m->kills : NAN;
    case 5: return (float)m->revives;
    case 6: return m->duration_sec / 60.0f;
    case 7: return (float)m->waves_reached;
    case 8: return (float)m->best_streak;
    case 9: return m->longest_kill_m;
    case 10: return fav >= 0 ? (float)m->kills_per_weapon[fav] : NAN;
    case 11: return m->distance_m;
    case 12: {
        // Win rate over the history up to this match.
        int wins = 0, n = 0;
        for (int i = 0; i <= idx; i++) { n++; wins += match(i)->result == MATCH_WIN; }
        return n ? (float)wins / (float)n : NAN;
    }
    default: return NAN;
    }
}

static void build_cards(Card *c) {
    const PlayerStats *ps = PS();
    char b[48], b2[48];
    int fav = stats_favorite_weapon(ps);
    static const char *const titles[NCARDS] = { "K/D", "Kills", "Deaths", "Accuracy", "Headshot %", "Revives", "Time played", "Highest wave",
                                               "Best killstreak", "Longest kill", "Favorite weapon", "Distance", "Win rate" };
    for (int i = 0; i < NCARDS; i++) {
        memset(&c[i], 0, sizeof(c[i]));
        c[i].title = titles[i];
        c[i].gauge = -1;
    }
    snprintf(c[0].value, sizeof(c[0].value), "%.2f", stats_kd(ps));
    chart_fmt_int(c[1].value, sizeof(c[1].value), ps->kills);
    snprintf(c[1].sub, sizeof(c[1].sub), "%s headshots", chart_fmt_int(b, sizeof(b), ps->headshots));
    chart_fmt_int(c[2].value, sizeof(c[2].value), ps->deaths);
    snprintf(c[2].sub, sizeof(c[2].sub), "%s times downed", chart_fmt_int(b, sizeof(b), ps->times_downed));
    c[2].lower_better = true;
    c[3].gauge = stats_overall_accuracy(ps);
    chart_fmt_pct(c[3].value, sizeof(c[3].value), c[3].gauge, 1);
    snprintf(c[3].sub, sizeof(c[3].sub), "%s / %s hits", chart_fmt_int(b, sizeof(b), stats_total_hits(ps)), chart_fmt_int(b2, sizeof(b2), stats_total_shots(ps)));
    c[4].gauge = stats_headshot_pct(ps);
    chart_fmt_pct(c[4].value, sizeof(c[4].value), c[4].gauge, 1);
    chart_fmt_int(c[5].value, sizeof(c[5].value), ps->revives_given);
    chart_fmt_duration(c[6].value, sizeof(c[6].value), ps->time_played_sec);
    snprintf(c[6].sub, sizeof(c[6].sub), "%s matches", chart_fmt_int(b, sizeof(b), ps->matches_played));
    chart_fmt_int(c[7].value, sizeof(c[7].value), ps->highest_wave);
    chart_fmt_int(c[8].value, sizeof(c[8].value), ps->best_killstreak);
    fdist(c[9].value, sizeof(c[9].value), ps->longest_kill.dist_m);
    if (ps->longest_kill.dist_m > 0)
        snprintf(c[9].sub, sizeof(c[9].sub), "%s · %s", ps->longest_kill.weapon >= 0 ? game_stats_weapon_name(ps, ps->longest_kill.weapon) : "-", game_stats_map_name(ps->longest_kill.map));
    dg_strcpy(c[10].value, sizeof(c[10].value), fav >= 0 ? game_stats_weapon_name(ps, fav) : "-");
    if (fav >= 0) snprintf(c[10].sub, sizeof(c[10].sub), "%s kills · %s", chart_fmt_int(b, sizeof(b), ps->weapons[fav].kills), chart_fmt_pct(b2, sizeof(b2), stats_weapon_accuracy(&ps->weapons[fav]), 1));
    fdist(c[11].value, sizeof(c[11].value), ps->total_distance_m);
    snprintf(c[11].sub, sizeof(c[11].sub), "%s steps · %s jumps", chart_fmt_int(b, sizeof(b), (double)ps->steps), chart_fmt_int(b2, sizeof(b2), ps->jumps));
    c[12].gauge = stats_win_rate(ps);
    chart_fmt_pct(c[12].value, sizeof(c[12].value), c[12].gauge, 0);
    snprintf(c[12].sub, sizeof(c[12].sub), "%s won", chart_fmt_int(b, sizeof(b), ps->matches_won));
    // Sparklines: the last 10 matches; the delta: their mean vs the 10 before.
    int nm = nmatches();
    for (int i = 0; i < NCARDS; i++) {
        int first = DG_MAX(0, nm - 10);
        for (int k = first; k < nm; k++) c[i].series[c[i].n++] = card_match_value(i, match(k), fav, k);
        double a = 0, b0 = 0;
        int na = 0, nb = 0;
        for (int k = first; k < nm; k++) { float v = card_match_value(i, match(k), fav, k); if (!isnan(v)) { a += v; na++; } }
        for (int k = DG_MAX(0, nm - 20); k < first; k++) { float v = card_match_value(i, match(k), fav, k); if (!isnan(v)) { b0 += v; nb++; } }
        if (na && nb && i != 10) {
            a /= na;
            b0 /= nb;
            c[i].has_delta = fabs(b0) > 1e-6 || fabs(a) > 1e-6;
            c[i].delta = fabs(b0) > 1e-6 ? (float)((a - b0) / fabs(b0)) : (a > 0 ? 1.0f : 0.0f);
        }
    }
}

static void draw_card(GuiRect r, Card *c, int idx, const ChartStyle *st) {
    GuiInteract in = gui_interact(gui_idi("st_card", idx), r, 0);
    card_bg(r, in.hovered);
    float pad = gui_s(12);
    caps(r.x + pad, r.y + gui_s(10), gui_s(TH->t_tiny + 1), TH->text_faint, c->title);
    float vy = r.y + gui_s(10) + gd_line_height(gui_s(TH->t_tiny + 1)) + gui_s(4);
    if (c->gauge >= 0) {
        GuiRect g = { r.x + pad * 0.5f, vy - gui_s(2), r.h - (vy - r.y) + gui_s(2), r.h - (vy - r.y) - gui_s(4) };
        chart_gauge(g, st, c->gauge, "", c->value, TH->accent);
        if (c->sub[0]) text_w(r.x + pad + g.w, vy + gui_s(2), gui_s(TH->t_tiny), GW_REGULAR, TH->text_dim, GUI_LEFT, r.w - g.w - pad * 2, c->sub);
    } else {
        text_w(r.x + pad, vy, gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT, r.w - pad * 2, c->value);
        if (c->sub[0]) text_w(r.x + pad, vy + gd_line_height(gui_s(TH->t_title - 2)), gui_s(TH->t_tiny), GW_REGULAR, TH->text_dim, GUI_LEFT, r.w - pad * 2, c->sub);
    }
    // Delta arrow (top right) and the sparkline (bottom right).
    if (c->has_delta && fabsf(c->delta) >= 0.005f) {
        char b[24];
        snprintf(b, sizeof(b), "%+.0f %%", c->delta * 100.0f);
        float s = gui_s(7), ty = r.y + gui_s(10);
        GuiText t = { gui_s(TH->t_tiny), GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, false, 0 };
        float w = gd_text(&t, r.x + r.w - pad, ty, b);
        bool up = c->delta > 0;
        delta_arrow(r.x + r.w - pad - w - s - gui_s(4), ty + (gd_line_height(t.size) - s) * 0.5f, s, up, up != c->lower_better);
        if (in.hovered) gui_tooltip(gui_idi("st_card", idx), "Last 10 matches vs the 10 before");
    }
    if (c->n >= 2) {
        GuiRect sr = { r.x + r.w * 0.52f, r.y + r.h - gui_s(34), r.w * 0.48f - pad, gui_s(24) };
        chart_sparkline(&T.spark[idx], base_key(), sr, st, c->series, c->n, TH->accent);
    }
}

static float card_grid(GuiRect a, float y, int n, float card_h, int *cols_out) {
    int cols = DG_CLAMP((int)(a.w / gui_s(250)), 2, 5);
    if (cols_out) *cols_out = cols;
    int rows = (n + cols - 1) / cols;
    return (float)rows * (card_h + gui_s(8));
}

static float page_overview(GuiRect a, const ChartStyle *st) {
    float y = a.y;
    y += empty_note(a, y);
    Card cards[NCARDS];
    build_cards(cards);
    int cols;
    float ch = gui_s(108), gap = gui_s(8);
    float h = card_grid(a, y, NCARDS, ch, &cols);
    float cw = (a.w - gap * (float)(cols - 1)) / (float)cols;
    for (int i = 0; i < NCARDS; i++) {
        GuiRect r = { a.x + (cw + gap) * (float)(i % cols), y + (ch + gap) * (float)(i / cols), cw, ch };
        draw_card(r, &cards[i], i, st);
    }
    return y + h - a.y;
}

// ------------------------------------------------------------------ public / compare (other players)

typedef struct PubRow { const char *name; double a, b; int kind; bool lower_better; } PubRow;   // kind 0 int, 1 pct, 2 dist, 3 time, 4 ratio

static int pub_rows(const StatsPublic *p, const StatsPublic *q, PubRow *r) {
    int n = 0;
#define ROW(nm, va, vb, k, lb) do { r[n].name = nm; r[n].a = va; r[n].b = vb; r[n].kind = k; r[n].lower_better = lb; n++; } while (0)
    ROW("Kills", p->kills, q ? q->kills : 0, 0, false);
    ROW("Deaths", p->deaths, q ? q->deaths : 0, 0, true);
    ROW("K/D", (double)p->kills / DG_MAX(p->deaths, 1u), q ? (double)q->kills / DG_MAX(q->deaths, 1u) : 0, 4, false);
    ROW("Accuracy", stats_ratio(p->shots_hit, p->shots_fired), q ? stats_ratio(q->shots_hit, q->shots_fired) : 0, 1, false);
    ROW("Headshot %", stats_ratio(p->headshots, p->kills), q ? stats_ratio(q->headshots, q->kills) : 0, 1, false);
    ROW("Revives", p->revives_given, q ? q->revives_given : 0, 0, false);
    ROW("Time played", p->time_played_sec, q ? q->time_played_sec : 0, 3, false);
    ROW("Highest wave", p->highest_wave, q ? q->highest_wave : 0, 0, false);
    ROW("Best killstreak", p->best_killstreak, q ? q->best_killstreak : 0, 0, false);
    ROW("Longest kill", p->longest_kill_m, q ? q->longest_kill_m : 0, 2, false);
    ROW("Distance", p->distance_m, q ? q->distance_m : 0, 2, false);
    ROW("Matches", p->matches_played, q ? q->matches_played : 0, 0, false);
    ROW("Win rate", stats_ratio(p->matches_won, p->matches_played), q ? stats_ratio(q->matches_won, q->matches_played) : 0, 1, false);
#undef ROW
    return n;
}

static const char *pub_fmt(const PubRow *r, double v, char *b, int n) {
    switch (r->kind) {
    case 1: return chart_fmt_pct(b, n, v, 1);
    case 2: return fdist(b, n, v);
    case 3: return chart_fmt_duration(b, n, v);
    case 4: snprintf(b, (size_t)n, "%.2f", v); return b;
    default: return chart_fmt_int(b, n, v);
    }
}

static float page_public(GuiRect a, const StatsPublic *p, const char *name) {
    float y = a.y;
    if (!p) {
        text(a.x, y, gui_s(TH->t_body), GW_REGULAR, TH->text_dim, GUI_LEFT, "Waiting for this player's stats from the host...");
        return gui_s(40);
    }
    PubRow rows[16];
    int n = pub_rows(p, NULL, rows);
    char b[64];
    snprintf(b, sizeof(b), "%s · favorite weapon: %s", name, p->favorite[0] ? p->favorite : "-");
    text(a.x, y, gui_s(TH->t_body), GW_MEDIUM, TH->text, GUI_LEFT, b);
    y += gd_line_height(gui_s(TH->t_body)) + gui_s(10);
    int cols;
    float ch = gui_s(70), gap = gui_s(8);
    float h = card_grid(a, y, n, ch, &cols);
    float cw = (a.w - gap * (float)(cols - 1)) / (float)cols;
    for (int i = 0; i < n; i++) {
        GuiRect r = { a.x + (cw + gap) * (float)(i % cols), y + (ch + gap) * (float)(i / cols), cw, ch };
        card_bg(r, false);
        caps(r.x + gui_s(12), r.y + gui_s(10), gui_s(TH->t_tiny + 1), TH->text_faint, rows[i].name);
        text(r.x + gui_s(12), r.y + gui_s(30), gui_s(TH->t_title - 4), GW_SEMIBOLD, TH->text, GUI_LEFT, pub_fmt(&rows[i], rows[i].a, b, sizeof(b)));
    }
    return y + h - a.y;
}

static float page_compare(GuiRect a, const ChartStyle *st, const StatsPublic *me, const StatsPublic *them, const char *name) {
    float y = a.y;
    if (!them) return page_public(a, NULL, name);
    PubRow rows[16];
    int n = pub_rows(me, them, rows);
    char b[64], b2[64];
    // Side-by-side cards: you (accent) vs them (info), a split bar of the two.
    int cols = DG_CLAMP((int)(a.w * 0.58f / gui_s(230)), 1, 3);
    float gap = gui_s(8), ch = gui_s(64);
    float left_w = a.w * 0.6f;
    float cw = (left_w - gap * (float)(cols - 1)) / (float)cols;
    text(a.x, y, gui_s(TH->t_small), GW_SEMIBOLD, TH->accent, GUI_LEFT, "You");
    text(a.x + gui_s(48), y, gui_s(TH->t_small), GW_SEMIBOLD, TH->info, GUI_LEFT, name);
    y += gd_line_height(gui_s(TH->t_small)) + gui_s(6);
    for (int i = 0; i < n; i++) {
        GuiRect r = { a.x + (cw + gap) * (float)(i % cols), y + (ch + gap) * (float)(i / cols), cw, ch };
        card_bg(r, false);
        float pad = gui_s(10);
        caps(r.x + pad, r.y + gui_s(8), gui_s(TH->t_tiny + 1), TH->text_faint, rows[i].name);
        float vy = r.y + gui_s(24);
        text(r.x + pad, vy, gui_s(TH->t_body + 1), GW_SEMIBOLD, TH->accent, GUI_LEFT, pub_fmt(&rows[i], rows[i].a, b, sizeof(b)));
        text(r.x + r.w - pad, vy, gui_s(TH->t_body + 1), GW_SEMIBOLD, TH->info, GUI_RIGHT, pub_fmt(&rows[i], rows[i].b, b2, sizeof(b2)));
        double tot = rows[i].a + rows[i].b;
        float f = tot > 0 ? (float)(rows[i].a / tot) : 0.5f;
        GuiRect bar = { r.x + pad, r.y + r.h - gui_s(10), r.w - pad * 2, gui_s(3) };
        gd_rect(gui_rect(bar.x, bar.y, bar.w * f, bar.h), TH->accent);
        gd_rect(gui_rect(bar.x + bar.w * f, bar.y, bar.w * (1 - f), bar.h), TH->info);
    }
    int rows_n = (n + cols - 1) / cols;
    float cards_h = (float)rows_n * (ch + gap);
    // Overlaid radar, normalized across the two players on each axis.
    static const char *const axes[6] = { "Kills", "Accuracy", "Headshot %", "Revives", "Distance", "Highest wave" };
    double va[6] = { me->kills, stats_ratio(me->shots_hit, me->shots_fired), stats_ratio(me->headshots, me->kills), me->revives_given, me->distance_m, me->highest_wave };
    double vb[6] = { them->kills, stats_ratio(them->shots_hit, them->shots_fired), stats_ratio(them->headshots, them->kills), them->revives_given, them->distance_m, them->highest_wave };
    static float ra[6], rb[6];
    for (int i = 0; i < 6; i++) {
        double mx = fmax(va[i], vb[i]);
        ra[i] = mx > 0 ? (float)(va[i] / mx) : 0;
        rb[i] = mx > 0 ? (float)(vb[i] / mx) : 0;
    }
    ChartRadarSpec rs;
    memset(&rs, 0, sizeof(rs));
    rs.axes = 6;
    rs.labels = axes;
    rs.polys = 2;
    rs.values[0] = ra;
    rs.values[1] = rb;
    rs.colors[0] = TH->accent;
    rs.colors[1] = TH->info;
    GuiRect rr = box(gui_rect(a.x + left_w + gap, y, a.w - left_w - gap, fmaxf(cards_h - gap, gui_s(300))), "You vs them");
    u32 k = mixk(base_key(), (u32)(va[0] + vb[0] * 7 + va[4] + vb[4]));
    int hv = chart_radar(&T.cradar, k, rr, st, &rs);
    if (hv >= 0) {
        static const int row_of_axis[6] = { 0, 3, 4, 5, 10, 7 };   // pub_rows order
        const PubRow *pr = &rows[row_of_axis[hv]];
        char t[160];
        snprintf(t, sizeof(t), "%s\nYou: %s\n%s: %s", axes[hv], pub_fmt(pr, va[hv], b, sizeof(b)), name, pub_fmt(pr, vb[hv], b2, sizeof(b2)));
        set_tip(t);
    }
    return y + fmaxf(cards_h, gui_s(300)) - a.y;
}

// ------------------------------------------------------------------ performance page

static float page_performance(GuiRect a, const ChartStyle *st) {
    float y = a.y, gap = gui_s(8), bh = gui_s(TH->button_h);
    y += empty_note(a, y);
    // Options: metric x range x view, moving average, the right axis.
    float w = (a.w - gap * 3) / 4.0f;
    gw_dropdown(gui_id("st_metric"), gui_rect(a.x, y, w, bh), k_metrics, MET_COUNT, &T.metric);
    gw_dropdown(gui_id("st_range"), gui_rect(a.x + (w + gap), y, w, bh), k_ranges, RG_COUNT, &T.range);
    gw_dropdown(gui_id("st_view"), gui_rect(a.x + (w + gap) * 2, y, w, bh), k_views, 2, &T.view);
    gw_dropdown(gui_id("st_metric2"), gui_rect(a.x + (w + gap) * 3, y, w, bh), k_metrics2, MET_COUNT + 1, &T.metric2);
    y += bh + gap;
    GuiRect tr = { a.x, y, gui_s(TH->button_h * 2.2f), bh };
    gw_toggle(gui_id("st_ma"), tr, &T.ma);
    text(tr.x + tr.w + gui_s(8), y + (bh - gd_line_height(gui_s(TH->t_small))) * 0.5f, gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT, "Moving average (5)");
    if (T.view == 0) {
        float lx = a.x + a.w;
        const char *names[3] = { "Abandoned", "Loss", "Win" };
        u32 cols[3] = { TH->text_dim, TH->danger, TH->ok };
        chart_legend(gui_rect(lx - gui_s(240), y, gui_s(240), bh), st, names, cols, 3);
    }
    y += bh + gap;
    perf_prepare();
    ChartSeries s[2];
    memset(s, 0, sizeof(s));
    s[0].y = T.py;
    s[0].name = k_metrics[T.metric];
    s[0].color = TH->accent;
    s[0].fill = true;
    s[0].ma_window = T.ma ? 5 : 0;
    s[1].y = T.py2;
    s[1].name = T.metric2 ? k_metrics[T.metric2 - 1] : "";
    s[1].color = TH->info;
    s[1].right_axis = true;
    s[1].dashed = true;
    ChartLineSpec ls;
    memset(&ls, 0, sizeof(ls));
    ls.n = T.pn;
    ls.nseries = T.metric2 ? 2 : 1;
    ls.s = s;
    ls.x_label = perf_xlabel;
    ls.y_fmt = axis_fmt;
    ls.y2_fmt = axis_fmt2;
    ls.user = (void *)(intptr_t)T.metric;
    ls.point_class = T.view == 0 ? T.pcls : NULL;
    ls.include_zero = true;
    ls.empty = "No matches in this range";
    char title[96];
    snprintf(title, sizeof(title), "%s %s", k_metrics[T.metric], T.view == 0 ? "per match" : "per day");
    GuiRect lr = box(gui_rect(a.x, y, a.w, gui_s(300)), title);
    int hv = chart_line(&T.perf, mixk(T.perf_key, T.ma), lr, st, &ls);
    if (hv >= 0) {
        char t[300], d[40], v[40], v2[40];
        metric_fmt(T.metric, T.py[hv], v, sizeof(v));
        if (T.view == 0) {
            const MatchRecord *m = match(T.pmatch[hv]);
            int number = (int)PS()->matches_played - (nmatches() - 1 - T.pmatch[hv]);
            snprintf(t, sizeof(t), "Match #%d · %s %s\n%s · %s · %d waves\n%s: %s", number, fday(d, sizeof(d), T.pday[hv]), ftime_of_day(v2, sizeof(v2), m->timestamp),
                     game_stats_map_name(m->map), k_results[m->result % MATCH_RESULT_COUNT], m->waves_reached, k_metrics[T.metric], v);
        } else snprintf(t, sizeof(t), "%s\n%s: %s", fday_full(d, sizeof(d), T.pday[hv]), k_metrics[T.metric], v);
        if (T.metric2) {
            size_t l = strlen(t);
            snprintf(t + l, sizeof(t) - l, "\n%s: %s", k_metrics[T.metric2 - 1], metric_fmt(T.metric2 - 1, T.py2[hv], v2, sizeof(v2)));
        }
        set_tip(t);
    }
    y += gui_s(300) + gap;
    // Kills per match histogram, accuracy vs kills scatter.
    float hw = (a.w - gap) * 0.5f, chh = gui_s(250);
    ChartHistSpec hs;
    memset(&hs, 0, sizeof(hs));
    hs.n = T.hist_n;
    hs.counts = T.hist;
    hs.label = hist_label;
    hs.marker = T.hist_mark;
    hs.marker_label = "Last match";
    hs.color = TH->accent;
    hs.empty = "No matches in this range";
    GuiRect hr = box(gui_rect(a.x, y, hw, chh), "Kills per match");
    hv = chart_histogram(&T.khist, T.perf_key, hr, st, &hs);
    if (hv >= 0) {
        char t[96], l[32];
        snprintf(t, sizeof(t), "%s kills\n%.0f match%s", hist_label(hv, l, sizeof(l), NULL), T.hist[hv], T.hist[hv] == 1 ? "" : "es");
        set_tip(t);
    }
    ChartScatterSpec ss;
    memset(&ss, 0, sizeof(ss));
    ss.n = T.sn;
    ss.x = T.sx;
    ss.y = T.sy;
    ss.colors = T.scol;
    ss.x_title = "Accuracy %";
    ss.y_title = "Kills";
    ss.empty = "No matches with shots in this range";
    GuiRect sr = box(gui_rect(a.x + hw + gap, y, hw, chh), "Accuracy vs kills");
    hv = chart_scatter(&T.scatter, T.perf_key, sr, st, &ss);
    if (hv >= 0) {
        const MatchRecord *m = match(T.smatch[hv]);
        char t[160], d[40];
        snprintf(t, sizeof(t), "%s · %s\nAccuracy %.1f %% · %u kills\n%s", fday(d, sizeof(d), stats_day_of(m->timestamp)), game_stats_map_name(m->map), T.sx[hv], m->kills,
                 k_results[m->result % MATCH_RESULT_COUNT]);
        set_tip(t);
    }
    y += chh;
    return y - a.y;
}

// ------------------------------------------------------------------ weapons page

static int g_wrows[STATS_WEAPON_COUNT], g_wn;

static double wcol_value(int slot, int col) {
    const WeaponStats *w = &PS()->weapons[slot];
    switch (col) {
    case WC_KILLS: return w->kills;
    case WC_SHOTS: return w->shots_fired;
    case WC_HITS: return w->shots_hit;
    case WC_ACC: return stats_weapon_accuracy(w);
    case WC_HS: return w->headshots;
    case WC_DMG: return w->damage_dealt;
    case WC_TIME: return w->time_equipped_sec;
    case WC_LONG: return w->longest_kill_m;
    default: return 0;
    }
}

static int cmp_wrow(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    int r;
    if (T.sort_col == WC_NAME) r = strcmp(game_stats_weapon_name(PS(), a), game_stats_weapon_name(PS(), b));
    else {
        double va = wcol_value(a, T.sort_col), vb = wcol_value(b, T.sort_col);
        r = va < vb ? -1 : va > vb;
    }
    if (!r) r = a - b;
    return T.sort_asc ? r : -r;
}

static void weapon_rows(void) {
    const PlayerStats *ps = PS();
    g_wn = 0;
    for (int i = 0; i < ps->weapon_count; i++) {
        const WeaponStats *w = &ps->weapons[i];
        if (w->shots_fired || w->kills || w->time_equipped_sec >= 1.0) g_wrows[g_wn++] = i;
    }
    qsort(g_wrows, (size_t)g_wn, sizeof(int), cmp_wrow);
}

static u32 weapon_color(int slot, const ChartStyle *st) { return st->series[slot % CHART_MAX_SERIES]; }

// Selecting a weapon opens its detail under the charts and scrolls there.
static void pick_weapon(int slot) {
    T.wsel = slot;
    T.wcmp_item = 0;
    if (slot >= 0) { T.scroll_req = -2.0f; T.scroll_req_frames = 3; }   // -2: the detail (its y is known after drawing)
}

static const char *wbar_label(int i, char *b, int n, void *user) {
    const int *order = (const int *)user;
    dg_strcpy(b, (size_t)n, game_stats_weapon_name(PS(), order[i]));
    return b;
}

static const char *pct_fmt(double v, char *b, int n, void *user) {
    (void)user;
    return chart_fmt_pct(b, n, v, v < 0.1 ? 1 : 0);
}

static const char *dist_bucket_label(int i, char *b, int n, void *user) {
    (void)user;
    const float *e = stats_kill_dist_edges();
    double k = imperial() ? 3.28084 : 1.0;
    if (i == STATS_KILL_DIST_BUCKETS - 1) snprintf(b, (size_t)n, "%.0f+", e[i] * k);
    else snprintf(b, (size_t)n, "%.0f", e[i] * k);
    return b;
}

static const char *match_xlabel(int i, char *b, int n, void *user) {
    int first = (int)(intptr_t)user;
    const MatchRecord *m = match(first + i);
    return m ? fday(b, n, stats_day_of(m->timestamp)) : "";
}

// The matches a weapon was used in (its per-match line): indices into the history.
static int g_wmatch[STATS_MAX_MATCHES];

static const char *wmatch_xlabel(int i, char *b, int n, void *user) {
    (void)user;
    const MatchRecord *m = match(g_wmatch[i]);
    return m ? fday(b, n, stats_day_of(m->timestamp)) : "";
}

static float weapon_detail(GuiRect a, float y, const ChartStyle *st) {
    const PlayerStats *ps = PS();
    int s = T.wsel;
    const WeaponStats *w = &ps->weapons[s];
    float gap = gui_s(8), bh = gui_s(TH->button_h);
    float y0 = y;
    y += section(a, y, game_stats_weapon_name(ps, s));
    // Compare dropdown + close.
    const char *items[STATS_WEAPON_COUNT + 1];
    int map[STATS_WEAPON_COUNT + 1], ni = 0;
    items[ni] = "Compare with...";
    map[ni++] = -1;
    for (int i = 0; i < g_wn; i++) if (g_wrows[i] != s) { items[ni] = game_stats_weapon_name(ps, g_wrows[i]); map[ni++] = g_wrows[i]; }
    T.wcmp_item = DG_CLAMP(T.wcmp_item, 0, ni - 1);
    gw_dropdown(gui_id("st_wcmp"), gui_rect(a.x, y, gui_s(220), bh), items, ni, &T.wcmp_item);
    bool back = gui_nav(NAV_BACK);   // Esc / B closes the detail before the screen
    if (back) gui_nav_consume(NAV_BACK);
    if (gw_button(gui_id("st_wclose"), gui_rect(a.x + a.w - gui_s(110), y, gui_s(110), bh), "Close", GB_SECONDARY, true) || back) { T.wsel = -1; gui_set_focus(0); return y + bh - y0; }
    int cmp = map[T.wcmp_item];
    y += bh + gap;
    // Its numbers (left) and the radar (right).
    char b[48], b2[48];
    float mins = (float)(w->time_equipped_sec / 60.0);
    struct { const char *k; char v[48]; } kv[12];
    int nk = 0;
#define KV(key, ...) do { kv[nk].k = key; snprintf(kv[nk].v, sizeof(kv[nk].v), __VA_ARGS__); nk++; } while (0)
    KV("Kills", "%s", chart_fmt_int(b, sizeof(b), w->kills));
    KV("Headshots", "%s (%s)", chart_fmt_int(b, sizeof(b), w->headshots), chart_fmt_pct(b2, sizeof(b2), stats_ratio(w->headshots, w->kills), 1));
    KV("Shots fired", "%s", chart_fmt_int(b, sizeof(b), w->shots_fired));
    KV("Hits", "%s", chart_fmt_int(b, sizeof(b), w->shots_hit));
    KV("Accuracy", "%s", chart_fmt_pct(b, sizeof(b), stats_weapon_accuracy(w), 1));
    KV("Headshot hits", "%s", chart_fmt_int(b, sizeof(b), w->headshot_hits));
    KV("Damage", "%s", chart_fmt_int(b, sizeof(b), w->damage_dealt));
    KV("Damage per shot", "%.1f", stats_ratio(w->damage_dealt, w->shots_fired));
    KV("Time equipped", "%s", chart_fmt_duration(b, sizeof(b), w->time_equipped_sec));
    KV("Kills per minute", "%.2f", stats_ratio(w->kills, mins));
    KV("Longest kill", "%s", fdist(b, sizeof(b), w->longest_kill_m));
#undef KV
    float lw = a.w * 0.42f, rowh = gui_s(TH->row_h) * 0.9f;
    for (int i = 0; i < nk; i++) {
        GuiRect r = { a.x, y + rowh * (float)i, lw, rowh };
        gw_label(r, kv[i].k, gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT);
        gw_label(r, kv[i].v, gui_s(TH->t_small), GW_MEDIUM, TH->text, GUI_RIGHT);
        gd_rect(gui_rect(r.x, r.y + r.h - 1, r.w, 1), TH->panel_header);
    }
    // Radar: each axis normalized by your best weapon on it.
    static const char *const axes[5] = { "Accuracy", "Headshot %", "Kills / min", "Damage / shot", "Longest kill" };
    float best[5] = { 0 };
    for (int i = 0; i < g_wn; i++) {
        const WeaponStats *x = &ps->weapons[g_wrows[i]];
        float v[5] = { stats_weapon_accuracy(x), stats_ratio(x->headshots, x->kills), stats_ratio(x->kills, x->time_equipped_sec / 60.0), stats_ratio(x->damage_dealt, x->shots_fired), x->longest_kill_m };
        for (int k = 0; k < 5; k++) best[k] = fmaxf(best[k], v[k]);
    }
    static float rv[2][5];
    const WeaponStats *ws2[2] = { w, cmp >= 0 ? &ps->weapons[cmp] : NULL };
    for (int p = 0; p < 2; p++) {
        if (!ws2[p]) continue;
        const WeaponStats *x = ws2[p];
        float v[5] = { stats_weapon_accuracy(x), stats_ratio(x->headshots, x->kills), stats_ratio(x->kills, x->time_equipped_sec / 60.0), stats_ratio(x->damage_dealt, x->shots_fired), x->longest_kill_m };
        for (int k = 0; k < 5; k++) rv[p][k] = best[k] > 0 ? v[k] / best[k] : 0;
    }
    ChartRadarSpec rs;
    memset(&rs, 0, sizeof(rs));
    rs.axes = 5;
    rs.labels = axes;
    rs.polys = cmp >= 0 ? 2 : 1;
    rs.values[0] = rv[0];
    rs.values[1] = rv[1];
    rs.colors[0] = TH->accent;
    rs.colors[1] = TH->info;
    float radar_h = fmaxf(rowh * (float)nk, gui_s(240));
    GuiRect rr = box(gui_rect(a.x + lw + gap * 2, y, a.w - lw - gap * 2, radar_h), cmp >= 0 ? "Profile (vs your best weapon on each axis) · compared" : "Profile (vs your best weapon on each axis)");
    int hv = chart_radar(&T.wradar, mixk(base_key(), (u32)(s * 64 + cmp + 1)), rr, st, &rs);
    if (hv >= 0) {
        char t[160];
        snprintf(t, sizeof(t), "%s\n%s: %.0f %% of your best", axes[hv], game_stats_weapon_name(ps, s), rv[0][hv] * 100.0f);
        if (cmp >= 0) { size_t l = strlen(t); snprintf(t + l, sizeof(t) - l, "\n%s: %.0f %%", game_stats_weapon_name(ps, cmp), rv[1][hv] * 100.0f); }
        set_tip(t);
    }
    y += radar_h + gap;
    // Per match it was used in (the last 50 of them): its kills and accuracy; its kill distances.
    int nm = nmatches(), n = 0;
    static float wk[STATS_MAX_MATCHES], wa[STATS_MAX_MATCHES];
    for (int i = nm - 1; i >= 0 && n < 50; i--) {
        const MatchRecord *m = match(i);
        if (m->shots_per_weapon[s] || m->kills_per_weapon[s]) g_wmatch[n++] = i;
    }
    for (int i = 0; i < n / 2; i++) { int t = g_wmatch[i]; g_wmatch[i] = g_wmatch[n - 1 - i]; g_wmatch[n - 1 - i] = t; }   // oldest first
    for (int i = 0; i < n; i++) {
        const MatchRecord *m = match(g_wmatch[i]);
        wk[i] = (float)m->kills_per_weapon[s];
        wa[i] = m->shots_per_weapon[s] ? 100.0f * fminf(1.0f, (float)m->hits_per_weapon[s] / (float)m->shots_per_weapon[s]) : NAN;
    }
    ChartSeries se[2];
    memset(se, 0, sizeof(se));
    se[0].y = wk;
    se[0].name = "Kills";
    se[0].color = TH->accent;
    se[0].fill = true;
    se[1].y = wa;
    se[1].name = "Accuracy";
    se[1].color = TH->info;
    se[1].right_axis = true;
    se[1].dashed = true;
    ChartLineSpec ls;
    memset(&ls, 0, sizeof(ls));
    ls.n = n;
    ls.nseries = 2;
    ls.s = se;
    ls.x_label = wmatch_xlabel;
    ls.y2_fmt = acc_axis_fmt;
    ls.include_zero = true;
    ls.empty = "Not used in the recorded matches";
    float hw = (a.w - gap) * 0.5f, chh = gui_s(240);
    GuiRect lr = box(gui_rect(a.x, y, hw, chh), "Kills (left) and accuracy (right), matches it was used in");
    hv = chart_line(&T.wline, mixk(base_key(), (u32)s), lr, st, &ls);
    if (hv >= 0) {
        char t[120], d[32];
        const MatchRecord *m = match(g_wmatch[hv]);
        snprintf(t, sizeof(t), "%s · %s\nKills: %s\nAccuracy: %s", fday(d, sizeof(d), stats_day_of(m->timestamp)), game_stats_map_name(m->map), isnan(wk[hv]) ? "-" : chart_fmt_int(b, sizeof(b), wk[hv]),
                 isnan(wa[hv]) ? "-" : chart_fmt_pct(b2, sizeof(b2), wa[hv] / 100.0, 1));
        set_tip(t);
    }
    static float dh[STATS_KILL_DIST_BUCKETS];
    for (int i = 0; i < STATS_KILL_DIST_BUCKETS; i++) dh[i] = (float)w->kill_dist[i];
    const float *e = stats_kill_dist_edges();
    int lb = stats_kill_dist_bucket(w->longest_kill_m);
    float frac = lb < STATS_KILL_DIST_BUCKETS - 1 ? (w->longest_kill_m - e[lb]) / (e[lb + 1] - e[lb]) : 0.5f;
    char ml[48];
    snprintf(ml, sizeof(ml), "Longest %s", fdist(b, sizeof(b), w->longest_kill_m));
    ChartHistSpec hs;
    memset(&hs, 0, sizeof(hs));
    hs.n = STATS_KILL_DIST_BUCKETS;
    hs.counts = dh;
    hs.label = dist_bucket_label;
    hs.marker = w->kills ? (float)lb + saturate(frac) : -1.0f;
    hs.marker_label = ml;
    hs.color = weapon_color(s, st);
    hs.empty = "No kills with it yet";
    GuiRect hr = box(gui_rect(a.x + hw + gap, y, hw, chh), imperial() ? "Kill distance (ft)" : "Kill distance (m)");
    hv = chart_histogram(&T.wdist, mixk(base_key(), (u32)s), hr, st, &hs);
    if (hv >= 0) {
        char t[96], l0[24];
        dist_bucket_label(hv, l0, sizeof(l0), NULL);
        snprintf(t, sizeof(t), "From %s %s\n%.0f kill%s", l0, imperial() ? "ft" : "m", dh[hv], dh[hv] == 1 ? "" : "s");
        set_tip(t);
    }
    y += chh;
    return y - y0;
}

static float page_weapons(GuiRect a, const ChartStyle *st) {
    const PlayerStats *ps = PS();
    float y = a.y, gap = gui_s(8);
    y += empty_note(a, y);
    weapon_rows();
    if (!g_wn) {
        text(a.x, y, gui_s(TH->t_body), GW_REGULAR, TH->text_dim, GUI_LEFT, "No weapon has been used yet.");
        return y + gui_s(30) - a.y;
    }
    if (T.wsel >= ps->weapon_count) T.wsel = -1;
    // The table (header click: sort; again: reverse).
    static const float k_w[WC_COUNT] = { 0.21f, 0.08f, 0.09f, 0.09f, 0.09f, 0.09f, 0.11f, 0.12f, 0.12f };
    float rh = gui_s(TH->row_h), x = a.x;
    float fs = gui_s(TH->t_small);
    for (int c = 0; c < WC_COUNT; c++) {
        float cw = a.w * k_w[c];
        GuiRect hr = { x, y, cw, rh };
        GuiInteract in = gui_interact(gui_idi("st_wcol", c), hr, GUI_FOCUSABLE);
        if (in.clicked) {
            if (T.sort_col == c) T.sort_asc = !T.sort_asc;
            else { T.sort_col = c; T.sort_asc = c == WC_NAME; }
            gui_sound(TH->snd_click);
        }
        u32 col = T.sort_col == c ? TH->text : in.hot ? TH->text_dim : TH->text_faint;
        int al = c == WC_NAME ? GUI_LEFT : GUI_RIGHT;
        float tx = c == WC_NAME ? x + gui_s(6) : x + cw - gui_s(6);
        GuiText t = { fs, GW_SEMIBOLD, col, al, TH->tracking_caps, true, false, 0 };
        float tw = gd_text(&t, tx, y + (rh - gd_line_height(fs)) * 0.5f, k_wcols[c]);
        if (T.sort_col == c) {
            float s = gui_s(6), ax = c == WC_NAME ? tx + tw + gui_s(5) : tx - tw - gui_s(5) - s;
            u32 ac = TH->accent;
            float ay = y + (rh - s) * 0.5f;
            if (T.sort_asc) ui_tri(ax, ay + s, ax + s, ay + s, ax + s * 0.5f, ay, ac);
            else ui_tri(ax, ay, ax + s, ay, ax + s * 0.5f, ay + s, ac);
        }
        if (in.focused) gw_focus_ring(hr, gui_s(2), 1.0f);
        x += cw;
    }
    y += rh;
    gd_rect(gui_rect(a.x, y - 1, a.w, 1), TH->panel_border);
    for (int i = 0; i < g_wn; i++) {
        int s = g_wrows[i];
        const WeaponStats *w = &ps->weapons[s];
        GuiRect r = { a.x, y, a.w, rh };
        GuiInteract in = gui_interact(gui_idi("st_wrow", s), r, GUI_FOCUSABLE);
        if (in.clicked) { pick_weapon(T.wsel == s ? -1 : s); gui_sound(TH->snd_click); }
        bool sel = T.wsel == s;
        if (sel || in.hot) gd_rect(r, sel ? UI_RGBA(255, 255, 255, 16) : UI_RGBA(255, 255, 255, 8));
        if (sel) gd_rect(gui_rect(r.x, r.y, gui_s(3), r.h), TH->accent);
        if (in.focused) gw_focus_ring(r, gui_s(2), 1.0f);
        char b[48];
        float cx = a.x;
        for (int c = 0; c < WC_COUNT; c++) {
            float cw = a.w * k_w[c];
            const char *v = b;
            switch (c) {
            case WC_NAME: v = game_stats_weapon_name(ps, s); break;
            case WC_ACC: chart_fmt_pct(b, sizeof(b), stats_weapon_accuracy(w), 1); break;
            case WC_DMG: chart_fmt_int(b, sizeof(b), w->damage_dealt); break;
            case WC_TIME: chart_fmt_duration(b, sizeof(b), w->time_equipped_sec); break;
            case WC_LONG: fdist(b, sizeof(b), w->longest_kill_m); break;
            default: chart_fmt_int(b, sizeof(b), wcol_value(s, c)); break;
            }
            if (c == WC_NAME) {
                gd_round(gui_rect(cx + gui_s(8), y + rh * 0.5f - gui_s(4), gui_s(8), gui_s(8)), gui_s(2), weapon_color(s, st));
                text_w(cx + gui_s(22), y + (rh - gd_line_height(fs)) * 0.5f, fs, GW_MEDIUM, TH->text, GUI_LEFT, cw - gui_s(24), v);
            } else text(cx + cw - gui_s(6), y + (rh - gd_line_height(fs)) * 0.5f, fs, GW_REGULAR, T.sort_col == c ? TH->text : TH->text_dim, GUI_RIGHT, v);
            cx += cw;
        }
        y += rh;
    }
    y += gap * 2;
    // Kills by weapon / accuracy by weapon (kills order), with the overall accuracy for reference.
    static int order[STATS_WEAPON_COUNT];
    static float kv[STATS_WEAPON_COUNT], av[STATS_WEAPON_COUNT];
    static u32 bc[STATS_WEAPON_COUNT];
    memcpy(order, g_wrows, sizeof(int) * (size_t)g_wn);
    for (int i = 1; i < g_wn; i++)
        for (int j = i; j > 0 && ps->weapons[order[j]].kills > ps->weapons[order[j - 1]].kills; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    int hl = -1;
    for (int i = 0; i < g_wn; i++) {
        kv[i] = (float)ps->weapons[order[i]].kills;
        av[i] = ps->weapons[order[i]].shots_fired ? stats_weapon_accuracy(&ps->weapons[order[i]]) : NAN;
        bc[i] = weapon_color(order[i], st);
        if (order[i] == T.wsel) hl = i;
    }
    float hw = (a.w - gap) * 0.5f, chh = fmaxf(gui_s(170), gui_s(26) * (float)g_wn + gui_s(60));
    u32 k = mixk(base_key(), (u32)(T.wsel + 1));
    ChartBarSpec bs;
    memset(&bs, 0, sizeof(bs));
    bs.n = g_wn;
    bs.groups = 1;
    bs.v = kv;
    bs.bar_colors = bc;
    bs.horizontal = true;
    bs.ref = NAN;
    bs.label = wbar_label;
    bs.user = order;
    bs.value_labels = true;
    bs.highlight = hl;
    bs.empty = "No kills yet";
    GuiRect r1 = box(gui_rect(a.x, y, hw, chh), "Kills by weapon");
    int hv = chart_bar(&T.wkills, k, r1, st, &bs);
    if (hv >= 0) {
        char t[96], b[32];
        snprintf(t, sizeof(t), "%s\n%s kills", game_stats_weapon_name(ps, order[hv]), chart_fmt_int(b, sizeof(b), kv[hv]));
        set_tip(t);
        if (gui_mouse_pressed(0)) pick_weapon(order[hv]);
    }
    char ref[48], b[32];
    snprintf(ref, sizeof(ref), "Overall %s", chart_fmt_pct(b, sizeof(b), stats_overall_accuracy(ps), 1));
    bs.v = av;
    bs.ref = stats_overall_accuracy(ps);
    bs.ref_label = ref;
    bs.fmt = pct_fmt;
    bs.empty = "No shots yet";
    GuiRect r2 = box(gui_rect(a.x + hw + gap, y, hw, chh), "Accuracy by weapon");
    hv = chart_bar(&T.wacc, k, r2, st, &bs);
    if (hv >= 0) {
        const WeaponStats *w = &ps->weapons[order[hv]];
        char t[128], b2[32], b3[32];
        snprintf(t, sizeof(t), "%s\n%s accuracy\n%s / %s hits", game_stats_weapon_name(ps, order[hv]), chart_fmt_pct(b, sizeof(b), stats_weapon_accuracy(w), 1),
                 chart_fmt_int(b2, sizeof(b2), w->shots_hit), chart_fmt_int(b3, sizeof(b3), w->shots_fired));
        set_tip(t);
        if (gui_mouse_pressed(0)) pick_weapon(order[hv]);
    }
    y += chh + gap;
    // Shares: kills, time equipped.
    static ChartDonutSeg dk[STATS_WEAPON_COUNT], dt[STATS_WEAPON_COUNT];
    double tk = 0, tt = 0;
    for (int i = 0; i < g_wn; i++) {
        const WeaponStats *w = &ps->weapons[order[i]];
        dk[i].value = (float)w->kills;
        dk[i].color = bc[i];
        dk[i].label = game_stats_weapon_name(ps, order[i]);
        dt[i] = dk[i];
        dt[i].value = (float)w->time_equipped_sec;
        tk += w->kills;
        tt += w->time_equipped_sec;
    }
    char c1[32], c2[32];
    float dh = gui_s(220);
    GuiRect d1 = box(gui_rect(a.x, y, hw, dh), "Share of kills");
    hv = chart_donut(&T.wdk, k, d1, st, dk, g_wn, chart_fmt_int(c1, sizeof(c1), tk), "kills", true);
    if (hv >= 0) {
        char t[96];
        snprintf(t, sizeof(t), "%s\n%s kills (%s)", dk[hv].label, chart_fmt_int(b, sizeof(b), dk[hv].value), chart_fmt_pct(c2, sizeof(c2), stats_ratio(dk[hv].value, tk), 1));
        set_tip(t);
    }
    GuiRect d2 = box(gui_rect(a.x + hw + gap, y, hw, dh), "Share of time equipped");
    hv = chart_donut(&T.wdt, k, d2, st, dt, g_wn, chart_fmt_duration(c1, sizeof(c1), tt), "equipped", true);
    if (hv >= 0) {
        char t[96];
        snprintf(t, sizeof(t), "%s\n%s (%s)", dt[hv].label, chart_fmt_duration(b, sizeof(b), dt[hv].value), chart_fmt_pct(c2, sizeof(c2), stats_ratio(dt[hv].value, tt), 1));
        set_tip(t);
    }
    y += dh + gap;
    // Kills per weapon over the matches (the top 5 + the rest).
    int nm = nmatches(), first = DG_MAX(0, nm - 50), n = nm - first;
    static float sv[6][STATS_MAX_MATCHES];
    static const float *sp[6];
    static u32 scol[6];
    static const char *snames[6];
    int ns = DG_MIN(g_wn, 5);
    for (int si = 0; si < 6; si++) sp[si] = sv[si];
    for (int i = 0; i < n; i++) {
        const MatchRecord *m = match(first + i);
        float rest = (float)m->kills;
        for (int si = 0; si < ns; si++) { sv[si][i] = (float)m->kills_per_weapon[order[si]]; rest -= sv[si][i]; }
        sv[ns][i] = fmaxf(0.0f, rest);
    }
    for (int si = 0; si < ns; si++) { scol[si] = bc[si]; snames[si] = game_stats_weapon_name(ps, order[si]); }
    scol[ns] = UI_RGBA(150, 150, 150, 200);
    snames[ns] = "Other";
    ChartStackSpec ss;
    memset(&ss, 0, sizeof(ss));
    ss.n = n;
    ss.nseries = ns + 1;
    ss.y = sp;
    ss.colors = scol;
    ss.x_label = match_xlabel;
    ss.user = (void *)(intptr_t)first;
    ss.empty = "Needs two recorded matches";
    float sh = gui_s(240);
    GuiRect sr = box(gui_rect(a.x, y, a.w, sh), "Kills per weapon over the last matches");
    chart_legend(gui_rect(sr.x + sr.w * 0.45f, sr.y - gui_s(22), sr.w * 0.55f, gui_s(18)), st, snames, scol, ns + 1);
    int band = -1;
    hv = chart_stacked_area(&T.wstack, k, sr, st, &ss, &band);
    if (hv >= 0) {
        const MatchRecord *m = match(first + hv);
        char t[160], d[32];
        size_t l = (size_t)snprintf(t, sizeof(t), "%s · %u kills", fday(d, sizeof(d), stats_day_of(m->timestamp)), m->kills);
        for (int si = 0; si <= ns && l < sizeof(t); si++)
            if (sv[si][hv] > 0) l += (size_t)snprintf(t + l, sizeof(t) - l, "\n%s%s: %.0f", si == band ? "> " : "", snames[si], sv[si][hv]);
        set_tip(t);
    }
    y += sh + gap;
    if (T.wsel >= 0) {
        T.detail_y = y - a.y;
        y += gap + weapon_detail(a, y, st);
    }
    return y - a.y;
}

// ------------------------------------------------------------------ movement page

static float page_movement(GuiRect a, const ChartStyle *st) {
    const PlayerStats *ps = PS();
    float y = a.y, gap = gui_s(8);
    y += empty_note(a, y);
    char v[4][48];
    fdist(v[0], 48, ps->total_distance_m);
    chart_fmt_int(v[1], 48, (double)ps->steps);
    chart_fmt_int(v[2], 48, ps->jumps);
    fdist(v[3], 48, stats_ratio(ps->total_distance_m, DG_MAX(ps->matches_played, 1u)));
    static const char *const titles[4] = { "Total distance", "Steps", "Jumps", "Average per match" };
    float cw = (a.w - gap * 3) / 4.0f, ch = gui_s(80);
    for (int i = 0; i < 4; i++) {
        GuiRect r = { a.x + (cw + gap) * (float)i, y, cw, ch };
        card_bg(r, false);
        caps(r.x + gui_s(12), r.y + gui_s(10), gui_s(TH->t_tiny + 1), TH->text_faint, titles[i]);
        text_w(r.x + gui_s(12), r.y + gui_s(32), gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT, r.w - gui_s(24), v[i]);
    }
    y += ch + gap;
    GuiRect tr = { a.x, y, gui_s(TH->button_h * 2.2f), gui_s(TH->button_h) };
    gw_toggle(gui_id("st_ma_move"), tr, &T.ma);
    text(tr.x + tr.w + gui_s(8), y + (tr.h - gd_line_height(gui_s(TH->t_small))) * 0.5f, gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT, "Moving average (5)");
    y += tr.h + gap;
    int nm = nmatches();
    static float d[STATS_MAX_MATCHES];
    for (int i = 0; i < nm; i++) d[i] = (float)(match(i)->distance_m / dist_unit());
    ChartSeries s;
    memset(&s, 0, sizeof(s));
    s.y = d;
    s.name = "Distance";
    s.color = TH->accent;
    s.fill = true;
    s.ma_window = T.ma ? 5 : 0;
    ChartLineSpec ls;
    memset(&ls, 0, sizeof(ls));
    ls.n = nm;
    ls.nseries = 1;
    ls.s = &s;
    ls.x_label = match_xlabel;
    ls.user = (void *)(intptr_t)0;
    ls.y_fmt = NULL;
    ls.include_zero = true;
    ls.empty = "No matches recorded yet";
    char title[64];
    snprintf(title, sizeof(title), "Distance per match (%s)", dist_unit_name());
    float lh = gui_s(300);
    GuiRect lr = box(gui_rect(a.x, y, a.w, lh), title);
    int hv = chart_line(&T.move, mixk(base_key(), T.ma), lr, st, &ls);
    if (hv >= 0) {
        const MatchRecord *m = match(hv);
        char t[160], dd[32], b[32], b2[32];
        snprintf(t, sizeof(t), "%s · %s\n%s · %s steps · %u jumps", fday(dd, sizeof(dd), stats_day_of(m->timestamp)), game_stats_map_name(m->map), fdist(b, sizeof(b), m->distance_m),
                 chart_fmt_int(b2, sizeof(b2), m->steps), m->jumps);
        set_tip(t);
    }
    y += lh;
    return y - a.y;
}

// ------------------------------------------------------------------ match history

static const char *wave_label(int i, char *b, int n, void *user) {
    int seg = (int)(intptr_t)user;
    if (seg > 0) snprintf(b, (size_t)n, "%dm", i * seg / 60);
    else snprintf(b, (size_t)n, "W%d", i + 1);
    return b;
}

static float match_detail(GuiRect a, float y, const ChartStyle *st) {
    const PlayerStats *ps = PS();
    const MatchRecord *m = match(T.msel);
    float y0 = y, gap = gui_s(8), bh = gui_s(TH->button_h);
    if (!m) { T.msel = -1; return 0; }
    bool back = gui_nav(NAV_BACK);   // Esc / B: back to the list before the screen closes
    if (back) gui_nav_consume(NAV_BACK);
    if (gw_button(gui_id("st_mback"), gui_rect(a.x, y, gui_s(120), bh), "Back", GB_SECONDARY, true) || back) { T.msel = -1; gui_set_focus(0); return bh; }
    char d[48], tm[16], b[48], b2[48];
    snprintf(b, sizeof(b), "%s %s · %s", fday_full(d, sizeof(d), stats_day_of(m->timestamp)), ftime_of_day(tm, sizeof(tm), m->timestamp), game_stats_map_name(m->map));
    text(a.x + gui_s(134), y + (bh - gd_line_height(gui_s(TH->t_body))) * 0.5f, gui_s(TH->t_body), GW_MEDIUM, TH->text, GUI_LEFT, b);
    u32 rc = m->result == MATCH_WIN ? TH->ok : m->result == MATCH_LOSS ? TH->danger : TH->text_dim;
    snprintf(b2, sizeof(b2), "%s · %s", k_results[m->result % MATCH_RESULT_COUNT], chart_fmt_duration(b, sizeof(b), m->duration_sec));
    text(a.x + a.w, y + (bh - gd_line_height(gui_s(TH->t_body))) * 0.5f, gui_s(TH->t_body), GW_SEMIBOLD, rc, GUI_RIGHT, b2);
    y += bh + gap;
    // Timeline: kills per wave (or per segment) with downs / revives.
    int seg = m->segment_sec;
    int nw = seg ? DG_CLAMP((int)ceilf(m->duration_sec / (float)seg), 1, STATS_MAX_WAVES) : DG_CLAMP((int)m->waves_reached, 1, STATS_MAX_WAVES);
    static float kw[STATS_MAX_WAVES];
    for (int i = 0; i < nw; i++) kw[i] = (float)m->kills_per_wave[i];
    ChartTimelineSpec ts;
    memset(&ts, 0, sizeof(ts));
    ts.n = nw;
    ts.bars = kw;
    ts.downs = m->downs_per_wave;
    ts.revives = m->revives_per_wave;
    ts.label = wave_label;
    ts.user = (void *)(intptr_t)seg;
    ts.empty = "Nothing happened in this match";
    float th = gui_s(240);
    GuiRect tr = box(gui_rect(a.x, y, a.w * 0.62f, th), seg ? "Kills over time (downs red, revives green)" : "Kills per wave (downs red, revives green)");
    u32 k = mixk(base_key(), (u32)T.msel);
    int hv = chart_timeline(&T.timeline, k, tr, st, &ts);
    if (hv >= 0) {
        char t[128], l[16];
        wave_label(hv, l, sizeof(l), (void *)(intptr_t)seg);
        snprintf(t, sizeof(t), "%s%s\n%.0f kills\n%u downs · %u revives", seg ? "From " : "Wave ", seg ? l : l + 1, kw[hv], m->downs_per_wave[hv], m->revives_per_wave[hv]);
        set_tip(t);
    }
    // Kills per weapon.
    static ChartDonutSeg ds[STATS_WEAPON_COUNT];
    int nd = 0;
    for (int i = 0; i < ps->weapon_count; i++)
        if (m->kills_per_weapon[i]) { ds[nd].value = m->kills_per_weapon[i]; ds[nd].color = weapon_color(i, st); ds[nd].label = game_stats_weapon_name(ps, i); nd++; }
    GuiRect dr = box(gui_rect(a.x + a.w * 0.62f + gap, y, a.w * 0.38f - gap, th), "Kills per weapon");
    hv = chart_donut(&T.mdonut, k, dr, st, ds, nd, chart_fmt_int(b, sizeof(b), m->kills), "kills", false);
    if (hv >= 0) {
        char t[96];
        snprintf(t, sizeof(t), "%s\n%.0f kills", ds[hv].label, ds[hv].value);
        set_tip(t);
    }
    y += th + gap;
    // This match vs your lifetime averages.
    y += section(a, y, "This match vs your average");
    double mp = DG_MAX(ps->matches_played, 1u);
    struct { const char *k; double v, avg; int kind; bool lower; } rows[] = {
        { "Kills", m->kills, ps->kills / mp, 0, false },
        { "Deaths", m->deaths, ps->deaths / mp, 0, true },
        { "Accuracy", stats_match_accuracy(m), stats_overall_accuracy(ps), 1, false },
        { "Headshots", m->headshots, ps->headshots / mp, 0, false },
        { "Damage dealt", m->damage_dealt, ps->damage_dealt / mp, 0, false },
        { "Damage taken", m->damage_taken, ps->damage_taken / mp, 0, true },
        { "Revives", m->revives, ps->revives_given / mp, 0, false },
        { "Distance", m->distance_m, ps->total_distance_m / mp, 2, false },
        { "Best streak", m->best_streak, 0, 0, false },
        { "Duration", m->duration_sec, ps->time_played_sec / mp, 3, false },
    };
    float rh = gui_s(TH->row_h) * 0.9f, cw = a.w / 2.0f;
    int nrows = (int)ARRAY_COUNT(rows);
    for (int i = 0; i < nrows; i++) {
        GuiRect r = { a.x + cw * (float)(i % 2), y + rh * (float)(i / 2), cw - gap, rh };
        char vv[48], av[48];
        switch (rows[i].kind) {
        case 1: chart_fmt_pct(vv, sizeof(vv), rows[i].v, 1); chart_fmt_pct(av, sizeof(av), rows[i].avg, 1); break;
        case 2: fdist(vv, sizeof(vv), rows[i].v); fdist(av, sizeof(av), rows[i].avg); break;
        case 3: chart_fmt_duration(vv, sizeof(vv), rows[i].v); chart_fmt_duration(av, sizeof(av), rows[i].avg); break;
        default: chart_fmt_int(vv, sizeof(vv), rows[i].v); chart_fmt_num(av, sizeof(av), rows[i].avg, 1); break;
        }
        if (i == 8) snprintf(av, sizeof(av), "best %u", ps->best_killstreak);
        gw_label(r, rows[i].k, gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT);
        float fs = gui_s(TH->t_small);
        text(r.x + r.w * 0.62f, r.y + (r.h - gd_line_height(fs)) * 0.5f, fs, GW_SEMIBOLD, TH->text, GUI_RIGHT, vv);
        text(r.x + r.w - gui_s(18), r.y + (r.h - gd_line_height(fs)) * 0.5f, fs, GW_REGULAR, TH->text_faint, GUI_RIGHT, av);
        if (i != 8 && fabs(rows[i].v - rows[i].avg) > fabs(rows[i].avg) * 0.02 + 1e-6) {
            bool up = rows[i].v > rows[i].avg;
            delta_arrow(r.x + r.w - gui_s(12), r.y + (r.h - gui_s(7)) * 0.5f, gui_s(7), up, up != rows[i].lower);
        }
        gd_rect(gui_rect(r.x, r.y + r.h - 1, r.w, 1), TH->panel_header);
    }
    y += rh * (float)((nrows + 1) / 2);
    return y - y0;
}

static float page_history(GuiRect a, const ChartStyle *st) {
    float y = a.y;
    y += empty_note(a, y);
    int nm = nmatches();
    if (T.msel >= nm) T.msel = -1;
    if (T.msel >= 0) return y + match_detail(a, y, st) - a.y;
    if (!nm) return y - a.y;
    static const char *const cols[8] = { "Date", "Map", "Result", "Waves", "Kills", "Deaths", "Accuracy", "Duration" };
    static const float cw[8] = { 0.2f, 0.2f, 0.12f, 0.08f, 0.09f, 0.09f, 0.11f, 0.11f };
    float rh = gui_s(TH->row_h), fs = gui_s(TH->t_small);
    float x = a.x;
    for (int c = 0; c < 8; c++) {
        GuiText t = { fs, GW_SEMIBOLD, TH->text_faint, c < 3 ? GUI_LEFT : GUI_RIGHT, TH->tracking_caps, true, false, 0 };
        gd_text(&t, c < 3 ? x + gui_s(8) : x + a.w * cw[c] - gui_s(8), y + (rh - gd_line_height(fs)) * 0.5f, cols[c]);
        x += a.w * cw[c];
    }
    y += rh;
    gd_rect(gui_rect(a.x, y - 1, a.w, 1), TH->panel_border);
    for (int i = nm - 1; i >= 0; i--) {
        const MatchRecord *m = match(i);
        GuiRect r = { a.x, y, a.w, rh };
        // Off-screen rows only take their space (a 100-row list stays cheap).
        GuiRect clip = gd_clip();
        if (r.y + r.h < clip.y || r.y > clip.y + clip.h) { y += rh; continue; }
        GuiInteract in = gui_interact(gui_idi("st_mrow", i), r, GUI_FOCUSABLE);
        if (in.clicked) { T.msel = i; gui_set_focus(0); gui_sound(TH->snd_click); }
        if (in.hot) gd_rect(r, UI_RGBA(255, 255, 255, 9));
        else if (i % 2) gd_rect(r, UI_RGBA(255, 255, 255, 3));
        if (in.focused) gw_focus_ring(r, gui_s(2), 1.0f);
        char v[8][48];
        char tm[16];
        fday(v[0], 48, stats_day_of(m->timestamp));
        size_t l = strlen(v[0]);
        snprintf(v[0] + l, 48 - l, "  %s", ftime_of_day(tm, sizeof(tm), m->timestamp));
        dg_strcpy(v[1], 48, game_stats_map_name(m->map));
        dg_strcpy(v[2], 48, k_results[m->result % MATCH_RESULT_COUNT]);
        if (m->waves_reached) chart_fmt_int(v[3], 48, m->waves_reached);
        else dg_strcpy(v[3], 48, "-");
        chart_fmt_int(v[4], 48, m->kills);
        chart_fmt_int(v[5], 48, m->deaths);
        if (m->shots_fired) chart_fmt_pct(v[6], 48, stats_match_accuracy(m), 1);
        else dg_strcpy(v[6], 48, "-");
        chart_fmt_duration(v[7], 48, m->duration_sec);
        float cx = a.x;
        for (int c = 0; c < 8; c++) {
            float w = a.w * cw[c];
            u32 col = c == 2 ? (m->result == MATCH_WIN ? TH->ok : m->result == MATCH_LOSS ? TH->danger : TH->text_faint) : c == 0 ? TH->text : TH->text_dim;
            if (c < 3) text_w(cx + gui_s(8), y + (rh - gd_line_height(fs)) * 0.5f, fs, c == 2 ? GW_SEMIBOLD : GW_REGULAR, col, GUI_LEFT, w - gui_s(10), v[c]);
            else text(cx + w - gui_s(8), y + (rh - gd_line_height(fs)) * 0.5f, fs, GW_REGULAR, col, GUI_RIGHT, v[c]);
            cx += w;
        }
        y += rh;
    }
    return y - a.y;
}

// ------------------------------------------------------------------ activity

static i32 g_cal_first;
static float g_cal[371];

static const char *month_label(int i, char *b, int n, void *user) {
    (void)user;
    int y, m, d;
    stats_day_ymd(g_cal_first + i, &y, &m, &d);
    if (d <= 7 && stats_weekday(g_cal_first + i) == 0) snprintf(b, (size_t)n, "%s", k_months[(m + 11) % 12]);
    else b[0] = 0;
    return b;
}

static float page_activity(GuiRect a, const ChartStyle *st) {
    const PlayerStats *ps = PS();
    float y = a.y, gap = gui_s(8), bh = gui_s(TH->button_h);
    y += empty_note(a, y);
    static const char *const modes[2] = { "Matches played", "Kills" };
    int nmode = gw_tabs(gui_id("st_act"), gui_rect(a.x, y, a.w * 0.5f, bh), modes, 2, T.act_mode);
    if (nmode != T.act_mode) T.act_mode = nmode;
    y += bh + gap;
    i32 today = stats_day_of(stats_now());
    int days = 365;
    g_cal_first = today - days + 1;
    int active = 0, best_i = -1, streak = 0;
    double total = 0;
    for (int i = 0; i < days; i++) {
        const DailyRecord *d = stats_day_find(ps, g_cal_first + i);
        g_cal[i] = d ? (float)(T.act_mode ? d->kills : d->matches) : 0.0f;
        if (d && d->matches) active++;
        total += g_cal[i];
        if (g_cal[i] > 0 && (best_i < 0 || g_cal[i] > g_cal[best_i])) best_i = i;
    }
    for (int i = days - 1; i >= 0; i--) {
        if (g_cal[i] > 0) streak++;
        else if (i != days - 1) break;   // today may still be empty
    }
    ChartCalendarSpec cs;
    memset(&cs, 0, sizeof(cs));
    cs.days = days;
    cs.values = g_cal;
    cs.last_weekday = stats_weekday(today);
    cs.month_label = month_label;
    cs.color = TH->accent;
    float chh = fminf(gui_s(210), (a.w - gui_s(40)) / 53.0f * 7.0f + gui_s(60));
    GuiRect cr = box(gui_rect(a.x, y, a.w, chh), T.act_mode ? "Kills per day, last year" : "Matches per day, last year");
    int hv = chart_calendar_heatmap(&T.cal, mixk(base_key(), (u32)(T.act_mode * 977 + today)), cr, st, &cs);
    if (hv >= 0) {
        char t[96], d[48];
        snprintf(t, sizeof(t), "%s\n%.0f %s", fday_full(d, sizeof(d), g_cal_first + hv), g_cal[hv], T.act_mode ? (g_cal[hv] == 1 ? "kill" : "kills") : (g_cal[hv] == 1 ? "match" : "matches"));
        set_tip(t);
    }
    y += chh + gap;
    char v[4][48];
    chart_fmt_int(v[0], 48, active);
    chart_fmt_int(v[1], 48, total);
    if (best_i >= 0) {
        char d[32];
        snprintf(v[2], 48, "%s (%.0f)", fday(d, sizeof(d), g_cal_first + best_i), g_cal[best_i]);
    } else dg_strcpy(v[2], 48, "-");
    snprintf(v[3], 48, "%d day%s", streak, streak == 1 ? "" : "s");
    const char *titles[4] = { "Active days", T.act_mode ? "Kills this year" : "Matches this year", "Best day", "Current streak" };
    float cw = (a.w - gap * 3) / 4.0f, ch = gui_s(74);
    for (int i = 0; i < 4; i++) {
        GuiRect r = { a.x + (cw + gap) * (float)i, y, cw, ch };
        card_bg(r, false);
        caps(r.x + gui_s(12), r.y + gui_s(10), gui_s(TH->t_tiny + 1), TH->text_faint, titles[i]);
        text_w(r.x + gui_s(12), r.y + gui_s(30), gui_s(TH->t_title - 4), GW_SEMIBOLD, TH->text, GUI_LEFT, r.w - gui_s(24), v[i]);
    }
    y += ch;
    return y - a.y;
}

// ------------------------------------------------------------------ the tab

static void init_state(void) {
    T.inited = true;
    T.wsel = T.msel = -1;
    T.scroll_req = -1.0f;
    T.sort_col = WC_KILLS;
    T.range = RG_25;
    T.ma = true;
}

void stats_tab_open(void) {
    if (!T.inited) init_state();
    T.gen++;   // every chart rebuilds (theme, size, a changed profile)
    T.rev = PS()->revision;
    T.rev_time = gui_time();
    T.player = 0;
    T.compare = false;
    gui_anim_restart(gui_id("st_open"));
}

void stats_tab_confirm_reset(bool yes) {
    if (!yes) return;
    game_stats_reset_all();
    T.wsel = T.msel = -1;
    T.gen++;
    dg_strcpy(T.status, sizeof(T.status), "Stats reset");
    T.status_time = gui_time();
}

static void toolbar(GuiRect r, const StatsPlayerRow *rows, int np) {
    float gap = gui_s(8), x = r.x;
    if (np > 1) {
        static char names[MAX_PLAYERS][40];
        const char *items[MAX_PLAYERS];
        for (int i = 0; i < np; i++) {
            if (rows[i].local) snprintf(names[i], sizeof(names[i]), "You (%s)", rows[i].name);
            else dg_strcpy(names[i], sizeof(names[i]), rows[i].name);
            items[i] = names[i];
        }
        if (gw_dropdown(gui_id("st_player"), gui_rect(x, r.y, gui_s(220), r.h), items, np, &T.player)) T.gen++;
        x += gui_s(220) + gap;
        if (!rows[T.player].local) {
            GuiRect tr = { x, r.y, gui_s(TH->button_h * 2.2f), r.h };
            gw_toggle(gui_id("st_compare"), tr, &T.compare);
            text(tr.x + tr.w + gui_s(8), r.y + (r.h - gd_line_height(gui_s(TH->t_small))) * 0.5f, gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT, "Compare with you");
        }
    }
    float bw = gui_s(128), xr = r.x + r.w;
    xr -= bw;
    if (gw_button(gui_id("st_reset"), gui_rect(xr, r.y, bw, r.h), "Reset stats", GB_DANGER, true))
        screens_confirm(CONFIRM_STATS_RESET, "Reset your stats?", "Every lifetime number, weapon total and match record of this profile is erased. This cannot be undone.", 0);
    xr -= bw + gap;
    if (gw_button(gui_id("st_export"), gui_rect(xr, r.y, bw, r.h), "Export JSON", GB_SECONDARY, true)) {
        char path[600];
        bool ok = game_stats_export(path, sizeof(path));
        snprintf(T.status, sizeof(T.status), ok ? "Exported: %s" : "Export failed: %s", path);
        T.status_time = gui_time();
    }
    xr -= gui_s(118) + gap;
    int units = imperial() ? 1 : 0;
    if (gw_dropdown(gui_id("st_units"), gui_rect(xr, r.y, gui_s(118), r.h), k_units, 2, &units)) game_stats_set_imperial(units == 1);
    if (T.status[0] && gui_time() - T.status_time < 6.0) {
        float sx = x + gui_s(8), sw = xr - gap - sx;
        if (sw > gui_s(60)) text_w(sx, r.y + (r.h - gd_line_height(gui_s(TH->t_tiny))) * 0.5f, gui_s(TH->t_tiny), GW_REGULAR, TH->text_dim, GUI_LEFT, sw, T.status);
    }
}

bool stats_tab_draw(GuiRect r) {
    if (!T.inited) stats_tab_open();
    // The revision the charts show follows the live one at most once a second (a match running
    // under an open tab does not rebuild every shot).
    if (PS()->revision != T.rev && gui_time() - T.rev_time >= 1.0) { T.rev = PS()->revision; T.rev_time = gui_time(); }
    T.tip[0] = 0;
    ChartStyle st = chart_style_default();
    StatsPlayerRow rows[MAX_PLAYERS];
    int np = game_stats_players(rows, MAX_PLAYERS);
    if (T.player >= np) T.player = 0;
    bool remote = np > 0 && !rows[T.player].local;
    float gap = gui_s(8), bh = gui_s(TH->button_h);
    GuiRect tabs = { r.x, r.y, r.w, bh };
    if (!remote) {
        int np2 = gw_tabs(gui_id("st_tabs"), tabs, k_pages, SP_COUNT, T.page);
        if (gui_nav(NAV_TAB_NEXT)) { np2 = (T.page + 1) % SP_COUNT; gui_nav_consume(NAV_TAB_NEXT); gui_sound(TH->snd_tab); }
        if (gui_nav(NAV_TAB_PREV)) { np2 = (T.page + SP_COUNT - 1) % SP_COUNT; gui_nav_consume(NAV_TAB_PREV); gui_sound(TH->snd_tab); }
        if (np2 != T.page) { T.page = np2; gui_set_focus(0); }
    } else {
        char b[80];
        snprintf(b, sizeof(b), "%s's stats", rows[T.player].name);
        caps(r.x, r.y + (bh - gd_line_height(gui_s(TH->t_small + 1))) * 0.5f, gui_s(TH->t_small + 1), TH->text, b);
        gd_rect(gui_rect(r.x, r.y + bh - 1, r.w, 1), TH->panel_border);
    }
    GuiRect tool = { r.x, r.y + bh + gap, r.w, bh };
    toolbar(tool, rows, np);
    float top = tool.y + bh + gap;
    GuiRect view = { r.x, top, r.w - gui_s(12), r.y + r.h - top };
    int si = remote ? SP_COUNT : T.page;
    GuiScroll *sc = &T.scroll[si];
    gw_scroll_begin(sc, gui_idi("st_scroll", si), view);
    float tin = gui_ease_out(gui_anim_t(gui_id("st_open")) / fmaxf(0.01f, TH->modal_in));
    GuiRect area = { view.x, gw_scroll_y(sc) + (1.0f - tin) * gui_s(8), view.w - gui_s(6), 0 };
    float h = 0;
    if (remote) {
        const StatsPublic *them = game_stats_public(rows[T.player].slot);
        if (T.compare) {
            const StatsPublic *me = game_stats_public(rows[0].slot);
            StatsPublic mine = me ? *me : (StatsPublic){ 0 };
            h = page_compare(area, &st, &mine, them, rows[T.player].name);
        } else h = page_public(area, them, rows[T.player].name);
    } else {
        switch (T.page) {
        case SP_OVERVIEW: h = page_overview(area, &st); break;
        case SP_PERFORMANCE: h = page_performance(area, &st); break;
        case SP_WEAPONS: h = page_weapons(area, &st); break;
        case SP_MOVEMENT: h = page_movement(area, &st); break;
        case SP_HISTORY: h = page_history(area, &st); break;
        case SP_ACTIVITY: h = page_activity(area, &st); break;
        default: break;
        }
    }
    gw_scroll_end(sc, h + gui_s(16));
    // A requested scroll (a picked weapon's detail, stats_tab): applied once the page's height is
    // known (gw_scroll_begin clamps the target to the last frame's height).
    if (T.scroll_req_frames > 0) {
        sc->target = T.scroll_req == -2.0f ? T.detail_y : T.scroll_req;
        if (--T.scroll_req_frames == 0) T.scroll_req = -1.0f;
    }
    if (T.tip[0]) chart_tooltip(T.tip);
    return false;
}

bool stats_tab_draw_modal(GuiRect r) {
    char sub[96];
    const PlayerStats *ps = PS();
    char b[32];
    snprintf(sub, sizeof(sub), "%s matches · %s", chart_fmt_int(b, sizeof(b), ps->matches_played), ps->profile[0] && strcmp(ps->profile, "local") ? "Steam profile" : "local profile");
    GuiRect c = screens_modal_frame(r, "Stats", sub);
    return stats_tab_draw(c);
}

// ------------------------------------------------------------------ commands

// stats_tab <page> [arg]: switch the sub-page (scripted screenshots); weapons <id> selects a
// weapon, history <n> opens the n-th newest match, performance <metric> <range> <view>.
static void cmd_stats_tab(int argc, const char **argv) {
    if (!T.inited) init_state();
    if (argc < 2) { log_info("stats_tab <overview|performance|weapons|movement|history|activity> [arg]  (page %s)", k_page_ids[T.page]); return; }
    for (int i = 0; i < SP_COUNT; i++) if (!strcmp(argv[1], k_page_ids[i])) T.page = i;
    if (T.page == SP_WEAPONS && argc > 2) pick_weapon(stats_weapon_find(PS(), argv[2]));
    if (T.page == SP_HISTORY && argc > 2) T.msel = atoi(argv[2]) > 0 ? nmatches() - atoi(argv[2]) : -1;
    if (T.page == SP_PERFORMANCE && argc > 2) T.metric = DG_CLAMP(atoi(argv[2]), 0, MET_COUNT - 1);
    if (T.page == SP_PERFORMANCE && argc > 3) T.range = DG_CLAMP(atoi(argv[3]), 0, RG_COUNT - 1);
    if (T.page == SP_PERFORMANCE && argc > 4) T.view = DG_CLAMP(atoi(argv[4]), 0, 1);
    if (T.page == SP_ACTIVITY && argc > 2) T.act_mode = !strcmp(argv[2], "kills");
    if (T.page != SP_PERFORMANCE && T.page != SP_WEAPONS) {
        T.scroll_req = argc > 3 ? (float)atof(argv[3]) : 0.0f;
        T.scroll_req_frames = 3;
    }
}

void stats_tab_register(void) {
    cmd_register("stats_tab", cmd_stats_tab, "stats_tab <overview|performance|weapons|movement|history|activity> [arg]: the Stats tab's page (weapons <id>, history <n newest>, performance <metric> <range> <view>, activity kills)");
}
