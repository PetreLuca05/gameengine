#include "anim_debug.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "game/game.h"
#include "net/net.h"
#include "gameplay/characters.h"
#include "gameplay/health.h"
#include "gameplay/items.h"
#include "anim/anim_library.h"
#include "anim/body_parts.h"
#include "render/debug_draw.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ state

static struct {
    Cvar *debug;
    EntityId locked;          // anim_target: inspected character (ENT_NONE = under the crosshair)
    // test stand
    EntityId stand;
    bool stand_playing;
    float stand_rate;
} S = { 0, ENT_NONE, ENT_NONE, true, 1.0f };

#define PANEL_BG UI_RGBA(8, 10, 12, 200)
#define TXT UI_RGBA(225, 228, 220, 255)
#define DIM UI_RGBA(150, 155, 150, 255)
#define HOT UI_RGBA(255, 210, 90, 255)
#define BAD UI_RGBA(255, 110, 90, 255)

static float scale_px(void) { return fmaxf(1.0f, ui_height() / 1080.0f); }

static float line(float x, float y, u32 col, const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_text(x, y, FONT_SMALL, col, UI_ALIGN_LEFT, buf);
    return y + 15.0f * scale_px();
}

static EntityId self_ent(void) {
    Player *p = game_local_player();
    return p ? p->ent : ENT_NONE;
}

static EntityId under_crosshair(HitboxRayHit *hit) {
    World *w = game_world();
    Game *g = game();
    if (!w || !g) return ENT_NONE;
    EntityId e = ENT_NONE;
    HitboxRayHit h;
    vec3 dir = dir_from_angles(g->view.yaw, g->view.pitch);
    if (!chars_raycast(w, self_ent(), g->view.eye, dir, 150.0f, 0, &e, &h)) return ENT_NONE;
    if (hit) *hit = h;
    return e;
}

static CharacterInst *target(void) {
    World *w = game_world();
    if (!w) return NULL;
    if (S.locked != ENT_NONE) {
        CharacterInst *c = char_get(w, S.locked);
        if (c) return c;
        S.locked = ENT_NONE;
    }
    EntityId e = under_crosshair(NULL);
    return e != ENT_NONE ? char_get(w, e) : NULL;
}

static CharacterInst *stand(void) {
    World *w = game_world();
    if (!w || S.stand == ENT_NONE) return NULL;
    CharacterInst *c = char_get(w, S.stand);
    if (!c) S.stand = ENT_NONE;
    return c;
}

static int layer_arg(const ControllerInst *ct, const char *s) {
    for (int l = 0; l < ct->def->layer_count; l++) if (!strcmp(ct->def->layers[l].name, s)) return l;
    if (s[0] >= '0' && s[0] <= '9') return atoi(s) < ct->def->layer_count ? atoi(s) : -1;
    return -1;
}

// ------------------------------------------------------------------ controller commands

static void cmd_anim_target(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (S.locked != ENT_NONE) { S.locked = ENT_NONE; log_info("anim_target: following the crosshair"); return; }
    S.locked = under_crosshair(NULL);
    log_info(S.locked != ENT_NONE ? "anim_target: locked ent %u" : "anim_target: no character under the crosshair", S.locked);
}

static void cmd_anim_solo(int argc, const char **argv) {
    CharacterInst *c = target();
    if (!c) { log_info("anim_solo: no character (aim at one or anim_target)"); return; }
    ControllerInst *ct = &c->rt.ctrl;
    if (argc < 2 || !strcmp(argv[1], "off")) { memset(ct->solo, 0, sizeof(ct->solo)); log_info("anim_solo: off"); return; }
    int l = layer_arg(ct, argv[1]);
    if (l < 0) { log_info("anim_solo: unknown layer '%s'", argv[1]); return; }
    ct->solo[l] = !ct->solo[l];
    log_info("anim_solo %s: %s", ct->def->layers[l].name, ct->solo[l] ? "on" : "off");
}

