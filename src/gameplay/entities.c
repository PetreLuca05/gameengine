#include "entities.h"
#include "health.h"
#include "core/log.h"
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
        const ItemDef *key = item_def(w->items, w->door[i].key_item);
        snprintf(prompt, (size_t)size, "Locked - requires %s", key->name);
        return;
    }
    snprintf(prompt, (size_t)size, w->door[i].target > 0.5f ? "Close Door" : "Open Door");
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

static bool door_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    if (hand != 1) return false; // right hand only
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
    d->knob_local = v3(width * 0.5f - 0.09f, -height * 0.5f + 1.0f, 0.07f);
    if (locked) w->flags[i] |= EF_LOCKED;
    ent_set_pos(w, e, center, rot, true);
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
    if (hand != 1) return false;
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

static void switch_focus(World *w, EntityId self, int player, char *prompt, int size) {
    int i = (int)ENT_INDEX(self);
    snprintf(prompt, (size_t)size, (w->flags[i] & EF_ON) ? "Turn Lights Off" : "Turn Lights On");
}

static void switch_use(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
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
    if (hand != 1) return false;
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

EntityId spawn_container(World *w, vec3 base, float yaw) {
    EntityId e = ent_spawn(w, ET_CONTAINER);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = VIS_BOX;
    w->vis[i].half = v3(0.5f, 0.3f, 0.35f);
    w->vis[i].color = col(0.45f, 0.32f, 0.18f);
    w->comps[i] |= C_VISUAL | C_CONTAINER | C_INTERACT;
    ent_set_pos(w, e, v3_add(base, v3(0, 0.3f, 0)), quat_yaw(yaw), true);
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
    int left = inv_add(&p->inv, w->items, pk->item, pk->count, pk->ammo);
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
    if (hand != 1) return false;
    int i = (int)ENT_INDEX(self);
    *pos = v3_add(w->xf[i].pos, v3(0, w->vis[i].half.y + 0.02f, 0));
    *rot = quat_mul(quat_yaw(w->players[player].sim.move.yaw), quat_axis_angle(v3(1, 0, 0), -1.2f));
    return true;
}

static const InteractDef k_pickup_def = { "pickup", pickup_focus, NULL, pickup_use, NULL, pickup_hand, 0.0f, HANDS_RIGHT };

EntityId spawn_pickup(World *w, vec3 pos, u16 item, int count, i16 ammo, float respawn) {
    EntityId e = ent_spawn(w, ET_PICKUP);
    int i = (int)ENT_INDEX(e);
    const ItemDef *d = item_def(w->items, item);
    w->vis[i].shape = (u8)d->shape;
    w->vis[i].half = d->size;
    w->vis[i].color = rgba8v(d->color);
    w->vis[i].emissive = 10;
    w->comps[i] |= C_VISUAL | C_PICKUP | C_INTERACT;
    w->pickup[i].item = item;
    w->pickup[i].count = (u16)DG_MAX(1, count);
    w->pickup[i].ammo = ammo;
    w->pickup[i].respawn_time = respawn;
    w->pickup[i].home = pos;
    w->item[i] = item;
    ent_set_pos(w, e, pos, quat_identity(), true);
    entity_setup(w, e);
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

// ------------------------------------------------------------------ setup / tick

const InteractDef *interact_def_for(EntityType t) {
    switch (t) {
    case ET_DOOR: return &k_door_def;
    case ET_LEVER: return &k_lever_def;
    case ET_SWITCH: return &k_switch_def;
    case ET_CONTAINER: return &k_container_def;
    case ET_PICKUP: return &k_pickup_def;
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
        w->inter[i].range = t == ET_PICKUP ? 2.2f : 2.5f;
        w->inter[i].enabled = !(w->flags[i] & EF_HIDDEN);
        if (t == ET_DOOR) w->inter[i].focus_offset = v3(w->vis[i].half.x - 0.09f, -w->vis[i].half.y + 1.0f, 0);
        if (t == ET_LEVER) w->inter[i].focus_offset = v3(0, 0.6f, 0);
    }
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
    case ET_PLAYER:
        w->comps[i] |= C_HITBOX;
        w->hitbox[i].radius = PLAYER_RADIUS;
        w->hitbox[i].height = PLAYER_HEIGHT;
        w->hitbox[i].head_radius = 0.15f;
        w->hitbox[i].head_height = 1.62f;
        break;
    default: break;
    }
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
            d->progress = approachf(d->progress, d->target, d->speed * dt);
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
                }
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
        phys_set_transform(w->phys, b, w->xf[i].pos, w->xf[i].rot, false);
    }
}
