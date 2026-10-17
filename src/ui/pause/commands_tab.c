// Pause menu > Commands (src/ui/pause/commands_tab.h).
#include "commands_tab.h"
#include "ui/screens/screens.h"
#include "ui/console.h"
#include "ui/menus.h"
#include "core/cmd_registry.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH (gui_theme())
#define MAX_ENTRIES 1400
#define RECENT_MAX 16

typedef struct Entry {
    const char *name;
    const char *desc;
    Cvar *cv;           // NULL = a command
    int cat;            // CmdRegistryCfg category, -1 none
} Entry;

// Category rows: the three lists first, then the file's categories.
enum { CAT_FAVORITES = -3, CAT_RECENT = -2, CAT_ALL = -1 };

static struct {
    CmdRegistryCfg cfg;
    bool cfg_loaded;
    Entry e[MAX_ENTRIES];
    int count;
    int shown[MAX_ENTRIES];
    int shown_count;
    int cat;                 // CAT_* or a category index
    char search[64];
    CmdList fav, recent;
    GuiScroll scroll, cat_scroll, log_scroll;
    char input[256];
    char hint[256];
    int hist_pos;            // console history browsing (count = the new line)
    char pending[256];       // destructive command waiting for the confirm dialog
    u32 seen_log;
    Cvar *favorites, *recent_cv, *enable;
} CT;

static void save_lists(void) {
    char b[CVAR_STR_MAX];
    cmdlist_format(&CT.fav, b, sizeof(b));
    cvar_set_cv(CT.favorites, b);
    cmdlist_format(&CT.recent, b, sizeof(b));
    cvar_set_cv(CT.recent_cv, b);
}

static void load_cfg(void) {
    CT.cfg_loaded = true;
    char *json = platform_read_file("assets/ui/command_categories.json", NULL);
    char err[128];
    memset(&CT.cfg, 0, sizeof(CT.cfg));
    if (!json || !cmdreg_parse(&CT.cfg, json, err, sizeof(err))) log_warn("command_categories.json: %s", json ? err : "missing");
    free(json);
}

static int cmp_entry(const void *a, const void *b) { return SDL_strcasecmp(((const Entry *)a)->name, ((const Entry *)b)->name); }

static void build(void) {
    if (!CT.cfg_loaded) load_cfg();
    CT.count = 0;
    for (int i = 0; i < cmd_count() && CT.count < MAX_ENTRIES; i++) {
        const char *n = cmd_name_at(i);
        if (n[0] == '+' || n[0] == '-') continue;   // +action / -action pairs: the bindings screen
        Entry *e = &CT.e[CT.count++];
        e->name = n;
        e->desc = cmd_desc_at(i);
        e->cv = NULL;
        e->cat = cmdreg_category(&CT.cfg, n);
    }
    for (int i = 0; i < cvar_count() && CT.count < MAX_ENTRIES; i++) {
        Cvar *cv = cvar_at(i);
        Entry *e = &CT.e[CT.count++];
        e->name = cv->name;
        e->desc = cv->desc;
        e->cv = cv;
        e->cat = cmdreg_category(&CT.cfg, cv->name);
    }
    qsort(CT.e, (size_t)CT.count, sizeof(Entry), cmp_entry);
}

static const Entry *find(const char *name) {
    for (int i = 0; i < CT.count; i++) if (!strcmp(CT.e[i].name, name)) return &CT.e[i];
    return NULL;
}

static void filter(void) {
    CT.shown_count = 0;
    if (CT.cat == CAT_RECENT || CT.cat == CAT_FAVORITES) {
        const CmdList *l = CT.cat == CAT_RECENT ? &CT.recent : &CT.fav;
        for (int k = 0; k < l->count; k++) {
            const Entry *e = find(l->items[k]);
            if (e && cmdreg_match(CT.search, e->name, e->desc)) CT.shown[CT.shown_count++] = (int)(e - CT.e);
        }
        return;
    }
    for (int i = 0; i < CT.count; i++) {
        const Entry *e = &CT.e[i];
        if (CT.cat >= 0 && e->cat != CT.cat) continue;
        if (!cmdreg_match(CT.search, e->name, e->desc)) continue;
        CT.shown[CT.shown_count++] = i;
    }
}

bool commands_tab_available(void) {
    Cvar *dev = cvar_find("developer");
    return (dev && dev->i) || (CT.enable && CT.enable->i);
}

void commands_tab_open(void) {
    build();
    // "*" = never edited: the defaults from command_categories.json.
    if (!strcmp(CT.favorites->str, "*")) CT.fav = CT.cfg.favorites;
    else cmdlist_parse(&CT.fav, CT.favorites->str);
    cmdlist_parse(&CT.recent, CT.recent_cv->str);
    CT.hist_pos = console_history_count();
    filter();
}

