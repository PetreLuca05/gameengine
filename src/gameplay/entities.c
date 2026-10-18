#include "entities.h"
#include "attachments.h"
#include "core/engine.h"
#include "characters.h"
#include "anim/anim_library.h"
#include "health.h"
#include "zombies/horde.h"
#include "revive/revive.h"
#include "core/log.h"
#include "core/cvar.h"
#include <stdio.h>
#include <math.h>

static u32 col(float r, float g, float b) { return rgba8(r, g, b, 1); }

// ------------------------------------------------------------------ helpers

static vec3 local_to_world(const World *w, EntityId e, vec3 local) {
    int i = (int)ENT_INDEX(e);
    return v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, local));
}

static vec3 player_feet(const World *w, int player) {
    const Player *p = &w->players[player];
    return p->sim.move.pos;
}

// ------------------------------------------------------------------ door

static void door_pose(const World *w, int i, float progress, vec3 *pos, quat *rot) {
    const Door *d = &w->door[i];
    float t = smoothstepf(0, 1, progress);
    if (d->sliding) {
        *pos = v3_add(d->closed_pos, v3(0, d->open_angle * t, 0));
        *rot = d->closed_rot;
        return;
    }
    float ang = d->open_angle * t * d->swing_sign;
    quat q = quat_axis_angle(v3(0, 1, 0), ang);
    *rot = quat_mul(q, d->closed_rot);
    *pos = v3_add(d->hinge, quat_rotate(q, v3_sub(d->closed_pos, d->hinge)));
}

static void door_focus(World *w, EntityId self, int player, char *prompt, int size) {
    int i = (int)ENT_INDEX(self);
    if (w->flags[i] & EF_LOCKED) {
        // key_item travels in the replicated item field so clients can name the key too.
        const ItemDef *key = item_def(w->items, w->door[i].key_item ? w->door[i].key_item : w->item[i]);
        snprintf(prompt, (size_t)size, "Locked - requires %s", key->name);
        return;
    }
    if ((w->vis[i].color >> 24) == DOOR_ALPHA_WINDOW) snprintf(prompt, (size_t)size, (w->flags[i] & EF_OPEN) ? "Close Window" : "Open Window");
    else snprintf(prompt, (size_t)size, (w->flags[i] & EF_OPEN) ? "Close Door" : "Open Door");
}

static void door_use(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
    Door *d = &w->door[i];
    Player *p = &w->players[player];
    if (w->flags[i] & EF_LOCKED) {
        if (d->key_item && inv_has(&p->inv, d->key_item)) {
            w->flags[i] &= ~(u32)EF_LOCKED;
            ev_hint(w, player, "Unlocked with %s", item_def(w->items, d->key_item)->name);
            ev_sound(w, "unlock", w->xf[i].pos, 1.0f, EVENT_ALL);
        } else {
            ev_hint(w, player, "Locked. Requires %s", item_def(w->items, d->key_item)->name);
            ev_sound(w, "locked", w->xf[i].pos, 1.0f, EVENT_ALL);
            return;
        }
    }
    bool opening = d->target < 0.5f;
    if (opening && d->progress < 0.02f) {
        // Swing away from the user: user on the door's +Z side pushes it towards -Z.
        vec3 n = quat_rotate(d->closed_rot, v3(0, 0, 1));
        float side = v3_dot(v3_sub(player_feet(w, player), d->closed_pos), n);
        d->swing_sign = side >= 0 ? 1.0f : -1.0f;
    }
    d->target = opening ? 1.0f : 0.0f;
    if (opening) w->flags[i] |= EF_OPEN;
    else w->flags[i] &= ~(u32)EF_OPEN;
    ev_sound(w, opening ? "door_open" : "door_close", w->xf[i].pos, 1.0f, EVENT_ALL);
}

bool door_action(World *w, EntityId door, int player, int action) {
    if (!ent_valid(w, door) || w->type[ENT_INDEX(door)] != ET_DOOR || player < 0 || player >= MAX_PLAYERS) return false;
    int i = (int)ENT_INDEX(door);
    Player *p = &w->players[player];
    if (!p->active || p->sim.move.mode == MOVE_DEAD) return false;
    if (v3_dist(player_eye_pos(&p->sim.move), ent_focus_point(w, door)) > w->inter[i].range + 0.75f) return false;
    Door *d = &w->door[i];
    if (action == DOOR_ACT_SLAM) {
        if ((w->flags[i] & EF_LOCKED) && !(d->key_item && inv_has(&p->inv, d->key_item))) {
            ev_hint(w, player, "Locked");
            ev_sound(w, "locked", w->xf[i].pos, 1.0f, EVENT_ALL);
            return false;
        }
        door_use(w, door, player);
        d->speed_mult = 3.5f;
        ev_sound(w, "door_slam", w->xf[i].pos, 1.6f, EVENT_ALL);
        horde_noise(w->xf[i].pos, 40.0f);
        return true;
    }
    door_use(w, door, player);
    return true;
}

