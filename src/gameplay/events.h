// Gameplay events: produced by the authoritative simulation, delivered to players by the
// net layer (reliable), or handled directly for the host's local player.
#ifndef DG_EVENTS_H
#define DG_EVENTS_H

#include "core/base.h"
#include "core/dgmath.h"

#define EVENT_ALL 0xFF
#define EVENT_DATA_SIZE 144   // container contents: CONTAINER_SLOTS x InvSlot
#define MAX_PENDING_EVENTS 512

typedef enum EventType {
    EV_NONE,
    EV_HINT,           // text message for a player (text)
    EV_SOUND,          // id = sound name hash, pos, value = volume
    EV_DAMAGE_NUMBER,  // pos, value = amount, ent = target, id = damage type
    EV_HIT_CONFIRM,    // to attacker: value = damage, id = 1 if kill, 2 if headshot
    EV_HURT,           // to victim: dir = from attacker, value = damage
    EV_IMPACT,         // pos, dir = normal, id = surface kind
    EV_TRACER,         // pos = from, dir = to
    EV_MUZZLE,         // ent = shooter
    EV_KILL,           // ent = victim, ent2 = killer, text = message
    EV_CONTAINER,      // ent = container, data = InvSlot[CONTAINER_SLOTS]
    EV_CONTAINER_CLOSE,
    EV_RESPAWN,        // ent = player entity
    EV_USE_ANIM,       // ent = user player entity, ent2 = target (for remote hand animation)
    EV_CHAT,           // text
    EV_BENCH,          // to the user: ent = weapon bench (opens the attachment UI)
    EV_TOD,            // time of day: id = TodEvent bits (sunrise / sunset / midnight / noon), value = hours
} EventType;

typedef struct GameEvent {
    u8 type;
    u8 target;   // player slot or EVENT_ALL
    u8 except;   // player slot to skip (predicted locally) or EVENT_ALL for none
    u8 pad;
    u32 ent, ent2;
    u32 id;
    float value;
    vec3 pos, dir;
    char text[64];
    u8 data[EVENT_DATA_SIZE];
} GameEvent;

struct World;
// Queues an event on the authoritative world (no-op on replicas).
void ev_push(struct World *w, const GameEvent *ev);
void ev_hint(struct World *w, int player, const char *fmt, ...);
void ev_sound(struct World *w, const char *name, vec3 pos, float volume, int except);

#endif
