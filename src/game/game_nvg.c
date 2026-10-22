// Night vision goggles, game side (docs/NIGHT_OPTICS.md "Gameplay rules"; the rules are pure in
// gameplay/nvg_rules.c, the image in render/post/post_nvg.c).
//
//   client   key 8 (ACT_NVG): a tap flips the goggles down / up when an `nvg` with charge is in the
//            inventory, a 0.4 s hold switches the IR illuminator; the request travels in
//            UserCmd.gear every tick (like the flashlight's BTN_FLASHLIGHT). ADS is stripped from
//            the command while the goggles are down or flipping (the host does the same: prediction
//            agrees). The tube protection (post_nvg: daylight, a light in the face) and an empty
//            battery switch the request off with a hint
//   host     validates the request against the inventory (the first nvg with charge, reading
//            order), drains the item's charge (InvItem.dur), turns it off when empty and mirrors
//            EF_NVG / EF_NVG_IR onto the player entity (everyone sees the goggles and the
//            illuminator; other NVGs see its IR light)
//   flip     every machine times the flip from the flag's edges (the hand case nvg_flip, the
//            goggle mesh's pivot, the view's transition)
#include "game_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/cmd_registry.h"
#include "core/platform.h"
#include "gameplay/inventory.h"
#include "gameplay/nvg_rules.h"
#include "gameplay/characters.h"
#include "render/post/nvg_model.h"
#include "render/post/post.h"
#include "input/input.h"
#include "audio/audio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

#define NVG_HOLD_S 0.4f   // hold 8 this long: the illuminator instead of the goggles

static struct {
    NvgConfig cfg;
    bool loaded;
    // client
    bool want, want_ir;          // the local request (UserCmd.gear)
    float press_t;               // s ACT_NVG held
    bool hold_fired;
    double local_flip_t0;        // render clock of the local request's last edge
    bool local_flipped;
    float off_wait;              // s the host shows off while we want it on
    bool tripped;                // the post's tube protection asked for an off
    // host
    NvgRuleState sv[MAX_PLAYERS];
    // every machine: flag edges per player (render clock)
    struct { bool on; double t0; bool seen; } edge[MAX_PLAYERS];
    Cvar *brightness_floor, *debug;
} NV;

const NvgConfig *nvg_config(void) { return &NV.cfg; }

static void load(void) {
    nvg_config_default(&NV.cfg);
    size_t n = 0;
    char *t = platform_read_file("assets/nvg/nvg.json", &n);
    if (!t || !nvg_config_load(&NV.cfg, t)) log_warn("nvg: assets/nvg/nvg.json missing or invalid (defaults)");
    free(t);
    NV.loaded = true;
}

// The nvg items of a tree in reading order (equipment, then the grid by row): what key 8 picks from.
static int item_views(const ItemDB *db, const InvTree *t, NvgItemView *out, int max) {
    int idx[INV_MAX_ITEMS], n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &t->items[i];
        if (inv_alive(it) && item_def(db, it->s.item)->nvg[0]) idx[n++] = i;
    }
    for (int a = 1; a < n; a++) {   // insertion sort by (loc, y, x)
        int k = idx[a], b = a - 1;
        const InvItem *x = &t->items[k];
        while (b >= 0) {
            const InvItem *y = &t->items[idx[b]];
            int kx = x->loc * 65536 + x->y * 256 + x->x, ky = y->loc * 65536 + y->y * 256 + y->x;
            if (ky <= kx) break;
            idx[b + 1] = idx[b];
            b--;
        }
        idx[b + 1] = k;
    }
    int m = 0;
    for (int i = 0; i < n && m < max; i++) {
        const InvItem *it = &t->items[idx[i]];
        out[m++] = (NvgItemView){ .uid = it->uid, .is_nvg = true, .charge = it->dur };
    }
    return m;
}

static const NvgVariant *variant_of(const ItemDB *db, u16 item) {
    return nvg_variant(&NV.cfg, item ? item_def(db, item)->nvg : NULL);
}

static int nvg_count_cells(World *w, Player *p) {
    int n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &p->inv.t.items[i];
        if (inv_alive(it) && item_def(w->items, it->s.item)->battery_cell) n += it->s.count;
    }
    return n;
}

