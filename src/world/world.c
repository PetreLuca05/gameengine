#include "vfx/vfx_types.h"
#include "world.h"
#include "gameplay/characters.h"
#include "core/log.h"
#include <stdlib.h>

static const char *k_type_names[ET_COUNT] = { "none", "player", "prop", "door", "gate", "lever", "switch", "light",
                                              "container", "pickup", "dummy", "projectile", "marker", "zombie" };

const char *ent_type_name(EntityType t) { return (t >= 0 && t < ET_COUNT) ? k_type_names[t] : "?"; }

World *world_create(bool authority, const ItemDB *items) {
    World *w = (World *)calloc(1, sizeof(World));
    if (!w) { log_fatal("world: out of memory"); return NULL; }
    w->authority = authority;
    w->items = items;
    w->phys = phys_world_create();
    w->max_used = 1;
    chars_system_create(w);
    for (int i = 0; i < MAX_PLAYERS; i++) w->players[i].peer = -1;
    return w;
}

void world_clear(World *w) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &w->players[i];
        if (p->character) phys_character_destroy(w->phys, p->character);
        p->character = NULL;
    }
    for (int i = 1; i < w->max_used; i++)
        if (w->alive[i]) ent_destroy(w, ENT_MAKE(i, w->gen[i]));
    for (int i = 0; i < w->level.body_count; i++) phys_remove_body(w->phys, w->level.bodies[i]);
    for (int i = 0; i < w->level.draw_count; i++) render_mesh_destroy(w->level.draws[i].mesh);
    memset(&w->level, 0, sizeof(w->level));
}

void world_destroy(World *w) {
    if (!w) return;
    world_clear(w);
    chars_system_destroy(w);
    phys_world_destroy(w->phys);
    free(w);
}

static void reset_slot(World *w, int i, EntityType type) {
    w->alive[i] = 1;
    w->type[i] = (u8)type;
    w->flags[i] = 0;
    w->comps[i] = 0;
    w->names[i][0] = 0;
    w->anim[i] = w->anim_prev[i] = 0;
    w->item[i] = 0;
    memset(&w->xf[i], 0, sizeof(Transform));
    w->xf[i].rot = w->xf[i].prev_rot = quat_identity();
    memset(&w->vis[i], 0, sizeof(Visual));
    memset(&w->body[i], 0, sizeof(BodyComp));
    w->body[i].body = PHYS_INVALID_BODY;
    memset(&w->health[i], 0, sizeof(Health));
    memset(&w->inter[i], 0, sizeof(Interactable));
    memset(&w->door[i], 0, sizeof(Door));
    memset(&w->lever[i], 0, sizeof(Lever));
    memset(&w->light[i], 0, sizeof(LightComp));
    memset(&w->cont[i], 0, sizeof(Container));
    memset(&w->pickup[i], 0, sizeof(Pickup));
    memset(&w->proj[i], 0, sizeof(Projectile));
    memset(&w->hitbox[i], 0, sizeof(Hitbox));
    w->player_slot[i] = 0xFF;
    w->char_id[i] = 0;
    w->life[i] = 0;
    for (int t = 0; t < LAGCOMP_TICKS; t++) w->lag[t][i].valid = 0;
    if (i + 1 > w->max_used) w->max_used = i + 1;
}

static EntityId spawn_in_range(World *w, EntityType type, int lo, int hi) {
    for (int i = lo; i < hi; i++) {
        if (w->alive[i]) continue;
        w->gen[i] = (u16)((w->gen[i] + 1) & 0xFFFFF);
        if (w->gen[i] == 0) w->gen[i] = 1;
        reset_slot(w, i, type);
        return ENT_MAKE(i, w->gen[i]);
    }
    log_error("world: out of entity slots");
    return ENT_NONE;
}

EntityId ent_spawn(World *w, EntityType type) { return spawn_in_range(w, type, 1, CLIENT_ENTITY_BASE); }
EntityId ent_spawn_local(World *w, EntityType type) { return spawn_in_range(w, type, CLIENT_ENTITY_BASE, MAX_ENTITIES); }

EntityId ent_spawn_at(World *w, EntityId id, EntityType type) {
    int i = (int)ENT_INDEX(id);
    if (i <= 0 || i >= MAX_ENTITIES) return ENT_NONE;
    if (w->alive[i]) ent_destroy(w, ENT_MAKE(i, w->gen[i]));
    w->gen[i] = (u16)ENT_GEN(id);
    reset_slot(w, i, type);
    return id;
}

bool ent_valid(const World *w, EntityId e) {
    u32 i = ENT_INDEX(e);
    return e != ENT_NONE && i < MAX_ENTITIES && w->alive[i] && w->gen[i] == (u16)ENT_GEN(e);
}

EntityId ent_from_index(const World *w, int i) {
    if (i <= 0 || i >= MAX_ENTITIES || !w->alive[i]) return ENT_NONE;
    return ENT_MAKE(i, w->gen[i]);
}

