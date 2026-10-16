// Wire protocol. Every message starts with a MsgType byte.
//
//   client -> host  reliable   HELLO{version,name,anim manifest[categories]}  RPC{type,a,b,c}
//   client -> host  unreliable CMDS{ack_snapshot, count, UserCmd[count]}   (last N cmds, redundancy)
//   host -> client  reliable   WELCOME{slot,map,seed,tick,snaprate}  REJECT{reason}  EVENT{GameEvent}
//                              INVENTORY{revision,slots}  CVARS{count,(name,value)*}
//   host -> client  unreliable SNAPSHOT{tick,baseline,ack_cmd,private player state, entity deltas, removals}
#ifndef DG_PROTOCOL_H
#define DG_PROTOCOL_H

#define DG_PROTOCOL_VERSION 10  // 10: weather in snapshots; 9: inventory intents (RPC_INV_OP, MSG_INVENTORY ack); 8: 8: weapon attachments (InvSlot.att, NF_ATTACH), time of day in snapshots
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