bool door_visor_view(const World *w, EntityId door, vec3 from, vec3 *eye, vec3 *dir) {
    if (!ent_valid(w, door) || w->type[ENT_INDEX(door)] != ET_DOOR) return false;
    int i = (int)ENT_INDEX(door);
    const Door *d = &w->door[i];
    // The panel as it stands (a peek needs it closed, but follow it anyway).
    vec3 n = quat_rotate(w->xf[i].rot, v3(0, 0, 1));
    float side = v3_dot(v3_sub(from, w->xf[i].pos), n) >= 0 ? 1.0f : -1.0f;
    vec3 v = d->visor_local;
    v.z = -side * (w->vis[i].half.z + 0.035f);
    *eye = v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, v));
    *dir = v3_scale(n, -side);
    return true;
}

static bool door_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    (void)hand;   // either hand (the free one when holding an item)
    int i = (int)ENT_INDEX(self);
    Door *d = &w->door[i];
    // Knob on the user's side of the panel (follows the panel as it swings).
    vec3 n = quat_rotate(w->xf[i].rot, v3(0, 0, 1));
    float side = v3_dot(v3_sub(player_feet(w, player), w->xf[i].pos), n) >= 0 ? 1.0f : -1.0f;
    vec3 knob = d->knob_local;
    knob.z = fabsf(knob.z) * side;
    *pos = local_to_world(w, self, knob);
    // Palm faces the panel, fingers wrap around the knob.
    *rot = quat_mul(w->xf[i].rot, quat_axis_angle(v3(0, 1, 0), side > 0 ? DG_PI : 0.0f));
    return true;
}

static const InteractDef k_door_def = { "door", door_focus, NULL, door_use, NULL, door_hand, 0.0f, HANDS_RIGHT };

EntityId spawn_door(World *w, vec3 base, float yaw, float width, float height, bool locked, u16 key_item, u32 color) {
    EntityId e = ent_spawn(w, ET_DOOR);
    int i = (int)ENT_INDEX(e);
    quat rot = quat_yaw(yaw);
    vec3 center = v3_add(base, v3(0, height * 0.5f, 0));
    w->vis[i].shape = VIS_BOX;
    w->vis[i].half = v3(width * 0.5f, height * 0.5f, 0.025f);
    w->vis[i].color = color;
    w->comps[i] |= C_VISUAL | C_DOOR | C_INTERACT;
    Door *d = &w->door[i];
    d->closed_pos = center;
    d->closed_rot = rot;
    d->hinge = v3_add(center, quat_rotate(rot, v3(-width * 0.5f + 0.02f, 0, 0)));
    d->open_angle = 100.0f * DG_DEG2RAD;
    d->speed = 1.6f;
    d->swing_sign = 1.0f;
    d->key_item = key_item;
    w->item[i] = key_item;
    d->knob_local = v3(width * 0.5f - 0.09f, -height * 0.5f + 1.0f, 0.07f);
    d->visor_local = v3(0, -height * 0.5f + fminf(1.58f, height - 0.25f), 0);
    d->speed_mult = 1.0f;
    if (locked) w->flags[i] |= EF_LOCKED;
    ent_set_pos(w, e, center, rot, true);
    entity_setup(w, e);
    return e;
}

// A casement sash: hinged on its left edge like a door, swings away from the user (out of the room),
// faster and less far; the handle sits mid-height on the free edge.
EntityId spawn_window(World *w, vec3 base, float yaw, float width, float height, u32 color) {
    EntityId e = spawn_door(w, base, yaw, width, height, false, 0, (color & 0x00FFFFFFu) | (DOOR_ALPHA_WINDOW << 24));
    int i = (int)ENT_INDEX(e);
    Door *d = &w->door[i];
    d->open_angle = 80.0f * DG_DEG2RAD;
    d->speed = 2.4f;
    d->knob_local = v3(width * 0.5f - 0.07f, 0, 0.04f);
    w->vis[i].half.z = 0.02f;
    w->inter[i].focus_offset = v3(width * 0.5f - 0.07f, 0, 0);
    return e;
}

bool door_is_window(const World *w, EntityId e) {
    return ent_valid(w, e) && w->type[ENT_INDEX(e)] == ET_DOOR && (w->vis[ENT_INDEX(e)].color >> 24) == DOOR_ALPHA_WINDOW;
}

static bool is_railing(const World *w, EntityId e) { return !strcmp(w->names[ENT_INDEX(e)], "lean_over"); }

bool ent_is_lean_spot(const World *w, EntityId e) {
    return ent_valid(w, e) && w->type[ENT_INDEX(e)] == ET_MARKER && (!strcmp(w->names[ENT_INDEX(e)], "lean") || is_railing(w, e));
}

EntityId lean_spot_window(const World *w, EntityId spot) {
    if (!ent_is_lean_spot(w, spot) || is_railing(w, spot)) return ENT_NONE;
    vec3 p = w->xf[ENT_INDEX(spot)].pos;
    EntityId best = ENT_NONE;
    float bd = 1.3f;   // the sash swings out of the opening: its centre moves ~1 m
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_DOOR || (w->vis[i].color >> 24) != DOOR_ALPHA_WINDOW) continue;
        float d = v3_dist(w->door[i].closed_pos.x != 0 || w->door[i].closed_pos.z != 0 ? w->door[i].closed_pos : w->xf[i].pos, p);
        if (d < bd) { bd = d; best = ENT_MAKE(i, w->gen[i]); }
    }
    return best;
}