static void cmd_anim_mute(int argc, const char **argv) {
    CharacterInst *c = target();
    if (!c) { log_info("anim_mute: no character (aim at one or anim_target)"); return; }
    ControllerInst *ct = &c->rt.ctrl;
    if (argc < 2 || !strcmp(argv[1], "off")) { memset(ct->mute, 0, sizeof(ct->mute)); log_info("anim_mute: off"); return; }
    int l = layer_arg(ct, argv[1]);
    if (l < 0) { log_info("anim_mute: unknown layer '%s'", argv[1]); return; }
    ct->mute[l] = !ct->mute[l];
    log_info("anim_mute %s: %s", ct->def->layers[l].name, ct->mute[l] ? "on" : "off");
}

static void cmd_anim_force(int argc, const char **argv) {
    CharacterInst *c = target();
    if (!c || argc < 3) { log_info("usage: anim_force <layer> <state> (character under the crosshair)"); return; }
    ControllerInst *ct = &c->rt.ctrl;
    int l = layer_arg(ct, argv[1]);
    int st = l >= 0 ? ctrl_state_index(ct->def, l, argv[2]) : -1;
    if (st < 0) { log_info("anim_force: unknown layer/state %s/%s", argv[1], argv[2]); return; }
    ctrl_force_state(ct, l, st, 0, 0);
    log_info("anim_force: %s -> %s (the host / next resync may override it)", ct->def->layers[l].name, argv[2]);
}

static void cmd_anim_clips(int argc, const char **argv) {
    const AnimLibrary *L = g_engine.anim;
    if (!L) return;
    const char *filter = argc > 1 ? argv[1] : NULL;
    for (int i = 0; i < L->clip_count; i++) {
        const Clip *k = &L->clips[i];
        if (filter && !strstr(k->name, filter)) continue;
        log_info("  %3d %-40s %5.2fs %s %08x", i, k->name, k->duration, k->loop ? "loop" : "once", k->hash);
    }
}

// ------------------------------------------------------------------ test stand

static void cmd_anim_stand(int argc, const char **argv) {
    World *w = game_world();
    if (!w) return;
    if (argc > 1 && !strcmp(argv[1], "off")) {
        CharacterInst *c = stand();
        if (c) c->rt.override_clip = -1;
        S.stand = ENT_NONE;
        log_info("anim_stand: off");
        return;
    }
    EntityId e = under_crosshair(NULL);
    if (e == ENT_NONE) {
        // Fall back to the nearest character dummy.
        float best = 1e9f;
        Game *g = game();
        ENT_FOREACH(w, i) {
            if (w->type[i] != ET_DUMMY || !char_get_index(w, i)) continue;
            float d = v3_dist(w->xf[i].pos, g->view.eye);
            if (d < best) { best = d; e = ENT_MAKE(i, w->gen[i]); }
        }
    }
    if (e == ENT_NONE) { log_info("anim_stand: no character found"); return; }
    S.stand = e;
    S.stand_playing = true;
    S.stand_rate = 1.0f;
    log_info("anim_stand: ent %u. anim_stand_clip <name|index|none>, anim_stand_scrub <0..1>, anim_stand_play, anim_stand_rate <x>,", e);
    log_info("  anim_stand_mirror, anim_stand_item [next|prev|id], anim_stand_char <id>, anim_stand_hit <part|all>, anim_stand_drop, anim_stand_push [N]");
}

static CharacterInst *need_stand(const char *what) {
    CharacterInst *c = stand();
    if (!c) log_info("%s: no test stand (run anim_stand while aiming at a character)", what);
    return c;
}

static void cmd_stand_clip(int argc, const char **argv) {
    CharacterInst *c = need_stand("anim_stand_clip");
    const AnimLibrary *L = g_engine.anim;
    if (!c || !L) return;
    if (argc < 2 || !strcmp(argv[1], "none")) { c->rt.override_clip = -1; log_info("anim_stand_clip: controller"); return; }
    int k = anim_lib_find_clip(L, argv[1]);
    if (k < 0 && argv[1][0] >= '0' && argv[1][0] <= '9') k = atoi(argv[1]);
    if (k < 0 || k >= L->clip_count) {
        // Partial match.
        for (int i = 0; i < L->clip_count && k < 0; i++) if (strstr(L->clips[i].name, argv[1])) k = i;
    }
    if (k < 0 || k >= L->clip_count) { log_info("anim_stand_clip: no clip '%s' (anim_clips lists them)", argv[1]); return; }
    c->rt.override_clip = (i16)k;
    c->rt.override_time = 0;
    S.stand_playing = true;
    log_info("anim_stand_clip: %s (%.2fs)", L->clips[k].name, L->clips[k].duration);
}

