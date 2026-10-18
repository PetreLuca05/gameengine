// Story mode HUD (docs/STORY.md): the objective panel top left (caps label, the active required leaves,
// a check mark on completion), onboarding prompts under it, subtitles above the hotbar, the document /
// message panel, world markers, the journal, the chapter-end card, the fade, and the story_debug
// overlay. Everything reads StoryUi (src/game/game_story.c), filled from EV_STORY on every machine.
#include "hud_internal.h"
#include "game/game_story.h"
#include "input/input.h"
#include "render/render.h"
#include <math.h>
#include <stdio.h>

static const char *text_of(const StoryUi *u, const char *id) {
    const StoryEntry *e = u->script ? story_script_find(u->script, id) : NULL;
    return e ? e->body : id;
}

static const char *title_of(const StoryUi *u, const char *id) {
    const StoryEntry *e = u->script ? story_script_find(u->script, id) : NULL;
    return e && e->title[0] ? e->title : NULL;
}

static void key_of(const char *action, char *out, int size) {
    static const struct { const char *name; Action a; } map[] = {
        {"interact", ACT_USE}, {"phone", ACT_PHONE}, {"inventory", ACT_INVENTORY}, {"heal_wheel", ACT_HEAL_WHEEL},
        {"attack", ACT_ATTACK}, {"sprint", ACT_SPRINT}, {"crouch", ACT_CROUCH}, {"hotbar", ACT_SLOT1}, {"throw_wheel", ACT_HEAL_WHEEL},
    };
    if (!strcmp(action, "move")) {
        char f[16], l[16], b[16], r[16];
        binding_name(input_get_binding(ACT_FORWARD, 0), f, sizeof(f));
        binding_name(input_get_binding(ACT_LEFT, 0), l, sizeof(l));
        binding_name(input_get_binding(ACT_BACK, 0), b, sizeof(b));
        binding_name(input_get_binding(ACT_RIGHT, 0), r, sizeof(r));
        snprintf(out, (size_t)size, "%s%s%s%s", f, l, b, r);
        return;
    }
    if (!strcmp(action, "look")) { snprintf(out, (size_t)size, "Mouse"); return; }
    for (int i = 0; i < ARRAY_COUNT(map); i++) {
        if (strcmp(map[i].name, action)) continue;
        binding_name(input_get_binding(map[i].a, 0), out, (size_t)size);
        return;
    }
    snprintf(out, (size_t)size, "%s", action);
}

// "{move} Move" -> key chips + text. Draws at (x, y); returns the width.
static float draw_prompt_text(float x, float y, const char *s, float alpha) {
    GuiText t = { gui_s(TH->t_body), GW_MEDIUM, TH->hud_text, GUI_LEFT, 0, false, true, 0 };
    GuiText kt = { gui_s(TH->t_small), GW_SEMIBOLD, TH->text_dark, GUI_CENTER, 0, false, false, 0 };
    float ks = gui_s(26), x0 = x;
    gd_alpha_push(alpha);
    while (*s) {
        if (*s == '{') {
            const char *e = strchr(s, '}');
            if (e) {
                char action[32], key[32];
                snprintf(action, sizeof(action), "%.*s", (int)(e - s - 1), s + 1);
                key_of(action, key, sizeof(key));
                float kw = fmaxf(ks, gd_text_width(&kt, key) + gui_s(12));
                GuiRect kr = { x, y, kw, ks };
                gd_round(kr, gui_s(4), UI_RGBA(236, 236, 230, 235));
                gd_text(&kt, kr.x + kr.w * 0.5f, kr.y + (ks - gd_line_height(kt.size)) * 0.5f, key);
                x += kw + gui_s(8);
                s = e + 1;
                while (*s == ' ') s++;
                continue;
            }
        }
        const char *e = strchr(s, '{');
        char run[160];
        snprintf(run, sizeof(run), "%.*s", e ? (int)(e - s) : (int)strlen(s), s);
        x += gd_text(&t, x, y + (ks - gd_line_height(t.size)) * 0.5f, run) + gui_s(8);
        s = e ? e : s + strlen(s);
    }
    gd_alpha_pop();
    return x - x0;
}