bool lean_spot_open(const World *w, EntityId spot) {
    if (ent_is_lean_spot(w, spot) && is_railing(w, spot)) return true;
    EntityId win = lean_spot_window(w, spot);
    return win && (w->flags[ENT_INDEX(win)] & EF_OPEN) && w->anim[ENT_INDEX(win)] > 0.6f;
}

static void lean_focus(World *w, EntityId self, int player, char *prompt, int size) {
    DG_UNUSED(player);
    if (is_railing(w, self)) { snprintf(prompt, (size_t)size, "Lean Over"); return; }
    EntityId win = lean_spot_window(w, self);
    if (!win) { snprintf(prompt, (size_t)size, "Window"); return; }
    snprintf(prompt, (size_t)size, lean_spot_open(w, self) ? "Lean Out" : (w->flags[ENT_INDEX(win)] & EF_OPEN) ? "Close Window" : "Open Window");
}

// Host: a closed window opens; an open one is leaned out of (the client's camera, game_lean.c), which
// changes nothing here.
static void lean_use(World *w, EntityId self, int player) {
    EntityId win = lean_spot_window(w, self);
    if (win && !lean_spot_open(w, self)) door_use(w, win, player);
}

static const InteractDef k_lean_def = { "lean", lean_focus, NULL, lean_use, NULL, NULL, 0.0f, HANDS_RIGHT };

EntityId spawn_lean_spot(World *w, vec3 pos, float yaw, bool railing) {
    EntityId e = ent_spawn(w, ET_MARKER);
    int i = (int)ENT_INDEX(e);
    dg_strcpy(w->names[i], sizeof(w->names[0]), railing ? "lean_over" : "lean");   // entity_setup gives it the definition (replicas too)
    w->vis[i].shape = VIS_NONE;
    w->vis[i].half = v3(0.12f, 0.12f, 0.02f);
    w->comps[i] |= C_VISUAL;
    ent_set_pos(w, e, pos, quat_yaw(yaw), true);
    entity_setup(w, e);
    return e;
}

EntityId spawn_gate(World *w, vec3 base, float yaw, float width, float height, float lift, u32 color) {
    EntityId e = ent_spawn(w, ET_GATE);
    int i = (int)ENT_INDEX(e);
    quat rot = quat_yaw(yaw);
    vec3 center = v3_add(base, v3(0, height * 0.5f, 0));
    w->vis[i].shape = VIS_BOX;
    w->vis[i].half = v3(width * 0.5f, height * 0.5f, 0.08f);
    w->vis[i].color = color;
    w->comps[i] |= C_VISUAL | C_DOOR;
    Door *d = &w->door[i];
    d->closed_pos = center;
    d->closed_rot = rot;
    d->sliding = true;
    d->open_angle = lift;
    d->speed = 0.5f;
    ent_set_pos(w, e, center, rot, true);
    entity_setup(w, e);
    return e;
}

// ------------------------------------------------------------------ lever

// Handle pivot is 0.35 m above the base, the handle is 0.45 m long and rotates around local X.
static vec3 lever_handle_tip(const World *w, EntityId e, float progress) {
    float ang = lerpf(-0.75f, 0.75f, smoothstepf(0, 1, progress));
    vec3 tip = v3(0, 0.35f + cosf(ang) * 0.45f, -sinf(ang) * 0.45f);
    return local_to_world(w, e, tip);
}

static void lever_focus(World *w, EntityId self, int player, char *prompt, int size) {
    int i = (int)ENT_INDEX(self);
    snprintf(prompt, (size_t)size, (w->flags[i] & EF_ON) ? "Pull Lever (close gate)" : "Pull Lever (open gate)");
}

static void lever_use(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
    w->flags[i] ^= EF_ON;
    bool on = (w->flags[i] & EF_ON) != 0;
    Lever *l = &w->lever[i];
    if (!ent_valid(w, l->target)) l->target = ent_find_by_name(w, l->target_name);
    if (ent_valid(w, l->target)) {
        int t = (int)ENT_INDEX(l->target);
        w->door[t].target = on ? 1.0f : 0.0f;
        ev_sound(w, "gate", w->xf[t].pos, 1.0f, EVENT_ALL);
    }
    ev_sound(w, "lever", w->xf[i].pos, 1.0f, EVENT_ALL);
}

static bool lever_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    (void)hand;   // either hand (the free one when holding an item)
    int i = (int)ENT_INDEX(self);
    *pos = lever_handle_tip(w, self, w->anim[i]);
    *rot = quat_mul(w->xf[i].rot, quat_axis_angle(v3(1, 0, 0), -0.5f));
    return true;
}

static const InteractDef k_lever_def = { "lever", lever_focus, NULL, lever_use, NULL, lever_hand, 0.0f, HANDS_RIGHT };

EntityId spawn_lever(World *w, vec3 base, float yaw, const char *target) {
    EntityId e = ent_spawn(w, ET_LEVER);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = VIS_BOX;
    w->vis[i].half = v3(0.15f, 0.2f, 0.15f);
    w->vis[i].color = col(0.35f, 0.35f, 0.38f);
    w->comps[i] |= C_VISUAL | C_LEVER | C_INTERACT;
    dg_strcpy(w->lever[i].target_name, sizeof(w->lever[i].target_name), target ? target : "");
    ent_set_pos(w, e, v3_add(base, v3(0, 0.2f, 0)), quat_yaw(yaw), true);
    entity_setup(w, e);
    return e;
}

