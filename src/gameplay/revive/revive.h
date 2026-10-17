// Downed / revive / corpse / DNA extraction / fabricator reconstruction: the game integration of
// the pure machines in revive_core.h (docs/REVIVE.md).
//
// Host-authoritative. Timed actions (revive, extract, insert) run inside game_sim_player on the
// host and on the predicting client alike (PlayerSim.act), so the ring starts at once and the
// reconcile settles it; the effects (healing, samples, queues, respawns) only happen on the host.
// Corpses and fabricators are replicated entities (count / extra / attach / name, always relevant),
// the DNA registry is a reliable event stream (resent to late joiners).
#ifndef DG_REVIVE_H
#define DG_REVIVE_H

#include "world/world.h"
#include "game/usercmd.h"
#include "gameplay/events.h"
#include "revive_core.h"

// apply_damage_ex flags
enum DamageFlags {
    DMGF_KILL = 1 << 0,     // kill-type: skips the downed state (kill command, falling out of the world)
};

// EV_REVIVE kinds (GameEvent.id)
typedef enum ReviveEvent {
    RVE_DOWNED = 1,         // ent = player, ent2 = attacker
    RVE_REVIVED,            // ent = player, ent2 = reviver
    RVE_BLED_OUT,           // ent = player
    RVE_EXTRACTED,          // ent = corpse, ent2 = extractor player, value = sample id
    RVE_INSERTED,           // ent = fabricator, ent2 = player, value = sample id
    RVE_RECON_START,        // ent = fabricator, value = sample id, pos = chamber
    RVE_RECON_DONE,         // ent = fabricator, ent2 = respawned player
    RVE_EJECTED,            // ent = fabricator, value = sample id
    RVE_CANCELLED,          // to the actor: value = ActCancel
} ReviveEvent;

void revive_register(void);                 // cvars + commands (once)
void revive_world_reset(World *w);          // new world / map (host and client)
const ReviveRules *revive_rules(void);
const DnaRegistry *revive_dna(void);

// ---- simulation (game_sim_player, host + predicting client + spectators)
// Before movement: the action machine, the downed restrictions (crawl, no interaction), the
// forced idle; edits the command copy the rest of the tick sees.
void revive_sim_pre(World *w, int slot, UserCmd *cmd, u16 pressed, float dt, bool first);
// The item shown in the player's hand (syringe while extracting, the vial while inserting), 0 = the equipped one.
u16 revive_held_override(const World *w, int slot);

// ---- host hooks
// 0 hp on a player: true = knocked down instead of dying (the caller keeps it alive).
bool revive_sv_try_down(World *w, int slot, u32 dmg_flags, float overkill, EntityId attacker);
void revive_sv_on_damage(World *w, int slot);   // interrupts timed actions
void revive_sv_on_death(World *w, int slot);    // corpse, carried samples, auto ping
void revive_sv_on_respawn(World *w, int slot);
bool revive_sv_timer_respawn_allowed(const World *w, int slot);
void revive_sv_player_joined(World *w, int slot);   // late join: the DNA registry
void revive_sv_player_left(World *w, int slot);
void revive_host_tick(World *w);            // corpses, fabricators, DNA carriers
// Every machine, every tick (after replication): interactables of downed players.
void revive_frame(World *w);
EntityId revive_spawn_fabricator(World *w, vec3 pos, float yaw, bool powered);
EntityId revive_spawn_corpse(World *w, int slot, vec3 pos, float yaw);
void revive_entity_setup(World *w, EntityId e);   // entity_setup: corpse / fabricator components (host + replicas)
// Down / revive a player (host; debug commands, tests).
bool revive_sv_down(World *w, int slot, EntityId attacker);
bool revive_sv_revive(World *w, int slot, int reviver);

// ---- client
void revive_handle_event(const GameEvent *ev);
// Fabricator sockets in world space: sample_slot, chamber_center, panel, spawn_point.
bool revive_fab_socket(const World *w, EntityId fab, const char *name, vec3 *pos, quat *rot);
// Corpse body axis (pelvis -> head) used for the range gate and the hand IK.
void revive_corpse_axis(const World *w, EntityId corpse, vec3 *pelvis, vec3 *head);

// What the HUD / spectator bar shows about the recovery of a dead player's body.
typedef enum RecoveryStage { REC_NONE, REC_BODY, REC_EXTRACTED, REC_QUEUED, REC_RECONSTRUCTING, REC_OPENING } RecoveryStage;
typedef struct RecoveryStatus {
    RecoveryStage stage;
    EntityId corpse, fabricator;
    vec3 corpse_pos;
    char by[32];               // extractor / carrier name
    float remaining;           // s (reconstruction countdown)
    float progress;            // 0..1
    int queue_pos;             // 1 = next
} RecoveryStatus;
bool revive_recovery_status(const World *w, int slot, RecoveryStatus *out);
// Fabricator view state from the replicated fields (any machine).
typedef struct FabView {
    FabMachine m;
    int current_player;        // slot being reconstructed, -1
    char current_name[24];
    float progress, remaining;
} FabView;
bool revive_fab_view(const World *w, EntityId fab, FabView *out);
// Display name of an inventory item: "DNA: Alex #3" for samples (who + unique id), else the item name.
// The returned string lives until 8 more calls.
const char *revive_item_name(const ItemDB *db, u16 item, i16 ammo);
// Reconstruction preview (dev room, reconstruction_preview): < 0 = off.
float revive_recon_preview(void);

#endif