static void draw_objectives(const StoryUi *u, GuiRect safe, float *bottom) {
    const StoryDef *d = u->def;
    int idx[8];
    int n = story_active_leaves(u->mirror, d, idx, 8);
    float x = safe.x + gui_s(10), y = safe.y + gui_s(12);
    bool flash = u->flash_obj != STORY_NONE && u->time - u->flash_time < 3.0f;
    if (!n && !flash) { *bottom = y; return; }
    hud_caps(x, y, gui_s(TH->t_label), TH->accent, GUI_LEFT, "Objective");
    y += gui_s(22);
    GuiText t = { gui_s(TH->t_body), GW_MEDIUM, TH->hud_text, GUI_LEFT, 0, false, true, gui_s(520) };
    if (flash) {
        float a = 1.0f - saturate((u->time - u->flash_time - 2.2f) / 0.8f);
        GuiText ft = t;
        ft.color = gui_rgba_mul(TH->ok, a);
        float cy = y + gui_s(11);   // a drawn check mark (the HUD font has no glyph for it)
        gd_line(x, cy, x + gui_s(4), cy + gui_s(4), gui_s(2), ft.color);
        gd_line(x + gui_s(4), cy + gui_s(4), x + gui_s(11), cy - gui_s(5), gui_s(2), ft.color);
        gd_text(&ft, x + gui_s(18), y, text_of(u, d->obj[u->flash_obj].text));
        y += gui_s(26);
    }
    for (int i = 0; i < n; i++) {
        const StoryObjective *o = &d->obj[idx[i]];
        char line[200];
        const char *txt = text_of(u, o->text[0] ? o->text : o->id);
        // A need-N group of options: show the progress ("2 / 3").
        if (o->has_children && o->need > 0) {
            int done = 0;
            for (int c = 0; c < d->obj_n; c++) done += d->obj[c].parent == idx[i] && u->mirror->obj[c].state == SOS_DONE;
            snprintf(line, sizeof(line), "%s  %d / %d", txt, done, o->need);
        } else {
            snprintf(line, sizeof(line), "%s", txt);
        }
        gd_rect(gui_rect(x, y + gui_s(4), gui_s(2), gui_s(16)), TH->accent);
        gd_text(&t, x + gui_s(10), y, line);
        y += gui_s(26);
    }
    *bottom = y;
}

static void draw_prompt(const StoryUi *u, float x, float y) {
    if (u->prompt == STORY_NONE) return;
    const StoryPromptDef *p = &u->def->prompt[u->prompt];
    float a = saturate((u->time - u->prompt_time) / 0.3f);
    gd_alpha_push(a * 0.9f);
    GuiRect r = { x + gui_s(4), y + gui_s(8), gui_s(360), gui_s(40) };
    gd_round(r, gui_s(TH->radius), TH->hud_panel);
    gd_alpha_pop();
    draw_prompt_text(r.x + gui_s(10), r.y + gui_s(7), text_of(u, p->text), a);
}

static void draw_subtitles(const StoryUi *u, GuiRect safe) {
    float y = safe.y + safe.h - gui_s(TH->slot * 0.82f) - gui_s(120);
    for (int i = 0; i < u->sub_n; i++) {
        const StoryUiSub *s = &u->subs[i];
        if (u->time < s->start) continue;
        float a = saturate((u->time - s->start) / 0.15f) * saturate((s->start + s->dur - u->time) / 0.3f);
        GuiText t = { gui_s(TH->t_body + 2), GW_MEDIUM, gui_rgba_mul(UI_RGBA(250, 248, 240, 255), a), GUI_CENTER, 0, false, true, 0 };
        char line[300];
        if (s->speaker[0]) snprintf(line, sizeof(line), "%s:  %s", s->speaker, s->text);
        else snprintf(line, sizeof(line), "%s", s->text);
        float w = fminf(gd_text_width(&t, line) + gui_s(28), gui_w() * 0.7f);
        gd_alpha_push(a * 0.55f);
        gd_round(gui_rect(gui_w() * 0.5f - w * 0.5f, y - gui_s(6), w, gui_s(34)), gui_s(6), UI_RGBA(0, 0, 0, 255));
        gd_alpha_pop();
        gd_text(&t, gui_w() * 0.5f, y, line);
        y -= gui_s(40);
    }
}