// ------------------------------------------------------------------ switch + lights

// A switch with a "preset" (level JSON, kept in target_name) jumps to a shadow reference condition
// (shadow_preset, docs/SHADOWS.md) instead of toggling lights; nothing else runs from level data.
static void switch_focus(World *w, EntityId self, int player, char *prompt, int size) {
    int i = (int)ENT_INDEX(self);
    if (w->lever[i].target_name[0]) { snprintf(prompt, (size_t)size, "Shadows: %s", w->lever[i].target_name); return; }
    snprintf(prompt, (size_t)size, (w->flags[i] & EF_ON) ? "Turn Lights Off" : "Turn Lights On");
}

static void switch_use(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
    if (w->lever[i].target_name[0]) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "shadow_preset %s", w->lever[i].target_name);
        if (w->authority) cbuf_add(cmd);
        ev_sound(w, "click", w->xf[i].pos, 0.8f, EVENT_ALL);
        return;
    }
    w->flags[i] ^= EF_ON;
    bool on = (w->flags[i] & EF_ON) != 0;
    u32 group = w->lever[i].light_group;
    ENT_FOREACH(w, k) {
        if (w->type[k] == ET_LIGHT && w->light[k].group == group) {
            if (on) w->flags[k] |= EF_ON;
            else w->flags[k] &= ~(u32)EF_ON;
        }
    }
    ev_sound(w, "click", w->xf[i].pos, 0.8f, EVENT_ALL);
}

static bool switch_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    (void)hand;   // either hand (the free one when holding an item)
    int i = (int)ENT_INDEX(self);
    // Finger tip presses the rocker on the plate's front face (+Z local).
    float rocker = (w->flags[i] & EF_ON) ? 0.025f : -0.025f;
    *pos = local_to_world(w, self, v3(0, rocker, 0.04f));
    *rot = quat_mul(w->xf[i].rot, quat_axis_angle(v3(0, 1, 0), DG_PI));
    return true;
}

static const InteractDef k_switch_def = { "switch", switch_focus, NULL, switch_use, NULL, switch_hand, 0.0f, HANDS_POINT };

EntityId spawn_switch(World *w, vec3 pos, float yaw, u32 group) {
    EntityId e = ent_spawn(w, ET_SWITCH);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = VIS_BOX;
    w->vis[i].half = v3(0.06f, 0.09f, 0.015f);
    w->vis[i].color = col(0.9f, 0.88f, 0.8f);
    w->comps[i] |= C_VISUAL | C_LEVER | C_INTERACT;
    w->lever[i].light_group = group;
    w->flags[i] |= EF_ON;
    ent_set_pos(w, e, pos, quat_yaw(yaw), true);
    entity_setup(w, e);
    return e;
}

EntityId spawn_light(World *w, vec3 pos, vec3 color, float radius, float intensity, u32 group, bool on) {
    EntityId e = ent_spawn(w, ET_LIGHT);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = VIS_BOX;
    w->vis[i].half = v3(0.3f, 0.04f, 0.3f);
    w->vis[i].color = rgba8(color.x, color.y, color.z, 1);
    w->comps[i] |= C_VISUAL | C_LIGHT;
    w->light[i].color = color;
    w->light[i].radius = radius;
    w->light[i].intensity = intensity;
    w->light[i].group = group;
    if (on) w->flags[i] |= EF_ON;
    ent_set_pos(w, e, pos, quat_identity(), true);
    return e;
}

// ------------------------------------------------------------------ container

static void container_focus(World *w, EntityId self, int player, char *prompt, int size) {
    int i = (int)ENT_INDEX(self);
    int items = 0;
    for (int s = 0; s < CONTAINER_SLOTS; s++) if (w->cont[i].slots[s].item) items++;
    if (w->cont[i].opened) snprintf(prompt, (size_t)size, "Search Crate (%d items)", items);
    else snprintf(prompt, (size_t)size, "Hold to Open Crate");
}

static void container_send(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_CONTAINER;
    ev.target = (u8)player;
    ev.except = EVENT_ALL;
    ev.ent = self;
    memcpy(ev.data, w->cont[i].slots, sizeof(w->cont[i].slots));
    ev_push(w, &ev);
    w->players[player].open_container = self;
}

static void container_use(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
    if (!w->cont[i].opened) {
        w->cont[i].opened = true;
        w->flags[i] |= EF_OPEN;
        ev_sound(w, "crate_open", w->xf[i].pos, 1.0f, EVENT_ALL);
    }
    container_send(w, self, player);
}

static void container_hold(World *w, EntityId self, int player, float progress) {
    int i = (int)ENT_INDEX(self);
    if (w->cont[i].opened) return;
    // Lid creaks up while being pried open; drops back if cancelled.
    w->cont[i].lid = progress < 0 ? 0.0f : progress * 0.25f;
}