// ---------------------------------------------------------------- client

static void hint(const char *t) { game_add_hint(t); }

static bool local_can_turn_on(const char **why) {
    Player *p = game_local_player();
    if (!p || !G->world) { *why = NULL; return false; }
    NvgItemView v[16];
    int n = item_views(G->world->items, &p->inv.t, v, 16);
    if (!n) { *why = "No night vision goggles"; return false; }
    if (nvg_rules_pick(v, n) < 0) { *why = "NVG battery empty: Replace battery"; return false; }
    return true;
}

static void set_want(bool on) {
    if (NV.want == on) return;
    NV.want = on;
    if (!on) NV.want_ir = false;
    NV.local_flip_t0 = g_engine.render_time;
    NV.local_flipped = true;
    NV.off_wait = 0.0f;
    audio_play_2d("click", 0.35f, SND_SFX);
}

void nvg_input_frame(float dt, bool blocked) {
    if (!NV.loaded) load();
    // 8 is also slot 8: an open wheel / the spectator controls take it (game_wheels.c, spectator.c)
    bool down = input_down(ACT_NVG) && !blocked;
    bool pressed = input_consume_press(ACT_NVG);
    if (blocked) { NV.press_t = 0.0f; NV.hold_fired = false; return; }
    if (down) {
        NV.press_t += dt;
        if (!NV.hold_fired && NV.press_t >= NVG_HOLD_S) {
            NV.hold_fired = true;
            if (NV.want) {
                NV.want_ir = !NV.want_ir;
                hint(NV.want_ir ? "IR illuminator on" : "IR illuminator off");
                audio_play_2d("click", 0.25f, SND_SFX);
            }
        }
        return;
    }
    if ((NV.press_t > 0.0f || pressed) && !NV.hold_fired) {   // a tap (released before the hold time)
        const char *why = NULL;
        if (NV.want) set_want(false);
        else if (local_can_turn_on(&why)) set_want(true);
        else if (why) hint(why);
    }
    NV.press_t = 0.0f;
    NV.hold_fired = false;
}

// The post's tube protection (bright source) or a lost item: the request goes off.
void nvg_local_shutoff(const char *reason) {
    if (!NV.want) return;
    set_want(false);
    if (reason) hint(reason);
}

void nvg_fill_cmd(UserCmd *c) {
    if (NV.want) c->gear |= GEAR_NVG;
    if (NV.want && NV.want_ir) c->gear |= GEAR_NVG_IR;
}

// The host keeps the flag off while we ask for it (battery ran out, the item was dropped): give up
// after a moment with the reason.
void nvg_client_tick(float dt) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!w || !p) return;   // the listen host too: its own drain switches its request off
    bool host_on = (w->flags[ENT_INDEX(p->ent)] & EF_NVG) != 0;
    NV.off_wait = NV.want && !host_on ? NV.off_wait + dt : 0.0f;
    if (NV.off_wait > 0.8f) {
        const char *why = NULL;
        bool ok = local_can_turn_on(&why);
        nvg_local_shutoff(ok ? "Night vision unavailable" : why);
    }
}

bool nvg_local_want(void) { return NV.want; }
bool nvg_local_ir(void) { return NV.want && NV.want_ir; }

// ---------------------------------------------------------------- host

void nvg_host_tick(World *w, int slot, const UserCmd *c, float dt) {
    if (!w->authority || slot < 0 || slot >= MAX_PLAYERS) return;
    if (!NV.loaded) load();
    Player *p = &w->players[slot];
    NvgRuleState *st = &NV.sv[slot];
    bool want = (c->gear & GEAR_NVG) && !(w->flags[ENT_INDEX(p->ent)] & EF_DEAD);
    NvgItemView v[16];
    int n = item_views(w->items, &p->inv.t, v, 16);
    if (st->on) {   // the item in use must still be carried
        bool have = false;
        for (int i = 0; i < n; i++) have |= v[i].uid == st->uid && v[i].charge > 0;
        if (!have) want = false;
    }
    if (want != st->on) nvg_rules_toggle(st, v, n, w->time);
    nvg_rules_set_illum(st, st->on && (c->gear & GEAR_NVG_IR));
    if (!st->on) return;
    InvItem *it = inv_get(&p->inv.t, st->uid);
    if (!it) { st->on = st->illum = false; return; }
    u16 before = it->dur;
    if (nvg_rules_drain(st, &it->dur, variant_of(w->items, it->s.item), st->illum, dt))
        ev_hint(w, slot, "NVG battery empty");
    if (it->dur != before) p->inv.revision++;
}