void commands_tab_set_filter(const char *search, const char *category) {
    if (!CT.count) commands_tab_open();
    dg_strcpy(CT.search, sizeof(CT.search), search ? search : "");
    if (category) {
        if (!strcmp(category, "all")) CT.cat = CAT_ALL;
        else if (!strcmp(category, "favorites")) CT.cat = CAT_FAVORITES;
        else if (!strcmp(category, "recent")) CT.cat = CAT_RECENT;
        else { int k = cmdreg_category_by_id(&CT.cfg, category); CT.cat = k >= 0 ? k : CAT_ALL; }
    }
    filter();
}

int commands_tab_visible_count(void) { return CT.shown_count; }

// Runs a line (from the list or the embedded console): destructive commands ask first.
static void run_line(const char *line, bool ask) {
    char word[CMDREG_NAME];
    int n = 0;
    while (line[n] && line[n] != ' ' && n < CMDREG_NAME - 1) { word[n] = line[n]; n++; }
    word[n] = 0;
    if (ask && cmdreg_destructive(&CT.cfg, word)) {
        dg_strcpy(CT.pending, sizeof(CT.pending), line);
        char text[320];
        const char *d = cmd_desc(word);
        snprintf(text, sizeof(text), "%s%s%s", line, d ? ": " : "", d ? d : "");
        screens_confirm(CONFIRM_COMMAND, "Run this command?", text, 0);
        return;
    }
    console_submit(line);
    if (find(word)) {
        cmdlist_touch(&CT.recent, word, RECENT_MAX);
        save_lists();
    }
    CT.hist_pos = console_history_count();
}

void commands_tab_confirm_result(bool yes) {
    if (yes && CT.pending[0]) run_line(CT.pending, false);
    CT.pending[0] = 0;
}

static void copy_entry(const Entry *e) {
    char b[CVAR_STR_MAX + CVAR_NAME_MAX + 8];
    if (e->cv) snprintf(b, sizeof(b), "%s \"%s\"", e->name, e->cv->str);
    else snprintf(b, sizeof(b), "%s", e->name);
    SDL_SetClipboardText(b);
    log_info("copied: %s", b);
}

// ------------------------------------------------------------------ drawing

static float draw_categories(GuiRect r) {
    float rh = gui_s(30);
    int rows = 3 + CT.cfg.cat_count;
    gw_scroll_begin(&CT.cat_scroll, gui_id("cmd_cats"), r);
    float y = gw_scroll_y(&CT.cat_scroll);
    for (int k = 0; k < rows; k++) {
        int cat = k < 3 ? (k == 0 ? CAT_FAVORITES : k == 1 ? CAT_RECENT : CAT_ALL) : k - 3;
        const char *label = k == 0 ? "Favorites" : k == 1 ? "Recent" : k == 2 ? "All" : CT.cfg.cats[cat].label;
        int count = 0;
        if (cat == CAT_FAVORITES) count = CT.fav.count;
        else if (cat == CAT_RECENT) count = CT.recent.count;
        else for (int i = 0; i < CT.count; i++) count += cat == CAT_ALL || CT.e[i].cat == cat;
        char b[64];
        snprintf(b, sizeof(b), "%s  %d", label, count);
        GuiRect rr = { r.x, y, r.w, rh - gui_s(2) };
        if (gw_menu_item(gui_idi("cmd_cat", k), rr, b, CT.cat == cat, 1.0f, true) && CT.cat != cat) {
            CT.cat = cat;
            CT.scroll.target = 0;
            filter();
        }
        y += rh;
        if (k == 2) y += gui_s(8);   // the lists, then the categories
    }
    gw_scroll_end(&CT.cat_scroll, rh * (float)rows + gui_s(8));
    return 0;
}