static bool container_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    int i = (int)ENT_INDEX(self);
    // Both hands on the front edge of the lid; the lid hinges at the back edge.
    vec3 h = w->vis[i].half;
    float lid_ang = w->anim[i] * 1.6f;
    float side = hand == 0 ? -0.22f : 0.22f;
    // Front edge relative to the back hinge (y = top, z = -h.z), rotated up by the lid angle.
    float dz = 2.0f * h.z;
    vec3 local = v3(side, h.y + sinf(lid_ang) * dz + 0.02f, -h.z + cosf(lid_ang) * dz);
    // Hand reaches from the player side; pick the face nearest to the player.
    vec3 to_player = v3_sub(player_feet(w, player), w->xf[i].pos);
    vec3 n = quat_rotate(w->xf[i].rot, v3(0, 0, 1));
    if (v3_dot(to_player, n) < 0) { local.z = -local.z; local.x = -local.x; }
    *pos = local_to_world(w, self, local);
    *rot = quat_mul(w->xf[i].rot, quat_axis_angle(v3(1, 0, 0), -1.2f + lid_ang * 0.5f));
    return true;
}

static const InteractDef k_container_def = { "container", container_focus, NULL, container_use, container_hold, container_hand, 1.0f, HANDS_BOTH };

EntityId spawn_container(World *w, vec3 base, float yaw) { return spawn_container_ex(w, base, yaw, v3(0.5f, 0.3f, 0.35f), true); }

// visible = false: furniture is the visual (building kit pieces, docs/BUILDING_KIT.md); half = the collider.
EntityId spawn_container_ex(World *w, vec3 base, float yaw, vec3 half, bool visible) {
    EntityId e = ent_spawn(w, ET_CONTAINER);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = visible ? VIS_BOX : VIS_NONE;
    w->vis[i].half = half;
    w->vis[i].color = col(0.45f, 0.32f, 0.18f);
    w->comps[i] |= C_VISUAL | C_CONTAINER | C_INTERACT;
    ent_set_pos(w, e, v3_add(base, v3(0, half.y, 0)), quat_yaw(yaw), true);
    entity_setup(w, e);
    return e;
}

// ------------------------------------------------------------------ pickup

static void pickup_focus(World *w, EntityId self, int player, char *prompt, int size) {
    int i = (int)ENT_INDEX(self);
    const ItemDef *d = item_def(w->items, w->pickup[i].item);
    if (w->pickup[i].count > 1) snprintf(prompt, (size_t)size, "Pick up %s (%d)", d->name, w->pickup[i].count);
    else snprintf(prompt, (size_t)size, "Pick up %s", d->name);
}

static void pickup_use(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
    Pickup *pk = &w->pickup[i];
    Player *p = &w->players[player];
    const ItemDef *d = item_def(w->items, pk->item);
    int left = inv_add_att(&p->inv, w->items, pk->item, pk->count, pk->ammo, pk->att);
    int taken = pk->count - left;
    if (taken <= 0) { ev_hint(w, player, "Inventory full"); return; }
    if (taken > 1) ev_hint(w, player, "Picked up %s x%d", d->name, taken);
    else ev_hint(w, player, "Picked up %s", d->name);
    ev_sound(w, "pickup", w->xf[i].pos, 0.8f, EVENT_ALL);
    if (left > 0) { pk->count = (u16)left; return; }
    if (pk->respawn_time > 0) {
        pk->respawn_timer = pk->respawn_time;
        w->flags[i] |= EF_HIDDEN;
        w->inter[i].enabled = false;
    } else {
        ent_destroy(w, self);
    }
}

static bool pickup_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    (void)hand;   // either hand (the free one when holding an item)
    int i = (int)ENT_INDEX(self);
    *pos = v3_add(ent_focus_point(w, self), v3(0, w->vis[i].half.y * 0.5f + 0.02f, 0));
    *rot = quat_mul(quat_yaw(w->players[player].sim.move.yaw), quat_axis_angle(v3(1, 0, 0), -1.2f));
    return true;
}

static const InteractDef k_pickup_def = { "pickup", pickup_focus, NULL, pickup_use, NULL, pickup_hand, 0.0f, HANDS_RIGHT };

// Items on the ground use the model held in the hands, lying flat (guns on their side) at a yaw
// derived from the spawn point, resting on the surface below: the lowest point of the rotated model
// touches it. Dropped / spawned items above it fall there (pickups_tick).
static quat pickup_rest_rot(const World *w, u16 item, vec3 pos) {
    u32 h = (u32)(pos.x * 73.0f) * 2654435761u ^ (u32)(pos.z * 91.0f) * 40503u ^ item * 2246822519u;
    float yaw = (float)(h % 6283u) * 0.001f;
    quat q = quat_yaw(yaw);
    if (item_weapon(w->items, item)) q = quat_mul(q, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f));   // on its side
    return q;
}

