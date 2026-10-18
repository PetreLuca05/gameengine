// Radial wheels in the game (docs/UI_WHEELS_HUD.md): the healing (C), throwables (hold G; a tap
// drops), ping (middle mouse / Z; a tap pings what you look at) and door (hold E on a door; a tap
// uses it) wheels on the shared widget (ui/widgets/radial_wheel.h), plus the HUD-centre timed ring
// for consumables, cooking grenades and hold interactions.
//
// Tap vs hold: each trigger key opens its wheel once held for wheel_hold_time; a release before
// that is the tap action. While a wheel is open the mouse / right stick pick the slot instead of
// turning the view, number keys and the scroll wheel pick directly, right click cancels, and the
// simulation runs at wheel_timescale (single player, or hosting alone). Releasing the key selects.
#include "game_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/entities.h"
#include "gameplay/weapons.h"
#include "gameplay/throwables.h"
#include "input/input.h"
#include "net/net.h"
#include "ui/console.h"
#include "ui/menus.h"
#include "ui/widgets/radial_wheel.h"
#include "ui/widgets/timed_ring.h"
#include "ui/gui/gui.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

typedef enum WheelKind { WK_NONE, WK_HEAL, WK_THROW, WK_PING, WK_DOOR, WK_COUNT } WheelKind;

typedef struct Gate {
    bool down;       // the trigger key is held
    float held;      // seconds
    bool opened;     // the wheel opened during this hold
    bool armed;      // a hold here may open the wheel (E: only on doors)
    EntityId target; // E: the door
    bool swallow;    // E pressed to end a peek: never reaches the simulation
} Gate;

static struct {
    WheelLib lib;
    bool loaded;
    RadialWheel wheel;
    int kind;                    // WheelKind of the open wheel
    Gate gate[WK_COUNT];
    EntityId door;               // the door wheel's door
    bool use_pulse;              // E tapped on a door: send BTN_USE for one command
    u16 auto_use;                // healing wheel: hold attack on this item until its use ends
    float auto_use_t;            // seconds since it started (gives up if the use never starts)
    bool auto_use_seen;          // its use started
    TimedRingState ring;         // HUD centre
    Cvar *hold_time, *timescale, *enable, *heal_auto, *ring_on;
} W;

static const char *const k_files[] = { "healing", "throwables", "ping", "door" };

static void load(void) {
    W.loaded = true;
    memset(&W.lib, 0, sizeof(W.lib));
    for (int i = 0; i < (int)ARRAY_COUNT(k_files); i++) {
        char path[96], err[160];
        snprintf(path, sizeof(path), "assets/ui/wheels/%s.json", k_files[i]);
        char *json = platform_read_file(path, NULL);
        WheelDef d;
        if (!json) { log_warn("wheels: missing %s", path); continue; }
        if (wheel_def_parse(&d, json, err, sizeof(err))) wheel_lib_add(&W.lib, &d);
        else log_warn("wheels: %s: %s", path, err);
        free(json);
    }
}

static const WheelDef *def_for(int kind) {
    if (!W.loaded) load();
    static const char *const ids[WK_COUNT] = { "", "healing", "throwables", "ping", "door" };
    return kind > WK_NONE && kind < WK_COUNT ? wheel_lib_find(&W.lib, ids[kind]) : NULL;
}

bool wheels_open(void) { return W.wheel.open; }

float wheels_timescale(void) {
    if (!W.wheel.open || !G || G->mode != GM_HOST) return 1.0f;
    // Hosting with others connected: never slow their game down.
    if (net_role() == NET_HOST && net_host_client_count() > 0) return 1.0f;
    return clampf(W.timescale->f, 0.05f, 1.0f);
}

// ------------------------------------------------------------------ slots

static bool heal_item(const ItemDef *d) {
    return d->kind == ITEM_CONSUMABLE && !d->vehicle_use && (d->heal > 0 || d->armor > 0);
}

static bool item_matches(const WheelDef *def, const ItemDef *d) {
    if (def->filter_kind[0]) {
        const char *k = d->kind == ITEM_CONSUMABLE ? "consumable" : d->kind == ITEM_THROWABLE ? "throwable" : d->kind == ITEM_WEAPON ? "weapon" : d->kind == ITEM_AMMO ? "ammo" : "misc";
        if (strcmp(k, def->filter_kind)) return false;
    }
    if (!strcmp(def->filter_tag, "heal") && !heal_item(d)) return false;
    return true;
}