static void draw_value(const Entry *e, GuiRect r, int row) {
    Cvar *cv = e->cv;
    GuiId id = gui_idi("cmd_val", row);
    if (cv->flags & CVAR_READONLY) {
        gw_label(r, cv->str, gui_s(TH->t_small), GW_MEDIUM, TH->text_dim, GUI_RIGHT);
        return;
    }
    if (cv->type == CVAR_BOOL) {
        bool v = cv->i != 0;
        GuiRect tr = { r.x + r.w - gui_s(96), r.y + (r.h - gui_s(22)) * 0.5f, gui_s(46), gui_s(22) };   // its On / Off label to the right
        if (gw_toggle(id, tr, &v)) cvar_set_int(cv, v ? 1 : 0);
    } else if ((cv->type == CVAR_FLOAT || cv->type == CVAR_INT) && cv->has_range && cv->max - cv->min <= 100000.0f) {
        float v = cv->f;
        float step = cv->type == CVAR_INT ? 1.0f : (cv->max - cv->min) / 200.0f;
        if (gw_slider(id, gui_rect(r.x, r.y + (r.h - gui_s(22)) * 0.5f, r.w, gui_s(22)), &v, cv->min, cv->max, step, cv->type == CVAR_INT ? "%.0f" : "%.3g")) {
            if (cv->type == CVAR_INT) cvar_set_int(cv, (int)lrintf(v));
            else cvar_set_float(cv, v);
        }
    } else {
        // Strings and unbounded numbers: a text field, applied on Enter.
        static char edit[CVAR_STR_MAX];
        static GuiId edit_id;
        if (!gw_textfield_editing(id) && edit_id == id) edit_id = 0;
        if (edit_id != id && gw_textfield_editing(id)) { edit_id = id; dg_strcpy(edit, sizeof(edit), cv->str); }
        if (edit_id == id) {
            if (gw_textfield(id, gui_rect(r.x, r.y + gui_s(4), r.w, r.h - gui_s(8)), edit, sizeof(edit), NULL)) cvar_set_cv(cv, edit);
        } else {
            char shown[CVAR_STR_MAX];
            dg_strcpy(shown, sizeof(shown), cv->str);
            gw_textfield(id, gui_rect(r.x, r.y + gui_s(4), r.w, r.h - gui_s(8)), shown, sizeof(shown), "(empty)");
            if (gw_textfield_editing(id)) { edit_id = id; dg_strcpy(edit, sizeof(edit), cv->str); }
        }
    }
}