static float pickup_rest_y(World *w, u16 item, vec3 pos, quat rot) {
    vec3 mn, mx;
    if (!item_model_bounds(w->items, item, &mn, &mx)) { mn = v3s(-0.05f); mx = v3s(0.05f); }
    float low = 1e9f;
    for (int k = 0; k < 8; k++) {
        vec3 c = v3((k & 1) ? mx.x : mn.x, (k & 2) ? mx.y : mn.y, (k & 4) ? mx.z : mn.z);
        low = fminf(low, quat_rotate(rot, c).y);
    }
    // The surface below the item's footprint centre (floor, table, crate ...).
    vec3 mid = quat_rotate(rot, v3_scale(v3_add(mn, mx), 0.5f));
    RayHit rh;
    vec3 from = v3(pos.x + mid.x, pos.y + 0.3f, pos.z + mid.z);
    float ground = phys_raycast(w->phys, from, v3(0, -20.0f, 0), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh) ? rh.point.y : pos.y - 0.5f;
    return ground - low + 0.002f;
}

EntityId spawn_pickup(World *w, vec3 pos, u16 item, int count, i16 ammo, float respawn) {
    EntityId e = ent_spawn(w, ET_PICKUP);
    int i = (int)ENT_INDEX(e);
    const ItemDef *d = item_def(w->items, item);
    vec3 mn, mx;
    if (!item_model_bounds(w->items, item, &mn, &mx)) { mn = v3_neg(d->size); mx = d->size; }
    w->vis[i].shape = (u8)VIS_BOX;
    w->vis[i].half = v3_scale(v3_sub(mx, mn), 0.5f);   // interaction tolerance / hand target
    w->vis[i].color = rgba8v(d->color);
    w->vis[i].emissive = 0;
    w->comps[i] |= C_VISUAL | C_PICKUP | C_INTERACT;
    w->pickup[i].item = item;
    w->pickup[i].count = (u16)DG_MAX(1, count);
    w->pickup[i].ammo = ammo;
    attach_defaults(item_weapon(w->items, item), w->pickup[i].att);   // spawned weapons: default attachments
    memcpy(w->attach[i], w->pickup[i].att, sizeof(w->attach[i]));
    w->pickup[i].respawn_time = respawn;
    w->pickup[i].home = pos;
    w->item[i] = item;
    quat rot = pickup_rest_rot(w, item, pos);
    w->pickup[i].rest_y = pickup_rest_y(w, item, pos, rot);
    w->pickup[i].fall_vel = 0;
    w->pickup[i].settled = pos.y <= w->pickup[i].rest_y;
    if (w->pickup[i].settled) pos.y = w->pickup[i].rest_y;
    ent_set_pos(w, e, pos, rot, true);
    entity_setup(w, e);
    w->inter[i].focus_offset = v3_scale(v3_add(mn, mx), 0.5f);   // the model's centre (item space)
    return e;
}

// ------------------------------------------------------------------ props, dummies, markers

EntityId spawn_prop(World *w, vec3 pos, quat rot, VisShape shape, vec3 half, u32 color, float mass) {
    EntityId e = ent_spawn(w, ET_PROP);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = (u8)shape;
    w->vis[i].half = half;
    w->vis[i].color = color;
    w->comps[i] |= C_VISUAL;
    w->body[i].mass = mass;
    ent_set_pos(w, e, pos, rot, true);
    entity_setup(w, e);
    return e;
}

EntityId spawn_dummy(World *w, vec3 feet, float yaw, float hp) {
    EntityId e = ent_spawn(w, ET_DUMMY);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = VIS_CAPSULE;
    w->vis[i].half = v3(0.3f, 0.6f, 0.3f);
    w->vis[i].color = col(0.75f, 0.6f, 0.35f);
    w->comps[i] |= C_VISUAL | C_HEALTH | C_HITBOX;
    health_init(&w->health[i], hp, 0);
    w->health[i].regen_delay = 3.0f;
    // Capsule center sits 0.9 m above the feet.
    ent_set_pos(w, e, v3_add(feet, v3(0, 0.9f, 0)), quat_yaw(yaw), true);
    entity_setup(w, e);
    return e;
}

EntityId spawn_marker(World *w, vec3 pos, quat rot, VisShape shape, vec3 half, u32 color, bool solid) {
    EntityId e = ent_spawn(w, ET_MARKER);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = (u8)shape;
    w->vis[i].half = half;
    w->vis[i].color = color;
    w->comps[i] |= C_VISUAL;
    if (solid) w->flags[i] |= EF_STATIC_BODY;
    ent_set_pos(w, e, pos, rot, true);
    entity_setup(w, e);
    return e;
}

// ------------------------------------------------------------------ weapon bench

// Weapon bench (dev room): opens the attachment UI for the equipped weapon. Changes go through
// RPC_ATTACH (validated by the host, game_sv_attach).
static void bench_focus(World *w, EntityId self, int player, char *prompt, int size) {
    (void)w;
    (void)self;
    (void)player;
    snprintf(prompt, (size_t)size, "Use Weapon Bench (attachments)");
}

static void bench_use(World *w, EntityId self, int player) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_BENCH;
    ev.target = (u8)player;
    ev.except = EVENT_ALL;
    ev.ent = self;
    ev_push(w, &ev);
}

static const InteractDef k_bench_def = { "bench", bench_focus, NULL, bench_use, NULL, NULL, 0.0f, HANDS_RIGHT };