static void cmd_stand_scrub(int argc, const char **argv) {
    CharacterInst *c = need_stand("anim_stand_scrub");
    const AnimLibrary *L = g_engine.anim;
    if (!c || !L || c->rt.override_clip < 0 || argc < 2) { log_info("usage: anim_stand_scrub <0..1> (after anim_stand_clip)"); return; }
    const Clip *k = &L->clips[c->rt.override_clip];
    c->rt.override_time = saturate((float)atof(argv[1])) * k->duration;
    S.stand_playing = false;
}

static void cmd_stand_play(int argc, const char **argv) {
    (void)argc; (void)argv;
    S.stand_playing = !S.stand_playing;
    log_info("anim_stand_play: %s", S.stand_playing ? "playing" : "paused");
}

static void cmd_stand_rate(int argc, const char **argv) {
    if (argc > 1) S.stand_rate = (float)atof(argv[1]);
    log_info("anim_stand_rate: %.2f", S.stand_rate);
}

static void cmd_stand_mirror(int argc, const char **argv) {
    (void)argc; (void)argv;
    CharacterInst *c = need_stand("anim_stand_mirror");
    if (c) c->rt.override_mirror = !c->rt.override_mirror;
}

static void cmd_stand_item(int argc, const char **argv) {
    CharacterInst *c = need_stand("anim_stand_item");
    World *w = game_world();
    if (!c || !w || !w->items) return;
    int i = (int)ENT_INDEX(c->ent);
    int n = w->items->item_count;
    int cur = w->item[i];
    int next = cur;
    if (argc < 2 || !strcmp(argv[1], "next")) next = (cur + 1) % n;
    else if (!strcmp(argv[1], "prev")) next = (cur - 1 + n) % n;
    else next = item_find(w->items, argv[1]);
    w->item[i] = (u16)next;
    const ItemDef *d = item_def(w->items, (u16)next);
    const WeaponDef *wd = item_weapon(w->items, (u16)next);
    log_info("anim_stand_item: %s (hold style %s)", d ? d->id : "none", wd ? wd->hold_style : (d ? d->hold_style : "-"));
}

static void cmd_stand_char(int argc, const char **argv) {
    CharacterInst *c = need_stand("anim_stand_char");
    World *w = game_world();
    if (!c || !w || argc < 2 || !g_engine.anim) { log_info("usage: anim_stand_char <character id> (char_list / assets/characters)"); return; }
    const CharacterDef *d = anim_lib_char_by_id(g_engine.anim, argv[1]);
    if (!d) { log_info("anim_stand_char: unknown character '%s'", argv[1]); return; }
    EntityId e = c->ent;
    char_detach(w, e);
    w->char_id[ENT_INDEX(e)] = d->net_id;
    char_attach(w, e, d->net_id);
    log_info("anim_stand_char: %s", d->id);
}

static void cmd_stand_hit(int argc, const char **argv) {
    CharacterInst *c = need_stand("anim_stand_hit");
    World *w = game_world();
    Game *g = game();
    if (!c || !w) return;
    if (!w->authority) { log_info("anim_stand_hit: host only"); return; }
    int first = 0, last = BP_COUNT - 1;
    if (argc > 1 && strcmp(argv[1], "all")) {
        int p = body_part_from_name(argv[1]);
        if (p < 0) { log_info("anim_stand_hit: unknown part '%s' (head, chest, upper_arm_l, ...)", argv[1]); return; }
        first = last = p;
    }
    float dmg = argc > 2 ? (float)atof(argv[2]) : 1.0f;
    for (int p = first; p <= last; p++) {
        vec3 point = c->root.t;
        for (int h = 0; h < c->hit.count; h++)
            if (c->hit.c[h].part == p) point = v3_scale(v3_add(c->hit.c[h].a, c->hit.c[h].b), 0.5f);
        vec3 dir = v3_norm(v3_sub(point, g->view.eye));
        apply_damage_part(w, c->ent, dmg, DMG_BULLET, self_ent(), point, dir, p, 1.0f);
    }
}

