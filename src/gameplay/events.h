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
    EV_KILL,           // ent = victim, ent2 = killer, text = message, id = KILLF_* (kill_stats.h), data = KillEventData
    EV_CONTAINER,      // ent = container, data = InvSlot[CONTAINER_SLOTS]
    EV_CONTAINER_CLOSE,
    EV_RESPAWN,        // ent = player entity
    EV_USE_ANIM,       // ent = user player entity, ent2 = target (for remote hand animation)
    EV_CHAT,           // text
    EV_BENCH,          // to the user: ent = weapon bench (opens the attachment UI)
    EV_TOD,            // time of day: id = TodEvent bits (sunrise / sunset / midnight / noon), value = hours
    EV_VEHICLE,        // vehicles: id 1 = ejected at speed (ent = player, dir = velocity, value = stun s)
    EV_PING,           // pings (gameplay/ping.h): id = uid, ent = owner slot, ent2 = type, pos, value = lifetime (< 0: removed), text = owner name
    EV_EXPLOSION,      // throwables: pos, value = radius, id = item
    EV_REVIVE,         // revive loop (gameplay/revive/revive.h ReviveEvent): id = kind, ent = subject, ent2 = other, value, pos, text = names
    EV_DNA,            // DNA registry entry (gameplay/revive): id = sample id, data = DnaSample (status DNA_FREE = removed)
    EV_SHOT,           // ballistic shot: ent = shooter, ent2 = slot + 1, pos = launch point, dir = aim, data = BalShotRecord (gameplay/ballistics/bal_world.h)
    EV_STORY,          // story mode (src/story/story_net.h StoryEvKind): id = kind, text = chapter / script / sound id, ent / ent2 / value / pos / dir / data per kind
} EventType;

// EV_KILL payload (data): names as shown in the kill feed and the killer's weapon item.
typedef struct KillEventData {
    char killer[32];
    char victim[32];
    u16 weapon;
} KillEventData;

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