EntityId spawn_bench(World *w, vec3 pos, float yaw) {
    EntityId e = ent_spawn(w, ET_BENCH);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = (u8)VIS_BOX;
    w->vis[i].half = v3(0.6f, 0.15f, 0.35f);   // interaction volume on the bench top (not drawn: the bench is a brush)
    ent_set_pos(w, e, pos, quat_yaw(yaw), true);
    entity_setup(w, e);
    return e;
}

// ------------------------------------------------------------------ setup / tick

// Interactions defined outside this file (vehicles, fuel pumps: src/game/game_vehicles.c).
static const InteractDef *g_extra_defs[ET_COUNT];
void entities_set_interact_def(EntityType t, const InteractDef *def) {
    if (t > ET_NONE && t < ET_COUNT) g_extra_defs[t] = def;
}

const InteractDef *interact_def_for(EntityType t) {
    if (t > ET_NONE && t < ET_COUNT && g_extra_defs[t]) return g_extra_defs[t];
    switch (t) {
    case ET_DOOR: return &k_door_def;
    case ET_LEVER: return &k_lever_def;
    case ET_SWITCH: return &k_switch_def;
    case ET_CONTAINER: return &k_container_def;
    case ET_PICKUP: return &k_pickup_def;
    case ET_BENCH: return &k_bench_def;
    default: return NULL;
    }
}

void entity_setup(World *w, EntityId e) {
    int i = (int)ENT_INDEX(e);
    EntityType t = (EntityType)w->type[i];
    const InteractDef *def = interact_def_for(t);
    if (def) {
        w->comps[i] |= C_INTERACT;
        w->inter[i].def = def;
        // Short ranges keep interactions within (stretched) arm's reach for the hand IK.
        switch (t) {
        case ET_SWITCH: w->inter[i].range = 1.9f; break;
        case ET_DOOR: case ET_LEVER: w->inter[i].range = 2.1f; break;
        default: w->inter[i].range = 2.3f; break;
        }
        w->inter[i].enabled = !(w->flags[i] & EF_HIDDEN);
        if (t == ET_DOOR) w->inter[i].focus_offset = v3(w->vis[i].half.x - 0.09f, -w->vis[i].half.y + 1.0f, 0);
        if (t == ET_DOOR && (w->vis[i].color >> 24) == DOOR_ALPHA_WINDOW) w->inter[i].focus_offset = v3(w->vis[i].half.x - 0.07f, 0, 0);
        if (t == ET_LEVER) w->inter[i].focus_offset = v3(0, 0.6f, 0);
    }
    if (t == ET_MARKER && (!strcmp(w->names[i], "lean") || !strcmp(w->names[i], "lean_over"))) {
        w->comps[i] |= C_INTERACT;
        w->inter[i].def = &k_lean_def;
        w->inter[i].range = 1.7f;
        w->inter[i].enabled = true;
        w->inter[i].focus_offset = v3(0, 0, 0);
    }
    if (t == ET_CORPSE || t == ET_FABRICATOR) revive_entity_setup(w, e);   // control column body, ranges, focus points
    bool replica = !w->authority;
    switch (t) {
    case ET_DOOR:
    case ET_GATE:
        ent_create_body(w, e, PM_KINEMATIC, PL_MOVING, 0);
        break;
    case ET_PROP:
        // Host simulates props; replicas use kinematic proxies driven by snapshots.
        ent_create_body(w, e, replica ? PM_KINEMATIC : PM_DYNAMIC, PL_MOVING, w->body[i].mass > 0 ? w->body[i].mass : 20.0f);
        break;
    case ET_CONTAINER:
    case ET_LEVER:
        ent_create_body(w, e, PM_STATIC, PL_STATIC, 0);
        break;
    case ET_DUMMY:
        w->comps[i] |= C_HITBOX;
        w->hitbox[i].radius = 0.3f;
        w->hitbox[i].height = 1.8f;
        w->hitbox[i].head_radius = 0.16f;
        w->hitbox[i].head_height = 1.6f;
        ent_create_body(w, e, PM_KINEMATIC, PL_CHARACTER, 0);
        break;
    case ET_MARKER:
        if (w->flags[i] & EF_STATIC_BODY) ent_create_body(w, e, PM_STATIC, PL_STATIC, 0);
        break;
    case ET_FUEL_PUMP:
        ent_create_body(w, e, PM_STATIC, PL_STATIC, 0);
        break;
    case ET_PLAYER:
        w->comps[i] |= C_HITBOX;
        w->hitbox[i].radius = PLAYER_RADIUS;
        w->hitbox[i].height = PLAYER_HEIGHT;
        w->hitbox[i].head_radius = 0.15f;
        w->hitbox[i].head_height = 1.62f;
        break;
    case ET_ZOMBIE: {
        w->comps[i] |= C_HITBOX | C_HEALTH;
        w->hitbox[i].radius = 0.3f;
        w->hitbox[i].height = 1.8f;
        w->hitbox[i].head_radius = 0.15f;
        w->hitbox[i].head_height = 1.62f;
        // Kinematic capsule so players bump into zombies (moved by the host AI).
        BodyDesc d;
        memset(&d, 0, sizeof(d));
        d.shape.kind = SHAPE_CAPSULE;
        d.shape.radius = 0.28f;
        d.shape.half_height = 0.6f;
        d.pos = v3_add(w->xf[i].pos, v3(0, 0.9f, 0));
        d.rot = quat_identity();
        d.motion = PM_KINEMATIC;
        d.layer = PL_CHARACTER;
        d.user = e;
        w->body[i].body = phys_add_body(w->phys, &d);
        w->body[i].motion = PM_KINEMATIC;
        w->comps[i] |= C_BODY;
        break;
    }
    default: break;
    }
    // Skinned characters on the shared skeleton.
    if (t == ET_PLAYER || t == ET_DUMMY || t == ET_ZOMBIE) {
        u8 id = w->char_id[i];
        if (!id && g_engine.anim) {
            const CharacterDef *cd = anim_lib_char_by_id(g_engine.anim, t == ET_PLAYER ? "player" : (t == ET_DUMMY ? "target_dummy" : "zombie_placeholder"));
            if (cd) id = cd->net_id;
        }
        if (id) char_attach(w, e, id);
    }
    if (t == ET_CORPSE) chars_attach_corpse(w, e, w->item[i] < MAX_PLAYERS ? (int)w->item[i] : -1);
}