// Disabled reason for a healing item (full health / full armour).
static const char *heal_reason(const ItemDef *d) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!p) return NULL;
    const Health *h = &w->health[ENT_INDEX(p->ent)];
    bool hp_full = h->hp >= h->max_hp - 0.01f, ar_full = h->max_armor <= 0 || h->armor >= h->max_armor - 0.01f;
    if (d->heal > 0 && d->armor <= 0 && hp_full) return "Full health";
    if (d->armor > 0 && d->heal <= 0 && ar_full) return "Armor full";
    if (hp_full && ar_full) return "Full health";
    return NULL;
}

// Inventory wheels: one slot per carried item type matching the filter, in the file's order.
static int build_inventory_slots(const WheelDef *def, WheelSlot *out, int max) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!p) return 0;
    int n = 0;
    for (int i = 0; i < INV_MAX_ITEMS && n < max; i++) {
        const InvSlot *s = &p->inv.t.items[i].s;
        if (!inv_alive(&p->inv.t.items[i]) || p->inv.t.items[i].loc == INV_LOC_NONE) continue;
        const ItemDef *d = item_def(w->items, s->item);
        if (!item_matches(def, d)) continue;
        int k = 0;
        while (k < n && out[k].item != s->item) k++;
        if (k < n) { out[k].count += s->count; continue; }
        WheelSlot *o = &out[n++];
        memset(o, 0, sizeof(*o));
        dg_strcpy(o->id, sizeof(o->id), d->id);
        dg_strcpy(o->label, sizeof(o->label), d->name);
        dg_strcpy(o->desc, sizeof(o->desc), d->desc);
        o->item = s->item;
        o->count = s->count;
        if (d->kind == ITEM_THROWABLE) dg_strcpy(o->icon, sizeof(o->icon), d->thr.kind == THROW_FLARE ? "flare" : d->thr.kind == THROW_MOLOTOV ? "molotov" : "grenade");
        if (!strcmp(def->filter_tag, "heal")) {
            const char *r = heal_reason(d);
            if (r) { o->disabled = true; dg_strcpy(o->reason, sizeof(o->reason), r); }
        }
    }
    // Display order: the file's list first, then the rest as carried.
    for (int a = 1; a < n; a++) {
        WheelSlot t = out[a];
        int oa = wheel_def_order(def, t.id);
        if (oa < 0) oa = 1000 + a;
        int b = a - 1;
        while (b >= 0) {
            int ob = wheel_def_order(def, out[b].id);
            if (ob < 0) ob = 1000 + b;
            if (ob <= oa) break;
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = t;
    }
    return n;
}

static int build_static_slots(const WheelDef *def, WheelSlot *out) {
    const GuiTheme *th = gui_theme();
    for (int i = 0; i < def->slot_count; i++) {
        const WheelSlotDef *sd = &def->slots[i];
        WheelSlot *o = &out[i];
        memset(o, 0, sizeof(*o));
        dg_strcpy(o->id, sizeof(o->id), sd->id);
        dg_strcpy(o->label, sizeof(o->label), sd->label);
        dg_strcpy(o->desc, sizeof(o->desc), sd->desc);
        dg_strcpy(o->icon, sizeof(o->icon), sd->icon);
        dg_strcpy(o->sub, sizeof(o->sub), sd->sub);
        if (sd->color >= 0) o->color = th->ping[sd->color];
    }
    return def->slot_count;
}

static void door_slots(WheelSlot *s, int n) {
    World *w = G->world;
    Player *p = game_local_player();
    int i = ent_valid(w, W.door) ? (int)ENT_INDEX(W.door) : -1;
    if (i < 0 || !p) return;
    bool open = (w->flags[i] & EF_OPEN) != 0;
    bool locked = (w->flags[i] & EF_LOCKED) != 0 && !(w->door[i].key_item && inv_has(&p->inv, w->door[i].key_item));
    for (int k = 0; k < n; k++) {
        if (!strcmp(s[k].id, "open")) {
            dg_strcpy(s[k].label, sizeof(s[k].label), open ? "Close" : "Open");
            if (open) dg_strcpy(s[k].icon, sizeof(s[k].icon), "door_close");
            if (locked) { s[k].disabled = true; dg_strcpy(s[k].reason, sizeof(s[k].reason), "Locked"); }
        } else if (!strcmp(s[k].id, "slam")) {
            dg_strcpy(s[k].label, sizeof(s[k].label), open ? "Slam Shut" : "Kick Open");
            if (locked) { s[k].disabled = true; dg_strcpy(s[k].reason, sizeof(s[k].reason), "Locked"); }
        } else if (!strcmp(s[k].id, "peek")) {
            const char *r = peek_block_reason(W.door);
            if (r) { s[k].disabled = true; dg_strcpy(s[k].reason, sizeof(s[k].reason), r); }
        }
    }
}