u32 nvg_entity_flags(int slot) {
    if (slot < 0 || slot >= MAX_PLAYERS) return 0;
    const NvgRuleState *st = &NV.sv[slot];
    u32 f = (st->on ? EF_NVG : 0) | (st->on && st->illum ? EF_NVG_IR : 0);
    World *w = G->world;
    if (st->on && w) {
        const InvItem *it = inv_get_c(&w->players[slot].inv.t, st->uid);
        if (it && variant_of(w->items, it->s.item)->tubes > 1) f |= EF_NVG_DUAL;
    }
    return f;
}

void nvg_player_reset(int slot) {
    if (slot >= 0 && slot < MAX_PLAYERS) memset(&NV.sv[slot], 0, sizeof(NV.sv[slot]));
    if (slot == G->local_slot) { NV.want = NV.want_ir = false; }
}

// ADS is refused while the goggles are down or flipping: the host by its state, the predicting
// client by its request.
bool nvg_blocks_ads(World *w, int slot) {
    if (!NV.loaded) load();
    float ft = NV.cfg.flip_time;
    if (w->authority) {
        const NvgRuleState *st = &NV.sv[slot];
        return !nvg_rules_ads_allowed(st, w->time, ft);
    }
    if (slot != G->local_slot) return false;
    if (NV.want) return true;
    return NV.local_flipped && g_engine.render_time - NV.local_flip_t0 < ft;
}

// ---------------------------------------------------------------- every machine: flip timing

// 0 (up / off) .. 1 (down): the goggles' flip for a player (the hand case, the mesh pivot); the
// local player follows its own request at once.
float nvg_flip_pos(World *w, int slot) {
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return 0.0f;
    if (!NV.loaded) load();
    float ft = DG_MAX(NV.cfg.flip_time, 0.05f);
    bool on;
    double t0;
    if (slot == G->local_slot) {
        on = NV.want;
        t0 = NV.local_flipped ? NV.local_flip_t0 : -1e9;
    } else {
        Player *p = &w->players[slot];
        on = p->active && (w->flags[ENT_INDEX(p->ent)] & EF_NVG);
        if (!NV.edge[slot].seen || NV.edge[slot].on != on) {
            NV.edge[slot].t0 = NV.edge[slot].seen ? g_engine.render_time : -1e9;
            NV.edge[slot].on = on;
            NV.edge[slot].seen = true;
        }
        t0 = NV.edge[slot].t0;
    }
    float t = (float)DG_CLAMP((g_engine.render_time - t0) / ft, 0.0, 1.0);
    return on ? t : 1.0f - t;
}

float nvg_flip_phase(World *w, int slot) {
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return -1.0f;
    nvg_flip_pos(w, slot);   // updates the edge record
    float ft = DG_MAX(NV.cfg.flip_time, 0.05f);
    double t0 = slot == G->local_slot ? (NV.local_flipped ? NV.local_flip_t0 : -1e9) : NV.edge[slot].t0;
    double ph = (g_engine.render_time - t0) / ft;
    return ph >= 0.0 && ph <= 1.0 ? (float)ph : -1.0f;
}

static float goggles_phase_hook(World *w, int slot) { return nvg_flip_phase(w, slot); }

// Worn while down or flipping (up after the flip: off the head); the model by the replicated variant bit.
static bool goggles_draw_hook(World *w, int slot, float *down, u16 *item) {
    float d = nvg_flip_pos(w, slot);
    bool flipping = nvg_flip_phase(w, slot) >= 0.0f;
    if (d <= 0.0f && !flipping) return false;
    *down = d;
    Player *p = &w->players[slot];
    bool dual = slot == G->local_slot ? nvg_player_variant(w, slot)->tubes > 1 : (w->flags[ENT_INDEX(p->ent)] & EF_NVG_DUAL) != 0;
    *item = item_find(w->items, dual ? "nvg_dual" : "nvg");
    return *item != 0;
}