static void cmd_stand_drop(int argc, const char **argv) {
    (void)argc; (void)argv;
    CharacterInst *c = need_stand("anim_stand_drop");
    World *w = game_world();
    if (!c || !w) return;
    if (chars_ragdoll_release(w, c->ent)) { log_info("anim_stand_drop: standing up"); return; }
    chars_ragdoll_now(w, c->ent, v3(0, 0, 0));
    log_info("anim_stand_drop: ragdoll (again to stand up)");
}

static void cmd_stand_push(int argc, const char **argv) {
    CharacterInst *c = need_stand("anim_stand_push");
    World *w = game_world();
    Game *g = game();
    if (!c || !w) return;
    float n = argc > 1 ? (float)atof(argv[1]) : 120.0f;
    vec3 dir = dir_from_angles(g->view.yaw, g->view.pitch);
    chars_ragdoll_now(w, c->ent, v3_scale(dir, n));
}

// aimat [part]: aims the view at a body part of the character nearest the crosshair, as this
// machine renders it (net / lag-compensation tests: clients aim at the interpolated pose).
static void cmd_aimat(int argc, const char **argv) {
    World *w = game_world();
    Game *g = game();
    if (!w || !w->chars || !g) return;
    int part = argc > 1 ? body_part_from_name(argv[1]) : BP_CHEST;
    if (part < 0) { log_info("aimat: unknown part '%s'", argv[1]); return; }
    vec3 fwd = dir_from_angles(g->view.yaw, g->view.pitch);
    // Nearest character within 15 deg of the view; otherwise the best aligned one.
    CharacterInst *best = NULL, *aligned = NULL;
    float best_dist = 1e9f, best_dot = -2.0f;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c || c->local || c->dead || !c->hit.count || !ent_valid(w, c->ent) || c->ent == self_ent()) continue;
        vec3 to = v3_sub(v3_add(c->root.t, v3(0, 1.2f, 0)), g->view.eye);
        float dist = v3_len(to), dot = v3_dot(v3_scale(to, 1.0f / fmaxf(dist, 1e-3f)), fwd);
        if (dot > cosf(15.0f * DG_DEG2RAD) && dist < best_dist) { best_dist = dist; best = c; }
        if (dot > best_dot) { best_dot = dot; aligned = c; }
    }
    if (!best) best = aligned;
    if (!best) { log_info("aimat: no character"); return; }
    vec3 target = best->root.t;
    for (int h = 0; h < best->hit.count; h++)
        if (best->hit.c[h].part == part) target = v3_scale(v3_add(best->hit.c[h].a, best->hit.c[h].b), 0.5f);
    vec3 d = v3_sub(target, g->view.eye);
    g->view.yaw = atan2f(-d.x, -d.z);
    g->view.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
    log_info("aimat: ent %u %s at %.2f %.2f %.2f", best->ent, body_part_name(part), target.x, target.y, target.z);
}

// ------------------------------------------------------------------ panels

static const char *param_label(const ControllerDef *d, int p) {
    if (p < AP_STD_COUNT) return ctrl_param_name(p);
    return p < d->param_count ? d->params[p].name : "?";
}