static void draw_document(const StoryUi *u) {
    if (!u->doc[0]) return;
    float a = saturate((u->time - u->doc_time) / 0.2f);
    const char *title = title_of(u, u->doc);
    const char *body = text_of(u, u->doc);
    float w = gui_s(560), cx = gui_w() * 0.5f;
    GuiText bt = { gui_s(TH->t_body + 1), GW_REGULAR, UI_RGBA(40, 36, 30, 255), GUI_LEFT, 0, false, false, 0 };
    float bh = gd_text_wrap(&bt, 0, 0, w - gui_s(56), body, false);
    float h = bh + gui_s(title ? 100 : 70);
    GuiRect r = { cx - w * 0.5f, gui_h() * 0.42f - h * 0.5f, w, h };
    gd_alpha_push(a);
    gd_shadow(r, gui_s(4), gui_s(30), UI_RGBA(0, 0, 0, 90));
    gd_round(r, gui_s(4), UI_RGBA(244, 238, 222, 245));
    float y = r.y + gui_s(24);
    if (title) {
        hud_caps(r.x + gui_s(28), y, gui_s(TH->t_label), UI_RGBA(120, 90, 50, 255), GUI_LEFT, title);
        y += gui_s(30);
    }
    gd_text_wrap(&bt, r.x + gui_s(28), y, w - gui_s(56), body, true);
    char k[24], hint[64];
    binding_name(input_get_binding(ACT_USE, 0), k, sizeof(k));
    snprintf(hint, sizeof(hint), "%s  Close", k);
    hud_caps(r.x + r.w - gui_s(20), r.y + r.h - gui_s(24), gui_s(TH->t_tiny), UI_RGBA(120, 110, 95, 255), GUI_RIGHT, hint);
    gd_alpha_pop();
}

static void draw_markers(const StoryUi *u) {
    for (int i = 0; i < STORY_UI_MARKERS; i++) {
        const StoryUiMarker *m = &u->markers[i];
        if (!m->on) continue;
        vec2 sp;
        if (!render_world_to_screen(m->pos, &sp)) continue;
        float s = gui_s(7);
        gd_line(sp.x - s, sp.y, sp.x, sp.y - s, gui_s(2), TH->accent);
        gd_line(sp.x, sp.y - s, sp.x + s, sp.y, gui_s(2), TH->accent);
        gd_line(sp.x + s, sp.y, sp.x, sp.y + s, gui_s(2), TH->accent);
        gd_line(sp.x, sp.y + s, sp.x - s, sp.y, gui_s(2), TH->accent);
    }
}