// The variant of the goggles a player wears (the first nvg of the inventory for the local player /
// on the host; remote players: the replicated worn item, else the default).
const NvgVariant *nvg_player_variant(World *w, int slot) {
    if (!NV.loaded) load();
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return &NV.cfg.v[0];
    Player *p = &w->players[slot];
    NvgItemView v[16];
    int n = item_views(w->items, &p->inv.t, v, 16);
    int k = nvg_rules_pick(v, n);
    if (w->authority && NV.sv[slot].on) {
        const InvItem *it = inv_get_c(&p->inv.t, NV.sv[slot].uid);
        if (it) return variant_of(w->items, it->s.item);
    }
    if (k >= 0) {
        const InvItem *it = inv_get_c(&p->inv.t, v[k].uid);
        if (it) return variant_of(w->items, it->s.item);
    }
    return &NV.cfg.v[0];
}

// The local goggles' item instance (blemish seed, battery for the HUD): uid 0 = none.
u16 nvg_local_item(float *charge, u16 *item) {
    Player *p = game_local_player();
    if (charge) *charge = 0.0f;
    if (item) *item = 0;
    if (!p || !G->world) return 0;
    NvgItemView v[16];
    int n = item_views(G->world->items, &p->inv.t, v, 16);
    int k = nvg_rules_pick(v, n);
    if (k < 0 && n > 0) k = 0;
    if (k < 0) return 0;
    const InvItem *it = inv_get_c(&p->inv.t, v[k].uid);
    if (!it) return 0;
    if (charge) *charge = inv_battery_charge(G->world->items, it);
    if (item) *item = it->s.item;
    return it->uid;
}

// ---------------------------------------------------------------- debug

static void cmd_nvg(int argc, const char **argv) {
    bool on = argc > 1 ? atoi(argv[1]) != 0 : !NV.want;
    const char *why = NULL;
    if (on && !local_can_turn_on(&why)) { log_info("nvg: %s", why ? why : "no player"); return; }
    set_want(on);
    if (argc > 2) NV.want_ir = on && atoi(argv[2]) != 0;
    log_info("nvg: %s%s", NV.want ? "on" : "off", NV.want_ir ? " + IR illuminator" : "");
}

static void cmd_nvg_report(int argc, const char **argv) {
    DG_UNUSED(argc); DG_UNUSED(argv);
    World *w = G->world;
    float charge = 0.0f;
    u16 item = 0;
    u16 uid = nvg_local_item(&charge, &item);
    Player *lp = game_local_player();
    log_info("nvg_report: want %d ir %d flip %.2f ads_blocked %d ads_t %.2f item %u (%s) charge %.3f",
             NV.want, NV.want_ir, w ? nvg_flip_pos(w, G->local_slot) : 0.0f, w ? nvg_blocks_ads(w, G->local_slot) : 0,
             lp ? lp->sim.wpn.ads_t : 0.0f, uid, item && w ? item_def(w->items, item)->id : "-", charge);
    if (w && w->authority)
        for (int s = 0; s < MAX_PLAYERS; s++)
            if (w->players[s].active)
                log_info("nvg_report: slot %d host on %d ir %d uid %u flags %s%s", s, NV.sv[s].on, NV.sv[s].illum, NV.sv[s].uid,
                         (w->flags[ENT_INDEX(w->players[s].ent)] & EF_NVG) ? "EF_NVG " : "",
                         (w->flags[ENT_INDEX(w->players[s].ent)] & EF_NVG_IR) ? "EF_NVG_IR" : "");
    if (w)
        for (int s = 0; s < MAX_PLAYERS; s++)
            if (w->players[s].active && s != G->local_slot)
                log_info("nvg_report: remote slot %d EF_NVG %d EF_NVG_IR %d flip %.2f", s,
                         (w->flags[ENT_INDEX(w->players[s].ent)] & EF_NVG) != 0,
                         (w->flags[ENT_INDEX(w->players[s].ent)] & EF_NVG_IR) != 0, nvg_flip_pos(w, s));
}