static void draw_controller_panel(CharacterInst *c) {
    World *w = game_world();
    const AnimLibrary *L = g_engine.anim;
    const ControllerInst *ct = &c->rt.ctrl;
    const ControllerDef *d = ct->def;
    float s = scale_px();
    float x = ui_width() - 430 * s, y = 70 * s, pw = 420 * s;
    float y0 = y;
    ui_rect(x - 8 * s, y - 8 * s, pw, 640 * s, PANEL_BG);
    int i = (int)ENT_INDEX(c->ent);
    y = line(x, y, HOT, "ANIM  ent %u  %s  [%s]%s%s", c->ent, c->def->id, ent_type_name((EntityType)w->type[i]),
             S.locked == c->ent ? "  LOCKED" : "", S.stand == c->ent ? "  STAND" : "");
    y = line(x, y, DIM, "set %s  lod %d (%.0f px, 1/%d)%s%s", ct->bind ? ct->bind->name : "-", c->lod, c->pixels, c->interval,
             c->culled ? " culled" : "", c->dead ? "  DEAD" : "");
    if (c->rt.override_clip >= 0 && L)
        y = line(x, y, HOT, "stand clip %s  t %.2f / %.2f%s%s", L->clips[c->rt.override_clip].name, c->rt.override_time,
                 L->clips[c->rt.override_clip].duration, c->rt.override_mirror ? " mirror" : "", S.stand_playing ? "" : " (paused)");
    y += 4 * s;
    for (int l = 0; l < d->layer_count; l++) {
        const LayerState *ls = &ct->layer[l];
        bool blending = ls->blend_total > 0 && ls->blend_ticks < ls->blend_total;
        u32 col = ct->mute[l] ? DIM : (ct->solo[l] ? HOT : TXT);
        char blend[64] = "";
        if (blending)
            snprintf(blend, sizeof(blend), "  <- %s %.0f%%", ctrl_state_name(ct, l, ls->prev_state), 100.0f * (float)ls->blend_ticks / (float)ls->blend_total);
        y = line(x, y, col, "%-6s %-14s ph %.2f t %4.2fs%s%s%s", d->layers[l].name, ctrl_state_name(ct, l, ls->state), ls->phase,
                 ctrl_state_time(ct, l), blend, ct->mute[l] ? " MUTE" : "", ct->solo[l] ? " SOLO" : "");
    }
    float hw = 0;
    bool hik = ctrl_hand_ik_weight(ct, &hw);
    y = line(x, y, DIM, "hand ik %s %.2f   inertialization %s", hik ? "on" : "off", hw, ct->inert_pending ? "pending" : "-");
    y += 4 * s;
    // Parameters (non-default only, two columns).
    y = line(x, y, HOT, "params");
    int col = 0;
    float yc = y;
    for (int p = 0; p < d->param_count && p < AC_MAX_PARAMS; p++) {
        float v = ct->params[p];
        if (fabsf(v - d->params[p].def) < 1e-4f) continue;
        char t[64];
        snprintf(t, sizeof(t), "%-14s %7.3f", param_label(d, p), v);
        ui_text(x + (float)col * 205 * s, yc, FONT_SMALL, TXT, UI_ALIGN_LEFT, t);
        col ^= 1;
        if (!col) yc += 15 * s;
    }
    y = yc + (col ? 15 * s : 0) + 4 * s;
    // Transition history (newest first).
    y = line(x, y, HOT, "transitions");
    for (int k = 0; k < 8; k++) {
        const TransitionLog *t = &ct->history[(ct->history_head - 1 - k + AC_HISTORY * 2) % AC_HISTORY];
        if (!t->tick && t->from == t->to) break;   // unused slot
        if (t->layer >= d->layer_count) continue;
        y = line(x, y, TXT, "  %6u %-6s %s -> %s", t->tick, d->layers[t->layer].name, ctrl_state_name(ct, t->layer, t->from), ctrl_state_name(ct, t->layer, t->to));
    }
    y += 4 * s;
    // Procedural.
    const ProcState *ps = &c->rt.proc;
    u32 gc = (ps->glue_err_pos > 0.002f || ps->glue_err_ang > 1.0f) ? BAD : TXT;
    y = line(x, y, gc, "glue %.3f mm / %.2f deg  right %.1f mm%s", ps->glue_err_pos * 1000, ps->glue_err_ang, ps->right_err_pos * 1000,
             ps->reach_shifted ? "  (reach shifted)" : "");
    y = line(x, y, TXT, "damage reaction %s  limb %s%s  hp %.0f", proc_damage_active(ps) ? "ACTIVE" : "rest",
             (c->limb & LIMB_LEGS) ? "LEGS " : "", (c->limb & LIMB_ARMS) ? "ARMS" : "", w->health[i].hp);
    // Host vs local (clients only: what the host says vs what this machine runs).
    if (!w->authority) {
        const AnimNetState *n = &c->net;
        bool m0 = n->state[0] == (u8)ct->layer[0].state, m1 = n->state[1] == (u8)ct->layer[1].state;
        y = line(x, y, (m0 && m1) ? TXT : BAD, "host  %s / %s   local %s / %s", ctrl_state_name(ct, 0, n->state[0]), ctrl_state_name(ct, 1, n->state[1]),
                 ctrl_state_name(ct, 0, ct->layer[0].state), ctrl_state_name(ct, 1, ct->layer[1].state));
        y = line(x, y, TXT, "phase host %.2f local %.2f   resyncs %d  mismatch %d ticks", (float)n->phase / 256.0f, ct->layer[0].phase, c->resyncs, c->mismatch_ticks);
    } else {
        y = line(x, y, DIM, "host: authoritative (clients mirror this state)");
    }
    y = line(x, y, DIM, "events hit %u die %u fire %u  rag %s%s", c->events_received[AEV_HIT], c->events_received[AEV_DIE], c->events_received[AEV_FIRE],
             c->rag.active ? "active" : "-", c->rag.active && c->rag.frozen ? " (frozen)" : "");
    y += 4 * s;
    y = line(x, y, DIM, "anim_target lock | anim_solo/mute <layer> | anim_force <layer> <state>");
    (void)y0;
}