static void draw_journal(const StoryUi *u) {
    if (!u->journal_open) return;
    const StoryDef *d = u->def;
    float w = gui_s(640), h = gui_s(520);
    GuiRect r = { gui_w() * 0.5f - w * 0.5f, gui_h() * 0.5f - h * 0.5f, w, h };
    gd_backdrop(r, UI_RGBA(20, 22, 26, 210));
    gd_round_outline(r, gui_s(TH->radius_panel), gui_s(1), TH->panel_border);
    hud_caps(r.x + gui_s(24), r.y + gui_s(20), gui_s(TH->t_label), TH->accent, GUI_LEFT, "Journal");
    static const char *kinds[] = { "Document", "Photo", "Voicemail", "Message", "Item" };
    float y = r.y + gui_s(54);
    int found = 0;
    for (int c = 0; c < d->collect_n; c++) {
        if (!u->mirror->found[c]) continue;
        found++;
        const char *t = title_of(u, d->collect[c].text);
        char line[160];
        snprintf(line, sizeof(line), "%s", t ? t : d->collect[c].id);
        hud_caps(r.x + gui_s(24), y + gui_s(3), gui_s(TH->t_tiny), TH->text_dim, GUI_LEFT, kinds[d->collect[c].kind % 5]);
        gd_text_simple(r.x + gui_s(130), y, gui_s(TH->t_body), GW_MEDIUM, TH->hud_text, GUI_LEFT, line);
        y += gui_s(28);
        if (y > r.y + r.h - gui_s(40)) break;
    }
    char foot[64];
    snprintf(foot, sizeof(foot), "%d / %d found", found, d->collect_n);
    hud_caps(r.x + r.w - gui_s(24), r.y + r.h - gui_s(28), gui_s(TH->t_tiny), TH->text_dim, GUI_RIGHT, foot);
}

static void draw_debug(void) {
    char buf[4096];
    story_debug_text(buf, sizeof(buf));
    float y = gui_s(140);
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        gd_text_simple(gui_w() - gui_s(16), y, gui_s(TH->t_tiny + 1), GW_REGULAR, UI_RGBA(230, 240, 200, 230), GUI_RIGHT, line);
        y += gui_s(15);
        line = nl ? nl + 1 : NULL;
    }
}

void hud_story_draw(GuiRect safe) {
    const StoryUi *u = story_ui();
    if (u->debug) draw_debug();
    if (!u->active || !u->def) return;
    float bottom;
    draw_objectives(u, safe, &bottom);
    draw_prompt(u, safe.x, bottom);
    draw_markers(u);
    draw_subtitles(u, safe);
    draw_document(u);
    draw_journal(u);
    if (u->ended) {
        float a = saturate((u->time - u->end_time) / 1.0f);
        GuiText t = { gui_s(TH->t_huge), GW_SEMIBOLD, gui_rgba_mul(TH->hud_text, a), GUI_CENTER, 0.1f, true, true, 0 };
        gd_text(&t, gui_w() * 0.5f, gui_h() * 0.4f, u->def->title);
        hud_caps(gui_w() * 0.5f, gui_h() * 0.4f + gui_s(64), gui_s(TH->t_label), gui_rgba_mul(TH->accent, a), GUI_CENTER, "Chapter complete");
    }
}

// The title card ("Day 3" + one caps line; the end card's type), over the fade: it reads on black
// while the screen fades in from a night.
static void draw_title(const StoryUi *u) {
    if (!u->title[0]) return;
    float t = u->time - u->title_time;
    if (t < 0 || t > u->title_len) return;
    float a = saturate(t / 0.6f) * saturate((u->title_len - t) / 0.8f);
    const char *title = title_of(u, u->title);
    const char *line = text_of(u, u->title);
    GuiText tt = { gui_s(TH->t_huge), GW_SEMIBOLD, gui_rgba_mul(TH->hud_text, a), GUI_CENTER, 0.1f, true, true, 0 };
    gd_text(&tt, gui_w() * 0.5f, gui_h() * 0.38f, title ? title : line);
    if (title && line[0]) hud_caps(gui_w() * 0.5f, gui_h() * 0.38f + gui_s(64), gui_s(TH->t_label), gui_rgba_mul(TH->accent, a), GUI_CENTER, line);
}

// The fade covers the whole HUD (drawn last), the title card on top.
void hud_story_draw_fade(void) {
    const StoryUi *u = story_ui();
    if (!u->active) return;
    if (u->fade >= 0.002f) gd_rect(gui_rect(0, 0, gui_w(), gui_h()), UI_RGBA(0, 0, 0, (int)(255.0f * saturate(u->fade))));
    draw_title(u);
}