void ent_destroy(World *w, EntityId e) {
    if (!ent_valid(w, e)) return;
    int i = (int)ENT_INDEX(e);
    if (w->chars) char_detach(w, e);
    if (w->body[i].body != PHYS_INVALID_BODY) phys_remove_body(w->phys, w->body[i].body);
    w->body[i].body = PHYS_INVALID_BODY;
    w->alive[i] = 0;
    w->comps[i] = 0;
    while (w->max_used > 1 && !w->alive[w->max_used - 1]) w->max_used--;
}

EntityId ent_find_by_name(const World *w, const char *name) {
    if (!name || !name[0]) return ENT_NONE;
    for (int i = 1; i < w->max_used; i++)
        if (w->alive[i] && strcmp(w->names[i], name) == 0) return ENT_MAKE(i, w->gen[i]);
    return ENT_NONE;
}

void ent_set_pos(World *w, EntityId e, vec3 pos, quat rot, bool teleport) {
    int i = (int)ENT_INDEX(e);
    w->xf[i].pos = pos;
    w->xf[i].rot = rot;
    if (teleport) { w->xf[i].prev_pos = pos; w->xf[i].prev_rot = rot; }
    if (w->body[i].body != PHYS_INVALID_BODY) phys_set_transform(w->phys, w->body[i].body, pos, rot, true);
}

vec3 ent_focus_point(const World *w, EntityId e) {
    int i = (int)ENT_INDEX(e);
    return v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, w->inter[i].focus_offset));
}

// Surface material of entity bodies (impact VFX / sounds), identical on host and clients.
static u8 entity_material(const World *w, int i) {
    switch (w->type[i]) {
    case ET_PROP: return (u8)(w->vis[i].shape == VIS_CYLINDER ? SURF_METAL_THIN : (w->vis[i].shape == VIS_SPHERE ? SURF_CLOTH : SURF_WOOD));
    case ET_DOOR: return SURF_WOOD;
    case ET_GATE: return SURF_METAL_THICK;
    case ET_CONTAINER: return SURF_METAL_THIN;
    case ET_LEVER: case ET_SWITCH: case ET_LIGHT: return SURF_METAL_THIN;
    case ET_PLAYER: case ET_DUMMY: case ET_ZOMBIE: return SURF_FLESH;
    default: return SURF_DEFAULT;
    }
}

void ent_create_body(World *w, EntityId e, PhysMotion motion, PhysLayer layer, float mass) {
    int i = (int)ENT_INDEX(e);
    Visual *v = &w->vis[i];
    BodyDesc d;
    memset(&d, 0, sizeof(d));
    switch (v->shape) {
    case VIS_CYLINDER: d.shape.kind = SHAPE_CYLINDER; d.shape.radius = v->half.x; d.shape.half_height = v->half.y; break;
    case VIS_SPHERE: d.shape.kind = SHAPE_SPHERE; d.shape.radius = v->half.x; break;
    case VIS_CAPSULE: d.shape.kind = SHAPE_CAPSULE; d.shape.radius = v->half.x; d.shape.half_height = v->half.y; break;
    default: d.shape.kind = SHAPE_BOX; d.shape.half = v->half; break;
    }
    d.pos = w->xf[i].pos;
    d.rot = w->xf[i].rot;
    d.motion = motion;
    d.layer = layer;
    d.user = e;
    d.mass = mass;
    d.friction = 0.7f;
    d.restitution = 0.05f;
    d.material = entity_material(w, i);
    if (w->body[i].body != PHYS_INVALID_BODY) phys_remove_body(w->phys, w->body[i].body);
    w->body[i].body = phys_add_body(w->phys, &d);
    w->body[i].motion = motion;
    w->body[i].mass = mass;
    w->comps[i] |= C_BODY;
}

void ent_sync_from_body(World *w, EntityId e) {
    int i = (int)ENT_INDEX(e);
    if (w->body[i].body == PHYS_INVALID_BODY) return;
    phys_get_transform(w->phys, w->body[i].body, &w->xf[i].pos, &w->xf[i].rot);
}

int world_player_by_ent(const World *w, EntityId e) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (w->players[i].active && w->players[i].ent == e) return i;
    return -1;
}

void world_store_prev(World *w) {
    for (int i = 1; i < w->max_used; i++) {
        if (!w->alive[i]) continue;
        w->xf[i].prev_pos = w->xf[i].pos;
        w->xf[i].prev_rot = w->xf[i].rot;
        w->anim_prev[i] = w->anim[i];
    }
}

void world_record_lagcomp(World *w) {
    int slot = (int)(w->tick % LAGCOMP_TICKS);
    for (int i = 1; i < w->max_used; i++) {
        LagRecord *r = &w->lag[slot][i];
        r->valid = w->alive[i] && (w->comps[i] & C_HITBOX) ? 1 : 0;
        if (r->valid) {
            r->pos = w->xf[i].pos;
            r->height = w->hitbox[i].height;
        }
    }
}

bool world_lagcomp_get(const World *w, EntityId e, u32 tick, vec3 *pos) {
    if (tick > w->tick || w->tick - tick >= LAGCOMP_TICKS) return false;
    const LagRecord *r = &w->lag[tick % LAGCOMP_TICKS][ENT_INDEX(e)];
    if (!r->valid) return false;
    *pos = r->pos;
    return true;
}
