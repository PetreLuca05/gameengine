#include "console.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "nuklear_config.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>

#define HISTORY 32
#define INPUT_MAX 256

typedef struct Console {
    bool open;
    char input[INPUT_MAX];
    int input_len;
    char history[HISTORY][INPUT_MAX];
    int history_count;
    int history_pos;
    u32 seen_revision;
    bool scroll_to_bottom;
    char completion_hint[512];
} Console;

#define C (g_engine.console)

static void cmd_toggleconsole(int argc, const char **argv) { console_toggle(); }

void console_init(void) {
    C = (Console *)calloc(1, sizeof(Console));
    cmd_register("toggleconsole", cmd_toggleconsole, "toggle the console overlay");
}

void console_shutdown(void) {
    free(C);
    C = NULL;
}

bool console_is_open(void) { return C && C->open; }

void console_set_open(bool open) {
    if (!C) return;
    C->open = open;
    C->scroll_to_bottom = true;
    ui_set_text_input(open);
}

void console_toggle(void) { console_set_open(!console_is_open()); }

static void set_input(const char *s) {
    dg_strcpy(C->input, sizeof(C->input), s);
    C->input_len = (int)strlen(C->input);
}

int console_complete(char *buf, int size, char *hint, int hint_size) {
    const char *matches[64];
    if (hint && hint_size) hint[0] = 0;
    // Complete the first token only.
    if (strchr(buf, ' ')) return 0;
    int n = cmd_complete(buf, matches, 64);
    if (n == 0) return 0;
    if (n == 1) {
        snprintf(buf, (size_t)size, "%s ", matches[0]);
        return 1;
    }
    // Longest common prefix
    size_t lcp = strlen(matches[0]);
    for (int i = 1; i < n; i++) {
        size_t k = 0;
        while (k < lcp && matches[i][k] && SDL_tolower(matches[i][k]) == SDL_tolower(matches[0][k])) k++;
        lcp = k;
    }
    if (lcp > strlen(buf)) snprintf(buf, (size_t)size, "%.*s", (int)lcp, matches[0]);
    for (int i = 0; hint && i < n && i < 24; i++) {
        strncat(hint, matches[i], (size_t)hint_size - strlen(hint) - 2);
        strncat(hint, "  ", (size_t)hint_size - strlen(hint) - 1);
    }
    return n;
}

static void complete(void) {
    console_complete(C->input, INPUT_MAX, C->completion_hint, sizeof(C->completion_hint));
    C->input_len = (int)strlen(C->input);
}

void console_hint(const char *input, char *out, int size) {
    out[0] = 0;
    char word[CVAR_NAME_MAX];
    int n = 0;
    while (input[n] == ' ') input++;
    while (input[n] && input[n] != ' ' && n < CVAR_NAME_MAX - 1) { word[n] = input[n]; n++; }
    word[n] = 0;
    if (!n) return;
    const Cvar *cv = cvar_find(word);
    if (cv) {
        if (cv->has_range) snprintf(out, (size_t)size, "%s = %s (default %s, %g..%g)  %s", cv->name, cv->str, cv->default_str, cv->min, cv->max, cv->desc);
        else snprintf(out, (size_t)size, "%s = \"%s\" (default \"%s\")  %s", cv->name, cv->str, cv->default_str, cv->desc);
        return;
    }
    const char *d = cmd_desc(word);
    if (d) snprintf(out, (size_t)size, "%s: %s", word, d);
}

int console_history_count(void) { return C ? DG_MIN(C->history_count, HISTORY) : 0; }
const char *console_history_at(int i) {
    if (!C || i < 0 || i >= console_history_count()) return "";
    int first = C->history_count - console_history_count();
    return C->history[(first + i) % HISTORY];
}

void console_submit(const char *line) {
    if (!C || !line || !line[0]) return;
    log_info("] %s", line);
    dg_strcpy(C->history[C->history_count % HISTORY], INPUT_MAX, line);
    C->history_count++;
    C->history_pos = C->history_count;
    cbuf_add(line);
    C->scroll_to_bottom = true;
}

bool console_handle_event(const SDL_Event *ev) {
    if (!console_is_open() || ev->type != SDL_EVENT_KEY_DOWN) return false;
    switch (ev->key.key) {
    case SDLK_TAB: complete(); return true;
    case SDLK_UP:
        if (C->history_count) {
            C->history_pos = DG_MAX(0, C->history_pos - 1);
            set_input(C->history[C->history_pos % HISTORY]);
        }
        return true;
    case SDLK_DOWN:
        if (C->history_count) {
            C->history_pos = DG_MIN(C->history_count, C->history_pos + 1);
            set_input(C->history_pos < C->history_count ? C->history[C->history_pos % HISTORY] : "");
        }
        return true;
    default: return false;
    }
}

static void submit(void) {
    C->input[C->input_len] = 0;
    if (!C->input[0]) return;
    console_submit(C->input);
    C->input[0] = 0;
    C->input_len = 0;
    C->completion_hint[0] = 0;
    C->scroll_to_bottom = true;
}

void console_draw(void) {
    if (!console_is_open()) return;
    struct nk_context *ctx = ui_nk();
    float w = ui_width(), h = ui_height() * 0.45f;
    float s = ui_scale();
    if (nk_begin(ctx, "Console", nk_rect(0, 0, w, h), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        float line_h = font_line_height(FONT_UI) + 2 * s;
        float log_h = h - line_h * (C->completion_hint[0] ? 3.4f : 2.4f) - 20 * s;
        nk_layout_row_dynamic(ctx, log_h, 1);
        if (nk_group_begin(ctx, "log", NK_WINDOW_BORDER)) {
            int count = log_line_count();
            for (int i = 0; i < count; i++) {
                const LogLine *l = log_get_line(i);
                struct nk_color col = nk_rgb(200, 200, 200);
                if (l->level == LOG_WARN) col = nk_rgb(240, 200, 80);
                else if (l->level >= LOG_ERROR) col = nk_rgb(255, 90, 80);
                else if (l->level == LOG_DEBUG) col = nk_rgb(140, 140, 150);
                else if (l->text[0] == ']') col = nk_rgb(150, 220, 255);
                nk_layout_row_dynamic(ctx, line_h, 1);
                nk_label_colored(ctx, l->text, NK_TEXT_LEFT, col);
            }
            if (C->scroll_to_bottom || C->seen_revision != log_revision()) {
                nk_group_set_scroll(ctx, "log", 0, (nk_uint)(count * (line_h + ctx->style.window.spacing.y) + 1000));
                C->seen_revision = log_revision();
                C->scroll_to_bottom = false;
            }
            nk_group_end(ctx);
        }
        if (C->completion_hint[0]) {
            nk_layout_row_dynamic(ctx, line_h, 1);
            nk_label_colored(ctx, C->completion_hint, NK_TEXT_LEFT, nk_rgb(150, 150, 160));
        }
        nk_layout_row_dynamic(ctx, line_h + 6 * s, 1);
        nk_edit_focus(ctx, NK_EDIT_FIELD);
        nk_flags r = nk_edit_string(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, C->input, &C->input_len, INPUT_MAX - 1, nk_filter_default);
        if (r & NK_EDIT_COMMITED) submit();
    }
    nk_end(ctx);
}
