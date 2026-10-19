#include "anim_net.h"
#include "net/bitbuf.h"
#include <math.h>

u16 anq_dir(vec3 d) {
    float l = fabsf(d.x) + fabsf(d.y) + fabsf(d.z);
    if (l < 1e-8f) return 0x8080;
    float x = d.x / l, y = d.y / l, z = d.z / l;
    if (z < 0) {
        float ox = (1.0f - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f);
        float oy = (1.0f - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    int qx = (int)lrintf((x * 0.5f + 0.5f) * 255.0f), qy = (int)lrintf((y * 0.5f + 0.5f) * 255.0f);
    return (u16)(DG_CLAMP(qx, 0, 255) | (DG_CLAMP(qy, 0, 255) << 8));
}

vec3 and_dir(u16 q) {
    float x = (float)(q & 0xFF) / 255.0f * 2.0f - 1.0f;
    float y = (float)(q >> 8) / 255.0f * 2.0f - 1.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0) {
        float ox = (1.0f - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f);
        float oy = (1.0f - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    return v3_norm(v3(x, y, z));
}

u16 anim_net_diff(const AnimNetState *a, const AnimNetState *b) {
    if (!a) return (u16)(ANS_ALL & ~(b->flags & ANF_DEAD ? 0 : ANS_DEATH) & ~(b->phone_flags ? 0 : ANS_PHONE));
    u16 m = 0;
    if (a->state[0] != b->state[0] || a->state[1] != b->state[1]) m |= ANS_STATE;
    if (a->phase != b->phase) m |= ANS_PHASE;
    if (a->state_start != b->state_start) m |= ANS_START;
    if (a->speed != b->speed) m |= ANS_SPEED;
    if (a->move_x != b->move_x || a->move_y != b->move_y) m |= ANS_MOVE;
    if (a->aim_pitch != b->aim_pitch) m |= ANS_PITCH;
    if (a->aim_yaw != b->aim_yaw) m |= ANS_YAW;
    if (a->body_yaw != b->body_yaw) m |= ANS_BODY;
    if (a->flags != b->flags) m |= ANS_FLAGS;
    if (a->limb_status != b->limb_status) m |= ANS_LIMB;
    if (a->weapon != b->weapon) m |= ANS_WEAPON;
    if (a->mantle_t != b->mantle_t) m |= ANS_MANTLE;
    if (a->character_id != b->character_id) m |= ANS_CHAR;
    if (a->death_tick != b->death_tick || a->death_seed != b->death_seed) m |= ANS_DEATH;
    if (memcmp(a->parts, b->parts, sizeof(a->parts)) || memcmp(a->worn, b->worn, sizeof(a->worn))) m |= ANS_PARTS;
    if (a->phone_flags != b->phone_flags || a->phone_rot != b->phone_rot) m |= ANS_PHONE;
    return m;
}

int anim_net_field_bytes(u16 f) {
    int n = 0;
    if (f & ANS_STATE) n += 2;
    if (f & ANS_PHASE) n += 1;
    if (f & ANS_START) n += 2;
    if (f & ANS_SPEED) n += 1;
    if (f & ANS_MOVE) n += 2;
    if (f & ANS_PITCH) n += 2;
    if (f & ANS_YAW) n += 2;
    if (f & ANS_BODY) n += 2;
    if (f & ANS_FLAGS) n += 1;
    if (f & ANS_LIMB) n += 1;
    if (f & ANS_WEAPON) n += 1;
    if (f & ANS_MANTLE) n += 1;
    if (f & ANS_CHAR) n += 1;
    if (f & ANS_DEATH) n += 5;
    if (f & ANS_PHONE) n += 5;
    if (f & ANS_PARTS) n += 2 + ANIM_NET_PART_SLOTS + ANIM_NET_WORN_SLOTS;
    return n;
}

// The parts' bits, then the worn gear's (bits ANIM_NET_PART_SLOTS..).
static u16 parts_mask(const AnimNetState *s) {
    u16 m = 0;
    for (int i = 0; i < ANIM_NET_PART_SLOTS; i++) if (s->parts[i]) m |= (u16)(1u << i);
    for (int i = 0; i < ANIM_NET_WORN_SLOTS; i++) if (s->worn[i]) m |= (u16)(1u << (ANIM_NET_PART_SLOTS + i));
    return m;
}

int anim_net_size(const AnimNetState *s, u16 f) {
    int n = anim_net_field_bytes(f & ~ANS_PARTS);
    if (f & ANS_PARTS) {
        n += 2;
        for (u16 m = parts_mask(s); m; m &= (u16)(m - 1)) n++;
    }
    return n;
}

void anim_net_write(Writer *w, const AnimNetState *s, u16 f) {
    w_u16(w, f);
    if (f & ANS_STATE) { w_u8(w, s->state[0]); w_u8(w, s->state[1]); }
    if (f & ANS_PHASE) w_u8(w, s->phase);
    if (f & ANS_START) w_u16(w, s->state_start);
    if (f & ANS_SPEED) w_u8(w, s->speed);
    if (f & ANS_MOVE) { w_u8(w, (u8)s->move_x); w_u8(w, (u8)s->move_y); }
    if (f & ANS_PITCH) w_u16(w, (u16)s->aim_pitch);
    if (f & ANS_YAW) w_u16(w, s->aim_yaw);
    if (f & ANS_BODY) w_u16(w, s->body_yaw);
    if (f & ANS_FLAGS) w_u8(w, s->flags);
    if (f & ANS_LIMB) w_u8(w, s->limb_status);
    if (f & ANS_WEAPON) w_u8(w, s->weapon);
    if (f & ANS_MANTLE) w_u8(w, s->mantle_t);
    if (f & ANS_CHAR) w_u8(w, s->character_id);
    if (f & ANS_DEATH) { w_u32(w, s->death_tick); w_u8(w, s->death_seed); }
    if (f & ANS_PARTS) {
        u16 m = parts_mask(s);
        w_u16(w, m);
        for (int i = 0; i < ANIM_NET_PART_SLOTS; i++) if (m & (1u << i)) w_u8(w, s->parts[i]);
        for (int i = 0; i < ANIM_NET_WORN_SLOTS; i++) if (m & (1u << (ANIM_NET_PART_SLOTS + i))) w_u8(w, s->worn[i]);
    }
    if (f & ANS_PHONE) { w_u8(w, s->phone_flags); w_u32(w, s->phone_rot); }
}

void anim_net_read(Reader *r, AnimNetState *s, u16 *fields) {
    u16 f = r_u16(r);
    if (fields) *fields = f;
    if (f & ANS_STATE) { s->state[0] = r_u8(r); s->state[1] = r_u8(r); }
    if (f & ANS_PHASE) s->phase = r_u8(r);
    if (f & ANS_START) s->state_start = r_u16(r);
    if (f & ANS_SPEED) s->speed = r_u8(r);
    if (f & ANS_MOVE) { s->move_x = (i8)r_u8(r); s->move_y = (i8)r_u8(r); }
    if (f & ANS_PITCH) s->aim_pitch = (i16)r_u16(r);
    if (f & ANS_YAW) s->aim_yaw = r_u16(r);
    if (f & ANS_BODY) s->body_yaw = r_u16(r);
    if (f & ANS_FLAGS) s->flags = r_u8(r);
    if (f & ANS_LIMB) s->limb_status = r_u8(r);
    if (f & ANS_WEAPON) s->weapon = r_u8(r);
    if (f & ANS_MANTLE) s->mantle_t = r_u8(r);
    if (f & ANS_CHAR) s->character_id = r_u8(r);
    if (f & ANS_DEATH) { s->death_tick = r_u32(r); s->death_seed = r_u8(r); }
    if (f & ANS_PARTS) {
        u16 m = r_u16(r);
        for (int i = 0; i < ANIM_NET_PART_SLOTS; i++) s->parts[i] = (m & (1u << i)) ? r_u8(r) : 0;
        for (int i = 0; i < ANIM_NET_WORN_SLOTS; i++) s->worn[i] = (m & (1u << (ANIM_NET_PART_SLOTS + i))) ? r_u8(r) : 0;
    }
    if (f & ANS_PHONE) { s->phone_flags = r_u8(r); s->phone_rot = r_u32(r); }
}

// Event layout: type u8 | ent u32 | tick u32 | seed u8 | [hit/die: part u8, dir u16, strength u8, dmg u8] | [land/fire: extra u8]
int anim_event_size(const AnimEvent *e) {
    int n = 1 + 4 + 4 + 1;
    if (e->type == AEV_HIT || e->type == AEV_DIE) n += 5;
    if (e->type == AEV_LAND || e->type == AEV_FIRE || e->type == AEV_EQUIP) n += 1;
    return n;
}

void anim_event_write(Writer *w, const AnimEvent *e) {
    w_u8(w, e->type);
    w_u32(w, e->ent);
    w_u32(w, e->tick);
    w_u8(w, e->seed);
    if (e->type == AEV_HIT || e->type == AEV_DIE) {
        w_u8(w, e->body_part);
        w_u16(w, e->dir);
        w_u8(w, e->strength);
        w_u8(w, (u8)(e->damage_type & 0x0F));
    }
    if (e->type == AEV_LAND || e->type == AEV_FIRE || e->type == AEV_EQUIP) w_u8(w, e->extra);
}

bool anim_event_read(Reader *r, AnimEvent *e) {
    memset(e, 0, sizeof(*e));
    e->type = r_u8(r);
    e->ent = r_u32(r);
    e->tick = r_u32(r);
    e->seed = r_u8(r);
    if (e->type == AEV_HIT || e->type == AEV_DIE) {
        e->body_part = r_u8(r);
        e->dir = r_u16(r);
        e->strength = r_u8(r);
        e->damage_type = r_u8(r) & 0x0F;
    }
    if (e->type == AEV_LAND || e->type == AEV_FIRE || e->type == AEV_EQUIP) e->extra = r_u8(r);
    return !r->overflow && e->type > AEV_NONE && e->type < AEV_COUNT;
}