void entities_link(World *w) {
    ENT_FOREACH(w, i) {
        if (w->type[i] == ET_LEVER && w->lever[i].target_name[0]) w->lever[i].target = ent_find_by_name(w, w->lever[i].target_name);
    }
}

void entities_tick(World *w, float dt) {
    ENT_FOREACH(w, i) {
        EntityId e = ENT_MAKE(i, w->gen[i]);
        switch (w->type[i]) {
        case ET_DOOR:
        case ET_GATE: {
            Door *d = &w->door[i];
            float prev = d->progress;
            float mult = d->speed_mult > 1.0f ? d->speed_mult : 1.0f;
            d->progress = approachf(d->progress, d->target, d->speed * mult * dt);
            if (d->progress == d->target) d->speed_mult = 1.0f;
            w->anim[i] = d->progress;
            if (prev != d->progress || w->tick < 2) {
                vec3 pos;
                quat rot;
                door_pose(w, i, d->progress, &pos, &rot);
                w->xf[i].pos = pos;
                w->xf[i].rot = rot;
                if (w->body[i].body != PHYS_INVALID_BODY) phys_move_kinematic(w->phys, w->body[i].body, pos, rot, dt);
            } else if (w->body[i].body != PHYS_INVALID_BODY && phys_is_active(w->phys, w->body[i].body)) {
                phys_set_velocity(w->phys, w->body[i].body, v3(0, 0, 0), v3(0, 0, 0));
            }
            break;
        }
        case ET_LEVER:
        case ET_SWITCH:
            w->anim[i] = approachf(w->anim[i], (w->flags[i] & EF_ON) ? 1.0f : 0.0f, dt * 4.0f);
            break;
        case ET_CONTAINER: {
            Container *c = &w->cont[i];
            float target = c->opened ? 1.0f : c->lid;
            w->anim[i] = approachf(w->anim[i], target, dt * (c->opened ? 2.5f : 4.0f));
            break;
        }
        case ET_PICKUP: {
            Pickup *pk = &w->pickup[i];
            if (pk->respawn_timer > 0) {
                pk->respawn_timer -= dt;
                if (pk->respawn_timer <= 0) {
                    w->flags[i] &= ~(u32)EF_HIDDEN;
                    w->inter[i].enabled = true;
                    vec3 home = v3(pk->home.x, fmaxf(pk->home.y, pk->rest_y), pk->home.z);
                    ent_set_pos(w, e, home, w->xf[i].rot, true);
                    pk->settled = home.y <= pk->rest_y;
                    pk->fall_vel = 0;
                }
            }
            // Falling onto the surface below (host; positions replicate like any entity).
            if (!pk->settled) {
                vec3 p = w->xf[i].pos;
                pk->fall_vel -= 9.81f * dt;
                p.y += pk->fall_vel * dt;
                if (p.y <= pk->rest_y) { p.y = pk->rest_y; pk->settled = true; pk->fall_vel = 0; }
                ent_set_pos(w, e, p, w->xf[i].rot, false);
            }
            break;
        }
        case ET_PROP:
            ent_sync_from_body(w, e);
            // Fell out of the world: put it back.
            if (w->xf[i].pos.y < -50.0f) ent_set_pos(w, e, v3(0, 3, 0), quat_identity(), true);
            break;
        case ET_DUMMY:
            health_tick_dummy(w, e, dt);
            break;
        default: break;
        }
    }
}

void entities_client_sync(World *w) {
    ENT_FOREACH(w, i) {
        BodyId b = w->body[i].body;
        if (b == PHYS_INVALID_BODY || w->body[i].motion == PM_STATIC) continue;
        // Player / zombie proxies are capsules centered at half height; entity positions are the feet.
        bool feet = w->type[i] == ET_PLAYER || w->type[i] == ET_ZOMBIE;
        vec3 pos = feet ? v3_add(w->xf[i].pos, v3(0, w->type[i] == ET_PLAYER ? PLAYER_HEIGHT * 0.5f : 0.9f, 0)) : w->xf[i].pos;
        quat rot = feet ? quat_identity() : w->xf[i].rot;
        phys_set_transform(w->phys, b, pos, rot, false);
    }
}
