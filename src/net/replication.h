// Snapshot replication: capture relevant entity state, delta-encode against the last
// acknowledged snapshot, decode on clients, apply to the replica world, interpolate.
#ifndef DG_REPLICATION_H
#define DG_REPLICATION_H

#include "world/world.h"
#include "bitbuf.h"

#define SNAP_HISTORY 32
#define MAX_SNAP_ENTS 768
#define INTERP_SAMPLES 16

typedef struct NetEnt {
    u32 id;
    u8 type, slot, shape, emissive;
    u16 model, pad;
    u32 flags;
    vec3 pos;
    quat rot;
    float anim;
    u16 item, count;
    float hp, max_hp, armor;
    vec3 half;
    u32 color;
    char name[24];
    vec3 light_color;
    float light_radius, light_intensity;
    u32 light_group;
    vec3 extra;
} NetEnt;

typedef struct SnapFrame {
    u32 tick;
    int count;
    NetEnt *ents;     // sorted by entity index
} SnapFrame;

typedef struct SnapHistory { SnapFrame frames[SNAP_HISTORY]; } SnapHistory;

typedef struct InterpSample { u32 tick; vec3 pos; quat rot; float anim; } InterpSample;
typedef struct InterpBuffer { InterpSample s[INTERP_SAMPLES]; int head, count; u32 id; } InterpBuffer;

void snap_history_init(SnapHistory *h);
void snap_history_free(SnapHistory *h);
SnapFrame *snap_history_find(SnapHistory *h, u32 tick);
SnapFrame *snap_history_slot(SnapHistory *h, u32 tick);

// Host: gather entities relevant to a viewer.
void repl_capture(World *w, vec3 center, float radius, SnapFrame *out);
// Host: write entity deltas (current vs baseline, baseline may be NULL = full).
void repl_write_delta(Writer *wr, const SnapFrame *base, const SnapFrame *cur);
// Client: decode deltas into out (starting from baseline copy). Returns false on malformed data.
bool repl_read_delta(Reader *rd, const SnapFrame *base, SnapFrame *out);
// Client: create/update/destroy replica entities; push interpolation samples.
void repl_apply(World *w, const SnapFrame *f, int local_slot, InterpBuffer *interp);
// Client: interpolate replicated transforms at render_tick (fractional server ticks).
void repl_interpolate(World *w, InterpBuffer *interp, double render_tick, int local_slot);

#endif