// Dev: the local goggles' battery (host), and a battery swap through the intent (as the menu's Replace battery).
static void cmd_nvg_battery(int argc, const char **argv) {
    World *w = G->world;
    Player *p = game_local_player();
    u16 uid = nvg_local_item(NULL, NULL);
    if (!w || !p || !uid) { log_info("nvg_battery: no goggles"); return; }
    if (argc > 1) {
        if (!w->authority) { log_info("nvg_battery: host only"); return; }
        InvItem *it = inv_get(&p->inv.t, uid);
        it->dur = (u16)DG_CLAMP((int)lroundf((float)atof(argv[1]) * NVG_CHARGE_FULL), 0, NVG_CHARGE_FULL);
        p->inv.revision++;
    }
    float c = 0.0f;
    nvg_local_item(&c, NULL);
    log_info("nvg_battery: %.3f (%d AA batteries carried)", c, nvg_count_cells(w, p));
}

static void cmd_nvg_recharge(int argc, const char **argv) {
    DG_UNUSED(argc); DG_UNUSED(argv);
    u16 uid = nvg_local_item(NULL, NULL);
    if (!uid) { log_info("nvg_recharge: no goggles"); return; }
    InvIntent in;
    memset(&in, 0, sizeof(in));
    in.type = II_RECHARGE;
    in.src_side = in.dst_side = INV_SIDE_SELF;
    in.src = uid;
    int r = game_inv_intent(&in);
    log_info("nvg_recharge: %s", r == IIR_OK ? "ok" : inv_result_text(r));
}

// The intensifier's state (post_nvg.c): ABC gain, metering, whiteout, protection, quality.
static void cmd_nvg_debug(int argc, const char **argv) {
    DG_UNUSED(argc); DG_UNUSED(argv);
    const NvgPostStats *st = post_nvg_stats();
    log_info("nvg_debug: flux mean %.3g current %.3g  gain %.1f (log2 %.2f)  whiteout %.2f flicker %.2f overbright %.2f s  "
             "quality %d tube scale %.3f halo %.1f px  nir %d  illuminator %d",
             st->phi_mean, st->phi_current, st->gain, st->log_gain, st->whiteout, st->flicker, st->overbright, st->quality,
             st->tube_scale, st->halo_px, st->nir, nvg_local_ir());
}

void nvg_register(void) {
    load();
    chars_set_goggles_hook(goggles_phase_hook);   // the third-person body's flip reach (characters.c)
    chars_set_goggles_draw_hook(goggles_draw_hook);   // the goggles on the head (characters.c)
    NV.brightness_floor = cvar_reg_float("nvg_brightness_floor", 0.0f, 0.0f, 0.15f, CVAR_ARCHIVE | CVAR_EDITABLE,
                                         "night vision: minimum image brightness (accessibility)");
    cmd_register("nvg", cmd_nvg, "nvg [0|1] [ir 0|1]: night vision goggles on / off (key 8; needs an nvg with charge)");
    cmd_register("nvg_battery", cmd_nvg_battery, "nvg_battery [0..1]: the local goggles' battery (set: host, dev)");
    cmd_register("nvg_recharge", cmd_nvg_recharge, "nvg_recharge: replace the local goggles' battery with an AA from the inventory (the menu's Replace battery)");
    cmd_register("nvg_debug", cmd_nvg_debug, "nvg_debug: the intensifier now: ABC gain, metered flux (mean / peak), whiteout, autogating, tube protection, quality, halo size");
    cmd_register("nvg_report", cmd_nvg_report, "nvg_report: the goggles' request, flip, ADS block, item, battery and the replicated flags");
}

float nvg_brightness_floor(void) { return NV.brightness_floor ? NV.brightness_floor->f : 0.0f; }

float nvg_hud_amount(void) {
    World *w = G->world;
    return w ? nvg_flip_pos(w, game_view_slot()) : 0.0f;
}

// ---------------------------------------------------------------- the image (game_post.c)

void nvg_ir_lights(World *w, int view_slot, PostSettings *s);