static void stand_tick(void) {
    CharacterInst *c = stand();
    const AnimLibrary *L = g_engine.anim;
    if (!c || !L || c->rt.override_clip < 0 || !S.stand_playing) return;
    const Clip *k = &L->clips[c->rt.override_clip];
    float t = c->rt.override_time + (float)g_engine.frame_dt * S.stand_rate;
    if (k->duration > 0) {
        t = fmodf(t, k->duration);
        if (t < 0) t += k->duration;
    }
    c->rt.override_time = t;
    // The host re-evaluates the base pose at tick rate; clients / local evaluation follow it too.
}

void anim_debug_init(void) {
    S.debug = cvar_find("anim_debug");
    cmd_register("anim_target", cmd_anim_target, "lock / unlock the anim debug panel on the character under the crosshair");
    cmd_register("anim_solo", cmd_anim_solo, "anim_solo <layer|off>: solo a controller layer (debug target)");
    cmd_register("anim_mute", cmd_anim_mute, "anim_mute <layer|off>: mute a controller layer (debug target)");
    cmd_register("anim_force", cmd_anim_force, "anim_force <layer> <state>: force a state (debug target)");
    cmd_register("anim_clips", cmd_anim_clips, "anim_clips [filter]: list baked clips");
    cmd_register("aimat", cmd_aimat, "aimat [part]: aim at a body part of the character nearest the crosshair");
    cmd_register("anim_stand", cmd_anim_stand, "anim_stand [off]: animation test stand on the character under the crosshair");
    cmd_register("anim_stand_clip", cmd_stand_clip, "anim_stand_clip <name|index|none>: play one clip on the stand");
    cmd_register("anim_stand_scrub", cmd_stand_scrub, "anim_stand_scrub <0..1>: pause the stand clip at a normalized time");
    cmd_register("anim_stand_play", cmd_stand_play, "toggle stand clip playback");
    cmd_register("anim_stand_rate", cmd_stand_rate, "anim_stand_rate <x>: stand clip playback rate");
    cmd_register("anim_stand_mirror", cmd_stand_mirror, "toggle mirrored stand clip");
    cmd_register("anim_stand_item", cmd_stand_item, "anim_stand_item [next|prev|id]: cycle the stand's item (hold styles)");
    cmd_register("anim_stand_char", cmd_stand_char, "anim_stand_char <id>: swap the stand's character definition");
    cmd_register("anim_stand_hit", cmd_stand_hit, "anim_stand_hit <part|all> [damage]: hit the stand on a body part (host)");
    cmd_register("anim_stand_drop", cmd_stand_drop, "ragdoll the stand character");
    cmd_register("anim_stand_push", cmd_stand_push, "anim_stand_push [N]: push the stand ragdoll along the view");
}

void anim_debug_draw(void) {
    if (!S.debug) S.debug = cvar_find("anim_debug");
    stand_tick();
    if (!S.debug || !S.debug->i || !game_world() || !g_engine.anim) return;
    CharacterInst *c = target();
    if (!c) c = stand();
    if (c) draw_controller_panel(c);
}