static bool open_def(int kind, const WheelDef *def) {
    if (!def) return false;
    WheelSlot slots[RADIAL_MAX_SLOTS];
    int n = def->source == WHEEL_INVENTORY ? build_inventory_slots(def, slots, def->max_slots) : build_static_slots(def, slots);
    if (kind == WK_DOOR) door_slots(slots, n);
    int count = DG_CLAMP(n, def->min_slots, def->max_slots);
    if (def->source == WHEEL_STATIC) count = n;
    // Initial selection: the default slot, or the equipped item's slot.
    int initial = def->default_slot;
    if (def->source == WHEEL_INVENTORY) {
        const InvSlot *eq = weapon_equipped(G->world, G->local_slot);
        for (int k = 0; k < n && eq; k++) if (slots[k].item == eq->item) initial = k;
    }
    for (int k = n; k < count; k++) { memset(&slots[k], 0, sizeof(slots[k])); slots[k].empty = true; }
    radial_wheel_open(&W.wheel, def, slots, count, initial);
    W.kind = kind;
    return true;
}

static bool open_kind(int kind) { return open_def(kind, def_for(kind)); }

// ------------------------------------------------------------------ actions

// Selects the hotbar slot bound to an item, or binds its first stack to a free hotbar slot (the
// selected one when all are taken) and selects that (docs/INVENTORY.md: binds are to instances).
static bool equip_item(u16 item) {
    Player *p = game_local_player();
    if (!p || !item) return false;
    int at = inv_hot_find(&p->inv, item);
    if (at < 0) {
        InvItem *it = inv_find(&p->inv, item);
        if (!it) return false;
        int to = G->hotbar_slot;
        for (int k = INV_HOTBAR - 1; k >= 0; k--) if (!inv_hot_c(&p->inv, k)->item) to = k;
        InvIntent in;
        memset(&in, 0, sizeof(in));
        in.type = II_BIND;
        in.src = it->uid;
        in.aux = (u8)to;
        if (game_inv_intent(&in) != IIR_OK) return false;
        at = to;
    }
    G->hotbar_slot = (u8)at;
    G->phone_out = false;
    return true;
}

static void start_auto_use(u16 item) {
    if (!equip_item(item)) return;
    if (W.heal_auto->i) {
        W.auto_use = item;
        W.auto_use_t = 0;
        W.auto_use_seen = false;
    }
}

// Tap on the healing key: the first usable healing item in the wheel's order.
static void quick_heal(void) {
    const WheelDef *def = def_for(WK_HEAL);
    if (!def) return;
    WheelSlot slots[RADIAL_MAX_SLOTS];
    int n = build_inventory_slots(def, slots, RADIAL_MAX_SLOTS);
    for (int k = 0; k < n; k++) {
        if (!wheel_slot_selectable(&slots[k])) continue;
        start_auto_use(slots[k].item);
        return;
    }
    game_add_hint(n ? "Full health" : "No healing items");
}

static void execute(int kind, const WheelSlot *s) {
    switch (kind) {
    case WK_HEAL: start_auto_use(s->item); break;
    case WK_THROW: equip_item(s->item); break;
    case WK_PING: game_ping_request(ping_type_from_id(s->id)); break;
    case WK_DOOR:
        if (!strcmp(s->id, "peek")) peek_start(W.door);
        else game_rpc(RPC_DOOR_ACTION, (int)W.door, !strcmp(s->id, "slam") ? DOOR_ACT_SLAM : DOOR_ACT_TOGGLE, 0);
        break;
    default: break;
    }
}

static void tap(int kind) {
    switch (kind) {
    case WK_HEAL: quick_heal(); break;
    case WK_THROW: game_rpc(RPC_INV_DROP, G->hotbar_slot, 0, 0); break;
    case WK_PING: game_ping_request(-1); break;
    case WK_DOOR: W.use_pulse = true; break;
    default: break;
    }
}