// The intensifier's settings for this frame: the viewed player's goggles (a spectator in first person
// sees through them too), the flip, the variant, the item's seed; the own muzzle flash kicks the
// whiteout. The tube protection's trip switches the local request off.
void nvg_post(PostSettings *s) {
    World *w = G->world;
    memset(&s->nvg, 0, sizeof(s->nvg));
    if (post_nvg_take_shutoff()) nvg_local_shutoff("Bright source: NVG tube protection");
    if (!w) return;
    int slot = game_view_slot();
    if (slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) return;
    float a = nvg_flip_pos(w, slot);
    if (a <= 0.0f) return;
    const NvgVariant *v = nvg_player_variant(w, slot);
    s->nvg.amount = a;
    s->nvg.v = *v;
    memcpy(s->nvg.w_gaas, NV.cfg.w_gaas, sizeof(s->nvg.w_gaas));
    s->nvg.k_vis = NV.cfg.k_vis;
    s->nvg.k_nir = NV.cfg.k_nir;
    s->nvg.y_sat = NV.cfg.y_sat;
    s->nvg.overload = NV.cfg.overload;
    s->nvg.floor = nvg_brightness_floor();
    u16 uid = slot == G->local_slot ? nvg_local_item(NULL, NULL) : 0;
    s->nvg.seed = uid ? dg_hash_u32(uid * 977u + (u32)slot) : dg_hash_u32((u32)slot + 31u);
    const Player *p = &w->players[slot];
    if (w->flags[ENT_INDEX(p->ent)] & EF_FIRING) s->nvg.flash = 3.0f;   // the own muzzle flash, right in front of the tube
    // near-IR layer: moonlight ~half near-IR; airglow and starlight mostly near-IR, the daytime sky little
    float day = daynight_daylight();
    s->nvg.nir = true;
    s->nvg.ir_moon = NV.cfg.ir_moon;
    s->nvg.ir_sky = NV.cfg.ir_sky + (0.5f - NV.cfg.ir_sky) * day;
    s->nvg.sky_glow = 0.5f;
    s->nvg.refl_default = 0.40f;
    s->nvg.refl_metal = 0.25f;
    s->nvg.refl_foliage = 0.55f;
    WeatherParams wp;
    weather_current(w, &wp);
    s->nvg.haze = 0.00005f;   // clear night air: aerosols show a beam barely; fog, smoke and rain show it well
    s->nvg.rain = wp.v[WP_PRECIP];
    nvg_ir_lights(w, slot, s);
}

// IR illuminators (EF_NVG_IR, the local request at once): IR-only spots from each player's eye along
// the aim, seen by every NVG (the own one included), invisible to the naked eye.
void nvg_ir_lights(World *w, int view_slot, PostSettings *s) {
    const NvgVariant *dv = &NV.cfg.v[0];
    s->nvg.ir_count = 0;
    for (int k = 0; k < MAX_PLAYERS && s->nvg.ir_count < 8; k++) {
        const Player *op = &w->players[k];
        if (!op->active || !ent_valid(w, op->ent)) continue;
        int i = (int)ENT_INDEX(op->ent);
        bool on = k == G->local_slot ? nvg_local_ir() : (w->flags[i] & EF_NVG_IR) != 0;
        if (!on || (w->flags[i] & EF_DEAD)) continue;
        bool own = k == G->local_slot;
        float yaw = own ? G->view.yaw : quat_get_yaw(w->xf[i].rot), pitch = own ? G->view.pitch : w->anim[i];
        vec3 f = dir_from_angles(yaw, pitch);
        vec3 eye = v3_add(w->xf[i].pos, v3(0, (w->flags[i] & EF_CROUCHED) ? PLAYER_EYE_CROUCH : PLAYER_EYE_STAND, 0));
        const NvgVariant *v = own ? nvg_player_variant(w, k) : dv;
        int n = s->nvg.ir_count++;
        s->nvg.ir[n].pos = v3_madd(eye, f, 0.12f);
        s->nvg.ir[n].dir = f;
        s->nvg.ir[n].range = v->illum_range;
        s->nvg.ir[n].cos_outer = cosf(v->illum_angle * 0.5f * DG_DEG2RAD);
        s->nvg.ir[n].cos_inner = cosf(v->illum_angle * 0.25f * DG_DEG2RAD);
        s->nvg.ir[n].intensity = v->illum_intensity;
    }
    DG_UNUSED(view_slot);
}