static void draw_list(GuiRect r) {
    float rh = gui_s(gui_tok("cmd_row_h"));
    gw_scroll_begin(&CT.scroll, gui_id("cmd_list"), r);
    float y0 = gw_scroll_y(&CT.scroll);
    if (!CT.shown_count) {
        const char *msg = CT.cat == CAT_FAVORITES ? "No favorites yet: star a command or setting." : CT.cat == CAT_RECENT ? "Nothing run from here yet." : "Nothing matches the search.";
        gw_label(gui_rect(r.x, r.y + gui_s(10), r.w, gui_s(24)), msg, gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    }
    for (int k = 0; k < CT.shown_count; k++) {
        float y = y0 + (float)k * rh;
        if (y + rh < r.y || y > r.y + r.h) continue;   // only the visible rows
        const Entry *e = &CT.e[CT.shown[k]];
        GuiRect row = { r.x, y, r.w - gui_s(10), rh - gui_s(2) };
        GuiInteract in = gui_interact(gui_idi("cmd_row", k), row, 0);
        if (k & 1) gd_rect(row, UI_RGBA(255, 255, 255, 6));
        if (in.hot) gd_rect(row, UI_RGBA(255, 255, 255, 12));
        float x = row.x + gui_s(6);
        // Favorite star.
        bool fav = cmdlist_has(&CT.fav, e->name);
        GuiRect sr = { x, row.y + (row.h - gui_s(22)) * 0.5f, gui_s(22), gui_s(22) };
        GuiInteract si = gui_interact(gui_idi("cmd_star", k), sr, 0);
        {
            float cx = sr.x + sr.w * 0.5f, cy = sr.y + sr.h * 0.5f, ro = sr.w * 0.42f, ri = ro * 0.45f;
            u32 c = fav ? TH->accent : (si.hot ? TH->text_dim : TH->text_faint);
            float px = 0, py = 0;
            for (int p = 0; p <= 10; p++) {
                float a = (float)p * DG_PI / 5.0f, rr = (p & 1) ? ri : ro;
                float qx = cx + sinf(a) * rr, qy = cy - cosf(a) * rr;
                if (p) gd_line(px, py, qx, qy, fmaxf(1.0f, gui_s(1.4f)), c);
                px = qx;
                py = qy;
            }
            if (fav) gd_circle(cx, cy, ri * 0.8f, c);
        }
        if (si.clicked) {
            cmdlist_toggle(&CT.fav, e->name);
            save_lists();
            if (CT.cat == CAT_FAVORITES) filter();
        }
        x += gui_s(30);
        // Name, kind, description.
        GuiText nt = { gui_s(TH->t_body - 1), GW_SEMIBOLD, TH->text, GUI_LEFT, 0, false, false, gui_s(230) };
        gd_text(&nt, x, row.y + gui_s(3), e->name);
        bool modified = e->cv && strcmp(e->cv->str, e->cv->default_str);
        const char *kind = e->cv ? (e->cv->flags & CVAR_CHEAT ? "cheat" : e->cv->flags & CVAR_SERVER ? "server" : "setting") : "command";
        if (!e->cv && cmdreg_destructive(&CT.cfg, e->name)) kind = "asks first";
        GuiText kt = { gui_s(TH->t_tiny), GW_SEMIBOLD, modified ? TH->accent : TH->text_faint, GUI_LEFT, TH->tracking_caps, true, false, 0 };
        gd_text(&kt, x, row.y + rh * 0.5f + gui_s(1), modified ? "modified" : kind);
        float ctrl_w = gui_s(220), btn_w = gui_s(62);
        float dx = x + gui_s(240);
        float dw = row.x + row.w - ctrl_w - btn_w * 2 - gui_s(24) - dx;
        GuiText dt = { gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, dw };
        if (dw > gui_s(60)) gd_text(&dt, dx, row.y + (row.h - gd_line_height(dt.size)) * 0.5f, e->desc);
        // Value editor / Run, then Reset / Copy.
        GuiRect cr = { row.x + row.w - ctrl_w - btn_w * 2 - gui_s(16), row.y, ctrl_w, row.h };
        GuiRect b1 = { cr.x + ctrl_w + gui_s(8), row.y + (row.h - gui_s(24)) * 0.5f, btn_w, gui_s(24) };
        GuiRect b2 = { b1.x + btn_w + gui_s(6), b1.y, btn_w, b1.h };
        if (e->cv) {
            draw_value(e, cr, k);
            if (gw_button(gui_idi("cmd_reset", k), b1, "Reset", GB_GHOST, modified)) {
                cvar_reset(e->cv);
                log_info("%s reset to %s", e->name, e->cv->default_str);
            }
        } else {
            GuiRect rr = { cr.x + cr.w - gui_s(110), b1.y, gui_s(110), b1.h };
            if (gw_button(gui_idi("cmd_run", k), rr, "Run", cmdreg_destructive(&CT.cfg, e->name) ? GB_DANGER : GB_SECONDARY, true)) run_line(e->name, true);
            if (gw_button(gui_idi("cmd_edit", k), b1, "Edit", GB_GHOST, true)) {
                // Into the console line for arguments.
                snprintf(CT.input, sizeof(CT.input), "%s ", e->name);
                gw_textfield_activate(gui_id("cmd_input"));
            }
        }
        if (gw_button(gui_idi("cmd_copy", k), b2, "Copy", GB_GHOST, true)) copy_entry(e);
        if (in.hot) gui_tooltip(gui_idi("cmd_row", k), e->desc);
    }
    gw_scroll_end(&CT.scroll, rh * (float)CT.shown_count);
}

static u32 log_color(const LogLine *l) {
    if (l->level == LOG_WARN) return TH->warning;
    if (l->level >= LOG_ERROR) return TH->danger;
    if (l->level == LOG_DEBUG) return TH->text_faint;
    if (l->text[0] == ']') return TH->info;
    return TH->text_dim;
}

static void draw_console(GuiRect r) {
    gd_round(r, gui_s(TH->radius), UI_RGBA(0, 0, 0, 110));
    float pad = gui_s(10);
    float ih = gui_s(30), hh = gui_s(20);
    GuiRect logr = { r.x + pad, r.y + pad, r.w - pad * 2, r.h - ih - hh - pad * 2 - gui_s(4) };
    // Log (coloured), newest at the bottom.
    float lh = gd_line_height(gui_s(TH->t_small)) + gui_s(1);
    int n = log_line_count();
    gw_scroll_begin(&CT.log_scroll, gui_id("cmd_log"), logr);
    if (CT.seen_log != log_revision()) { CT.log_scroll.target = 1e9f; CT.seen_log = log_revision(); }
    float y0 = gw_scroll_y(&CT.log_scroll);
    for (int i = 0; i < n; i++) {
        float y = y0 + (float)i * lh;
        if (y + lh < logr.y || y > logr.y + logr.h) continue;
        const LogLine *l = log_get_line(i);
        GuiText t = { gui_s(TH->t_small), l->text[0] == ']' ? GW_SEMIBOLD : GW_REGULAR, log_color(l), GUI_LEFT, 0, false, false, logr.w };
        gd_text(&t, logr.x, y, l->text);
    }
    gw_scroll_end(&CT.log_scroll, (float)n * lh);
    // Hint line: the command's description / the cvar's value, or the completions.
    GuiRect hr = { r.x + pad, logr.y + logr.h + gui_s(2), r.w - pad * 2, hh };
    char hint[256];
    console_hint(CT.input, hint, sizeof(hint));
    gw_label(hr, CT.hint[0] ? CT.hint : hint, gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT);
    // Input: Enter runs, Tab completes, Up / Down walk the shared history.
    GuiId iid = gui_id("cmd_input");
    GuiRect ir = { r.x + pad, r.y + r.h - pad - ih, r.w - pad * 2 - gui_s(110), ih };
    bool editing = gw_textfield_editing(iid);
    if (editing && gui_key_pressed_raw(SDL_SCANCODE_TAB)) {
        console_complete(CT.input, sizeof(CT.input), CT.hint, sizeof(CT.hint));
    }
    if (editing && gui_key_pressed_raw(SDL_SCANCODE_UP) && console_history_count()) {
        CT.hist_pos = DG_MAX(0, CT.hist_pos - 1);
        dg_strcpy(CT.input, sizeof(CT.input), console_history_at(CT.hist_pos));
    }
    if (editing && gui_key_pressed_raw(SDL_SCANCODE_DOWN) && console_history_count()) {
        CT.hist_pos = DG_MIN(console_history_count(), CT.hist_pos + 1);
        dg_strcpy(CT.input, sizeof(CT.input), CT.hist_pos < console_history_count() ? console_history_at(CT.hist_pos) : "");
    }
    if (gw_textfield(iid, ir, CT.input, sizeof(CT.input), "Type a command or a setting (Tab completes, Up / Down history)")) {
        if (CT.input[0]) run_line(CT.input, true);
        CT.input[0] = 0;
        CT.hint[0] = 0;
        gw_textfield_activate(iid);   // keep typing
    }
    if (!CT.input[0]) CT.hint[0] = 0;
    // Pop out: the full ~ console over the game.
    GuiRect pr = { ir.x + ir.w + gui_s(10), ir.y, gui_s(100), ih };
    if (gw_button(gui_id("cmd_popout"), pr, "Pop out", GB_SECONDARY, true)) {
        menus_close_all();
        console_set_open(true);
    }
}

bool commands_tab_draw(GuiRect r) {
    if (!CT.count) commands_tab_open();
    char sub[64];
    snprintf(sub, sizeof(sub), "%d of %d", CT.shown_count, CT.count);
    GuiRect c = screens_modal_frame(r, "Commands", sub);
    float gap = gui_s(TH->gap * 1.5f);
    // Search.
    GuiRect sr = { c.x, c.y, fminf(gui_s(gui_tok("cmd_search_w")), c.w * 0.5f), gui_s(TH->button_h + 2) };
    char before[64];
    dg_strcpy(before, sizeof(before), CT.search);
    gw_textfield(gui_id("cmd_search"), sr, CT.search, sizeof(CT.search), "Search commands and settings");
    if (strcmp(before, CT.search)) { CT.scroll.target = 0; filter(); }
    screens_footer_hint(gui_rect(c.x, sr.y + gui_s(6), c.w, gui_s(20)), "Star to favorite  \xC2\xB7  Esc back");
    float top = sr.y + sr.h + gap;
    float con_h = fminf(gui_s(gui_tok("cmd_console_h")), (c.y + c.h - top) * 0.45f);
    GuiRect body = { c.x, top, c.w, c.y + c.h - top - con_h - gap };
    float cw = gui_s(190);
    draw_categories(gui_rect(body.x, body.y, cw, body.h));
    draw_list(gui_rect(body.x + cw + gap, body.y, body.w - cw - gap, body.h));
    draw_console(gui_rect(c.x, c.y + c.h - con_h, c.w, con_h));
    return false;
}

static void cmd_commands(int argc, const char **argv) {
    commands_tab_set_filter(argc > 2 ? argv[2] : "", argc > 1 ? argv[1] : "all");
    log_info("commands: %d shown", commands_tab_visible_count());
}

void commands_tab_register(void) {
    CT.favorites = cvar_reg_string("con_favorites", "*", CVAR_ARCHIVE, "Commands tab: favorite commands and settings (space separated; * = the defaults)");
    CT.recent_cv = cvar_reg_string("con_recent", "", CVAR_ARCHIVE, "Commands tab: recently run commands (newest first)");
    CT.enable = cvar_reg_bool("con_enable", false, CVAR_ARCHIVE, "the pause menu's Commands tab without developer mode");
    cmd_register("commands_filter", cmd_commands, "Commands tab: commands_filter <all|favorites|recent|category> [search]: filter the list (tests)");
}