static void close_wheel(bool accept) {
    if (!W.wheel.open) return;
    int kind = W.kind;
    if (!accept) { radial_wheel_cancel(&W.wheel); W.kind = WK_NONE; return; }
    int k = radial_wheel_release(&W.wheel);
    W.kind = WK_NONE;
    if (k < 0) return;
    const WheelSlot *s = &W.wheel.slots[k];
    // Sub-wheel hook: the slot opens another wheel (still held: release selects there).
    if (s->sub[0]) {
        const WheelDef *sub = wheel_lib_find(&W.lib, s->sub);
        if (sub && open_def(kind, sub)) return;
    }
    execute(kind, s);
}

// ------------------------------------------------------------------ frame

static bool action_down(int kind) {
    switch (kind) {
    case WK_HEAL: return input_down(ACT_HEAL_WHEEL);
    case WK_THROW: return input_down(ACT_DROP);
    case WK_PING: return input_down(ACT_PING);
    case WK_DOOR: return input_down(ACT_USE);
    default: return false;
    }
}

static bool is_door(EntityId e) {
    World *w = G->world;
    return ent_valid(w, e) && w->type[ENT_INDEX(e)] == ET_DOOR;
}

static void update_ring(float dt) {
    World *w = G->world;
    Player *p = game_local_player();
    bool active = false;
    float dur = 0, rem = 0;
    int mode = TR_DRAIN;
    const char *label = "";
    static char rv_label[48];
    if (p && p->sim.move.mode != MOVE_DEAD) {
        const InvSlot *eq = weapon_equipped(w, G->local_slot);
        const ItemDef *d = eq && eq->item ? item_def(w->items, eq->item) : NULL;
        const char *tl = NULL;
        const ActionState *act = &p->sim.act;
        const DownState *dn = &p->sim.down;
        if (act->phase == ACTP_RUNNING && act->dur) {
            // Revive / extract / insert (docs/REVIVE.md): predicted, so the ring starts on the press.
            active = true;
            dur = (float)act->dur / (float)REVIVE_HZ;
            rem = act_remaining(act);
            int ts = world_player_by_ent(w, act->target);
            if (act->kind == RACT_REVIVE) snprintf(rv_label, sizeof(rv_label), "Reviving %s", ts >= 0 ? w->players[ts].name : "");
            else snprintf(rv_label, sizeof(rv_label), "%s", act->kind == RACT_EXTRACT ? "Extracting DNA" : "Inserting DNA");
            label = rv_label;
        } else if (dn->downed && dn->reviver != REVIVE_NOBODY && dn->revive_total) {
            // The downed player's ring follows the reviver (host state).
            active = true;
            dur = (float)dn->revive_total / (float)REVIVE_HZ;
            rem = (float)(dn->revive_total - DG_MIN(dn->revive_ticks, dn->revive_total)) / (float)REVIVE_HZ;
            snprintf(rv_label, sizeof(rv_label), "Revived by %s", dn->reviver < MAX_PLAYERS ? w->players[dn->reviver].name : "");
            label = rv_label;
        } else if (d && d->kind == ITEM_CONSUMABLE && p->sim.wpn.use_t >= 0 && d->use_time > 0) {
            active = true;
            dur = d->use_time;
            rem = fmaxf(0.0f, d->use_time - p->sim.wpn.use_t);
            label = d->name;
        } else if (throwables_ring(w, G->local_slot, &p->sim, &dur, &rem, &mode, &tl)) {
            active = true;
            label = tl;
        } else if (G->hold_progress > 0.001f && ent_valid(w, G->focus)) {
            const InteractDef *def = w->inter[ENT_INDEX(G->focus)].def;
            if (def && def->hold_time > 0) {
                active = true;
                dur = def->hold_time;
                rem = def->hold_time * (1.0f - G->hold_progress);
                mode = TR_CHARGE;
                label = def->name;
            }
        }
    }
    timed_ring_update(&W.ring, active && W.ring_on->i, dur, rem, mode, label, dt);
}

