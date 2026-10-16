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

static void complete(void) {
    const char *matches[64];
    // Complete the first token only.
    if (strchr(C->input, ' ')) return;
    int n = cmd_complete(C->input, matches, 64);
    if (n == 0) { C->completion_hint[0] = 0; return; }
    if (n == 1) {
        char buf[INPUT_MAX];
        snprintf(buf, sizeof(buf), "%s ", matches[0]);
        set_input(buf);
        C->completion_hint[0] = 0;
        return;
    }
    // Longest common prefix
    size_t lcp = strlen(matches[0]);
    for (int i = 1; i < n; i++) {
        size_t k = 0;
        while (k < lcp && matches[i][k] && SDL_tolower(matches[i][k]) == SDL_tolower(matches[0][k])) k++;
        lcp = k;
    }
    char buf[INPUT_MAX];
    snprintf(buf, sizeof(buf), "%.*s", (int)lcp, matches[0]);
    if (strlen(buf) > strlen(C->input)) set_input(buf);
    C->completion_hint[0] = 0;
    for (int i = 0; i < n && i < 24; i++) {
        strncat(C->completion_hint, matches[i], sizeof(C->completion_hint) - strlen(C->completion_hint) - 2);
        strncat(C->completion_hint, "  ", sizeof(C->completion_hint) - strlen(C->completion_hint) - 1);
    }
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
    log_info("] %s", C->input);
    dg_strcpy(C->history[C->history_count % HISTORY], INPUT_MAX, C->input);
    C->history_count++;
    C->history_pos = C->history_count;
    cbuf_add(C->input);
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
