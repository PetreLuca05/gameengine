// Wire protocol. Every message starts with a MsgType byte.
//
//   client -> host  reliable   HELLO{version,name,anim manifest[categories],build hash,build version,steam id,listen port}  RPC{type,a,b,c}
//   both ways       reliable   LOBBY{LobbyOp, payload}   (src/net/lobby.h: snapshots, chat, intents, start, kick, migrate)
//   client -> host  unreliable CMDS{ack_snapshot, view_tick, send_ms, count, UserCmd[count]}   (last N cmds, redundancy)
//   host -> client  reliable   WELCOME{slot,map,seed,tick,snaprate}  REJECT{reason}  EVENT{GameEvent}
//                              INVENTORY{slot,revision,ack,slots}  CVARS{count,(name,value)*}
//   host -> client  unreliable SNAPSHOT{tick,baseline,local data (own or spectated player),spectator block,
//                              world time, weather, vehicles, entity deltas, removals}
//   host -> client  unreliable SPEC_CMDS{target,count,(UserCmd,send_ms)[count]}   (spectating, docs/SPECTATE.md)
#ifndef DG_PROTOCOL_H
#define DG_PROTOCOL_H

#define DG_PROTOCOL_VERSION 30  // 30: grid inventory (docs/INVENTORY.md; branch protocol 27 on top of 29: MSG_INVENTORY = hotbar binds + tree, MSG_LOOT, MSG_INV_INTENT replaces RPC_INV_OP, EV_CONTAINER without contents, the 6 hotbar slots kept); 29: story SEVT_TITLE (the day card, from origin/main's 27) on top of 28; 28: no ricochets (bal_hit_surface draws no skip roll, so the bullet RNG stream and the replayed paths change; EV_IMPACT outcome bit 0 is free, docs/BALLISTICS.md); 27: barrel-forward aim + ledge rules (UserCmd barrel_cam: shots leave along the barrel, no convergence; PlayerMove climb_intent / regrab_point / hang_stamina / shimmy, docs/AIM.md, docs/CONTROLLER.md); 26: story mode (EV_STORY: chapter / objectives / subtitles / prompts / journal / fade / audio / markers / sync, docs/STORY.md); 25: settings audit (MSG_HELLO consistency-set hash after the animation manifest, CVAR_RULE cvars in MSG_CVARS, LobbySettings foliage_pct); 24: 6 hotbar slots (INV_HOTBAR 9 -> 6: MSG_INVENTORY / RPC_INV_OP slot indices, UserCmd slot, WPN_SLOT_PHONE); 23: UserCmd eye_cam (the aim ray starts at the first-person camera, docs/AIM.md); 22: aim model (UserCmd muzzle_cam / aim_dist; shots leave the muzzle, EV_SHOT / EV_TRACER pos = the muzzle origin); 21: water (PlayerMove water / oxygen / stamina / wet / swim phase, MOVE_SWIM, EF_SWIM / EF_UNDERWATER / EF_WADE, DMG_DROWN); 20: projectile ballistics (EV_SHOT + BalShotRecord replaces the per-pellet EV_TRACER and the shot EV_SOUND; EV_IMPACT id bits 16..31 = energy / outcome); 19: revive loop (PlayerSim down / act / frozen_ticks, EF_DOWNED / EF_DNA, ET_CORPSE / ET_FABRICATOR with count + extra, EV_REVIVE / EV_DNA, InteractDef action); 18: HUD (EV_KILL flags + KillEventData, EV_PING / RPC_PING, RPC_DOOR_ACTION, EV_EXPLOSION, throwable projectiles); 17: spectating (MSG_SPEC_CMDS, spectator block after the snapshot local data, MSG_INVENTORY slot, MSG_CMDS send_ms, UserCmd phone_zoom / BTN_CAM_THIRD); 16: lobby (HELLO build hash / version / Steam id / listen port, MSG_LOBBY); 15: hand poses (AC_HAND_POSES in the join manifest); 14: phone (UserCmd phone_rot / phone_flags, ANS_PHONE in the character state); 13: modular character parts (ANS_PARTS in the anim state, RPC_SET_PART); 12: PlayerMove.veh_raise; 11: vehicles (PlayerMove seat fields, vehicle block in snapshots, BTN_VEHICLE); 10: weather in snapshots; 9: inventory intents (RPC_INV_OP, MSG_INVENTORY ack); 8: weapon attachments (InvSlot.att, NF_ATTACH), time of day in snapshots
#define DG_DEFAULT_PORT 27015
#define DG_DISCOVERY_PORT 27016

typedef enum MsgType {
    MSG_HELLO = 1,
    MSG_WELCOME,
    MSG_REJECT,
    MSG_CMDS,
    MSG_SNAPSHOT,
    MSG_EVENT,
    MSG_INVENTORY,
    MSG_RPC,
    MSG_CVARS,
    MSG_BYE,
    MSG_ANIM_EVENTS,   // host -> client reliable: count u8, AnimEvent[count] (compact, see anim/anim_net.h)
    MSG_LOBBY,         // both ways reliable: lobby (src/net/lobby.h LobbyOp + payload)
    MSG_SPEC_CMDS,     // host -> observer unreliable: a spectated player's commands, relayed on arrival (net/spectate.h)
    MSG_LOOT,          // host -> client reliable: the open world inventory (entity, revision, open u8, tree; docs/INVENTORY.md)
    MSG_INV_INTENT,    // client -> host reliable: an inventory intent (gameplay/inventory/inv_wire.c)
} MsgType;

// Per-entity delta field bits.
enum NetFieldBits {
    NF_TYPE  = 1 << 0,   // type + player slot
    NF_FLAGS = 1 << 1,
    NF_POS   = 1 << 2,
    NF_ROT   = 1 << 3,
    NF_ANIM  = 1 << 4,
    NF_ITEM  = 1 << 5,   // item + count
    NF_HP    = 1 << 6,   // hp, max hp, armor
    NF_VIS   = 1 << 7,   // shape, half extents, color, emissive
    NF_NAME  = 1 << 8,
    NF_LIGHT = 1 << 9,   // light color/radius/intensity/group
    NF_EXTRA = 1 << 10,  // door knob / lever target data
    NF_ANIMST = 1 << 11, // replicated animation state (characters), own sub-field mask
    NF_ATTACH = 1 << 12, // weapon attachment ids (u8 per slot, own per-slot delta mask)
    NF_DPOS  = 1 << 13,  // position as an i16 x 3 delta (1/256 m) from the baseline (instead of NF_POS)
    NF_DPOS8 = 1 << 14,  // position as an i8 x 3 delta (1/64 m, moves under 2 m: walking zombies)
    NF_ALL   = 0x1FFF,
};

#define CMDS_PER_PACKET 4

#endif