bool wheels_frame(float dt, bool ui_blocking) {
    if (!G || !G->world) return false;
    Player *p = game_local_player();
    bool alive = p && p->sim.move.mode != MOVE_DEAD;
    radial_wheel_tick(&W.wheel, dt);
    update_ring(dt);
    // Auto-use (healing wheel): stops when the use ends, the item changes or it never starts.
    if (W.auto_use) {
        const InvSlot *eq = weapon_equipped(G->world, G->local_slot);
        W.auto_use_t += dt;
        if (p && p->sim.wpn.use_t >= 0) W.auto_use_seen = true;
        // The equip happens in the next tick: a mismatch counts once the use has started.
        bool done = !alive || (W.auto_use_seen && (!eq || eq->item != W.auto_use || p->sim.wpn.use_t < 0)) ||
                    (!W.auto_use_seen && W.auto_use_t > 1.0f) || input_peek_press(ACT_ATTACK);
        if (done) W.auto_use = 0;
    }
    if (ui_blocking || !alive || !W.enable->i) {
        close_wheel(false);
        for (int k = 0; k < WK_COUNT; k++) W.gate[k].down = false;
        return false;
    }
    float hold = W.hold_time->f;
    for (int k = WK_HEAL; k < WK_COUNT; k++) {
        Gate *g = &W.gate[k];
        bool down = action_down(k);
        if (down && !g->down) {
            g->down = true;
            g->held = 0;
            g->opened = false;
            g->armed = !W.wheel.open && peek_amount() <= 0.0f;
            g->swallow = false;
            if (k == WK_DOOR) {
                g->target = G->focus;
                g->swallow = peek_amount() > 0.0f;
                g->armed = g->armed && is_door(G->focus);
            }
        }
        if (g->down && down) {
            g->held += dt;
            const WheelDef *def = def_for(k);
            float ht = def && def->hold_time >= 0 ? def->hold_time : hold;
            if (g->armed && !g->opened && !W.wheel.open && g->held >= ht) {
                if (k == WK_DOOR) W.door = g->target;
                g->opened = open_kind(k);
            }
        }
        if (g->down && !down) {
            g->down = false;
            if (g->opened) { if (W.kind == k) close_wheel(true); }
            else if (g->armed) tap(k);
        }
    }
    if (!W.wheel.open) return false;
    // The open wheel takes the look, number keys and the scroll wheel.
    InputFrame f = input_frame();
    if (v2_len(f.rstick) > 0.35f) radial_wheel_feed_stick(&W.wheel, f.rstick);
    else radial_wheel_feed_mouse(&W.wheel, f.mouse_px);
    for (int i = 0; i < 9; i++) if (input_consume_press((Action)(ACT_SLOT1 + i))) radial_wheel_pick(&W.wheel, i);
    if (input_consume_press(ACT_NEXT_ITEM)) radial_wheel_step(&W.wheel, 1);
    if (input_consume_press(ACT_PREV_ITEM)) radial_wheel_step(&W.wheel, -1);
    if (input_consume_press(ACT_AIM)) {
        int k = W.kind;
        close_wheel(false);
        W.gate[k].opened = true;   // the release does nothing more
    }
    // The door wheel closes if the door goes out of reach.
    if (W.kind == WK_DOOR && !ent_valid(G->world, W.door)) close_wheel(false);
    return true;
}

void wheels_filter_cmd(UserCmd *c) {
    // E on a door: the press waits for the release (tap = use) or the wheel (hold).
    Gate *g = &W.gate[WK_DOOR];
    if ((g->down && (g->armed || g->swallow)) || W.kind == WK_DOOR) c->buttons &= (u16)~BTN_USE;
    if (peek_active()) c->buttons &= (u16)~(BTN_USE | BTN_ATTACK | BTN_AIM);
    if (W.use_pulse) {
        c->buttons |= BTN_USE;
        c->use_target = g->target;
        W.use_pulse = false;
    }
    // The other wheels' keys never reach the simulation while open; attack waits too.
    if (W.wheel.open) c->buttons &= (u16)~(BTN_ATTACK | BTN_RELOAD);
    else if (W.auto_use) c->buttons |= BTN_ATTACK;
}

void wheels_on_hurt(void) {
    peek_on_hurt();
    // interrupt_on_damage: the host cancels the use; stop holding attack for it.
    Player *p = game_local_player();
    const InvSlot *eq = p ? weapon_equipped(G->world, G->local_slot) : NULL;
    if (W.auto_use && eq && item_def(G->world->items, eq->item)->interrupt_on_damage) W.auto_use = 0;
}

void wheels_reset(void) {
    W.wheel.open = false;
    W.wheel.t = 0;
    W.kind = WK_NONE;
    W.auto_use = 0;
    W.use_pulse = false;
    memset(W.gate, 0, sizeof(W.gate));
    memset(&W.ring, 0, sizeof(W.ring));
    peek_stop();
}

void wheels_draw(void) {
    if (!G || !G->world) return;
    if (radial_wheel_visible(&W.wheel)) {
        radial_wheel_draw(&W.wheel, &W.ring);
        return;
    }
    // No wheel: the timed ring at the HUD centre.
    if (W.ring.alpha > 0.01f) ui_timed_ring_state(&W.ring, gui_w() * 0.5f, gui_h() * 0.5f + gui_s(gui_tok("ring_hud_offset")), 1.0f);
}

// ------------------------------------------------------------------ commands (scripted tests)

static int kind_from_name(const char *s) {
    if (!strcmp(s, "heal") || !strcmp(s, "healing")) return WK_HEAL;
    if (!strcmp(s, "throw") || !strcmp(s, "throwables")) return WK_THROW;
    if (!strcmp(s, "ping")) return WK_PING;
    if (!strcmp(s, "door")) return WK_DOOR;
    return WK_NONE;
}

// wheel_open <heal|throw|ping|door> [slot]: opens like a held key (door: the focused door).
static void cmd_wheel_open(int argc, const char **argv) {
    if (argc < 2) { log_info("wheel_open <heal|throw|ping|door> [slot]"); return; }
    int k = kind_from_name(argv[1]);
    if (!k) { log_warn("wheel_open: unknown wheel %s", argv[1]); return; }
    close_wheel(false);
    if (k == WK_DOOR) {
        if (!is_door(G->focus)) { log_warn("wheel_open door: not looking at a door"); return; }
        W.door = G->focus;
    }
    if (open_kind(k) && argc > 2) radial_wheel_pick(&W.wheel, atoi(argv[2]));
    W.gate[k].opened = true;
}

// wheel_select <slot>: picks a slot of the open wheel; wheel_release: selects it (as the key release).
static void cmd_wheel_select(int argc, const char **argv) {
    if (argc > 1 && W.wheel.open) radial_wheel_pick(&W.wheel, atoi(argv[1]));
}
static void cmd_wheel_release(int argc, const char **argv) { (void)argc; (void)argv; close_wheel(true); }
static void cmd_wheel_cancel(int argc, const char **argv) { (void)argc; (void)argv; close_wheel(false); }
static void cmd_wheel_reload(int argc, const char **argv) {
    (void)argc; (void)argv;
    load();
    log_info("wheels: %d definition(s)", W.lib.count);
}
static void cmd_peek(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (peek_active()) { peek_stop(); return; }
    const char *r = peek_block_reason(G->focus);
    if (r) log_warn("peek: %s", r);
    else peek_start(G->focus);
}

void wheels_register(void) {
    W.hold_time = cvar_reg_float("wheel_hold_time", 0.22f, 0.05f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "radial wheels: hold the key this long to open the wheel (a shorter press is the tap action)");
    W.timescale = cvar_reg_float("wheel_timescale", 0.35f, 0.05f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "radial wheels: simulation speed while a wheel is open (single player / hosting alone)");
    W.enable = cvar_reg_bool("wheel_enable", true, CVAR_ARCHIVE | CVAR_EDITABLE, "radial wheels: hold-to-open wheels (off: the keys only do their tap action)");
    W.heal_auto = cvar_reg_bool("wheel_heal_auto_use", true, CVAR_ARCHIVE | CVAR_EDITABLE, "healing wheel / quick heal: start using the item at once");
    W.ring_on = cvar_reg_bool("hud_timed_ring", true, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: the timed ring for healing, cooking grenades and hold interactions");
    cmd_register("wheel_open", cmd_wheel_open, "open a radial wheel as if its key were held: wheel_open heal|throw|ping|door [slot]");
    cmd_register("wheel_select", cmd_wheel_select, "pick a slot of the open radial wheel: wheel_select <slot>");
    cmd_register("wheel_release", cmd_wheel_release, "release the open radial wheel (selects its slot)");
    cmd_register("wheel_cancel", cmd_wheel_cancel, "close the open radial wheel without selecting");
    cmd_register("wheel_reload", cmd_wheel_reload, "reload assets/ui/wheels/*.json");
    cmd_register("peek", cmd_peek, "peek through the visor of the door you look at (again: stop)");
    peek_register();
}
