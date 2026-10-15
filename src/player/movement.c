// First-person movement on top of Jolt CharacterVirtual: walk/sprint/crouch, acceleration +
// friction, air control, coyote time, jump buffering, stairs/slopes, and ledge traversal
// (mantle, sprint-vault, ledge hang with pull-up / drop).
#include "player.h"
#include "world/world.h"
#include "physics/physics.h"
#include "render/debug_draw.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include <stdlib.h>
#include <math.h>

#define T (g_engine.ptuning)

void player_register_cvars(void) {
    PlayerTuning *t = (PlayerTuning *)calloc(1, sizeof(PlayerTuning));
    g_engine.ptuning = t;
    t->walk_speed = cvar_reg_float("pl_walk_speed", 4.2f, 0.5f, 20, CVAR_SERVER, "walk speed (m/s)");
    t->sprint_speed = cvar_reg_float("pl_sprint_speed", 6.8f, 0.5f, 30, CVAR_SERVER, "sprint speed (m/s)");
    t->crouch_speed = cvar_reg_float("pl_crouch_speed", 2.0f, 0.2f, 10, CVAR_SERVER, "crouch speed (m/s)");
    t->walk_slow_speed = cvar_reg_float("pl_slow_walk_speed", 1.8f, 0.2f, 10, CVAR_SERVER, "slow walk speed (m/s)");
    t->accel = cvar_reg_float("pl_accel", 11.0f, 0.1f, 100, CVAR_SERVER, "ground acceleration");
    t->air_accel = cvar_reg_float("pl_air_accel", 2.5f, 0.0f, 100, CVAR_SERVER, "air acceleration");
    t->air_control = cvar_reg_float("pl_air_control", 0.35f, 0.0f, 1, CVAR_SERVER, "fraction of ground speed reachable in air");
    t->friction = cvar_reg_float("pl_friction", 8.0f, 0.0f, 50, CVAR_SERVER, "ground friction");
    t->gravity = cvar_reg_float("pl_gravity", 18.0f, 1.0f, 60, CVAR_SERVER, "player gravity (m/s^2)");
    t->jump_speed = cvar_reg_float("pl_jump_speed", 6.0f, 0.5f, 30, CVAR_SERVER, "jump impulse (m/s), ~1.0m jump");
    t->coyote_time = cvar_reg_float("pl_coyote_time", 0.12f, 0.0f, 1, CVAR_SERVER, "grace time to jump after leaving ground");
    t->jump_buffer_time = cvar_reg_float("pl_jump_buffer", 0.15f, 0.0f, 1, CVAR_SERVER, "jump press buffer time");
    t->step_height = cvar_reg_float("pl_step_height", 0.32f, 0.0f, 1, CVAR_SERVER, "max stair step height (m)");
    t->max_slope = cvar_reg_float("pl_max_slope", 47.0f, 1.0f, 89, CVAR_SERVER, "max walkable slope (deg)");
    t->mantle_min = cvar_reg_float("pl_mantle_min", 0.45f, 0.1f, 3, CVAR_SERVER, "min ledge height for mantle (m)");
    t->mantle_max = cvar_reg_float("pl_mantle_max", 2.4f, 0.5f, 4, CVAR_SERVER, "max ledge height for direct mantle (m)");
    t->hang_max = cvar_reg_float("pl_hang_reach", 2.3f, 1.0f, 4, CVAR_SERVER, "airborne: max ledge height above feet that can be grabbed (m)");
    t->air_mantle_max = cvar_reg_float("pl_air_mantle_max", 1.5f, 0.3f, 3, CVAR_SERVER, "airborne: max ledge height above feet for a direct mantle (m)");
    t->vault_max = cvar_reg_float("pl_vault_max", 1.2f, 0.3f, 2, CVAR_SERVER, "max obstacle height for sprint vault (m)");
    t->mantle_reach = cvar_reg_float("pl_mantle_reach", 0.65f, 0.1f, 2, CVAR_SERVER, "forward reach for ledge detection (m)");
    t->mantle_speed = cvar_reg_float("pl_mantle_speed", 1.0f, 0.2f, 4, CVAR_SERVER, "mantle animation speed multiplier");
    t->vault_speed = cvar_reg_float("pl_vault_speed", 1.0f, 0.2f, 4, CVAR_SERVER, "vault animation speed multiplier");
    t->debug_mantle = cvar_reg_bool("pl_debug_mantle", false, 0, "draw mantle/vault/hang shape casts");
    t->debug_melee = cvar_reg_bool("wpn_debug_melee", false, 0, "draw melee sweep rays");
    t->noclip_speed = cvar_reg_float("pl_noclip_speed", 10.0f, 1.0f, 100, 0, "noclip fly speed");
    t->crouch_toggle = cvar_reg_bool("cl_crouch_toggle", false, CVAR_ARCHIVE, "crouch: toggle instead of hold");
    t->sprint_toggle = cvar_reg_bool("cl_sprint_toggle", false, CVAR_ARCHIVE, "sprint: toggle instead of hold");
}

const PlayerTuning *player_tuning(void) { return T; }

float player_eye_height(const PlayerMove *m) {
    return lerpf(PLAYER_EYE_STAND, PLAYER_EYE_CROUCH, smoothstepf(0, 1, m->crouch_amount));
}

vec3 player_eye_pos(const PlayerMove *m) { return v3_add(m->pos, v3(0, player_eye_height(m), 0)); }

void player_move_reset(PlayerSim *sim, vec3 feet, float yaw) {
    memset(&sim->move, 0, sizeof(sim->move));
    sim->move.pos = feet;
    sim->move.yaw = yaw;
    sim->move.mode = MOVE_WALK;
    sim->move.fall_start_y = feet.y;
}

// ------------------------------------------------------------------ ledge detection

typedef enum LedgeKind { LEDGE_NONE, LEDGE_MANTLE, LEDGE_VAULT, LEDGE_HANG } LedgeKind;

typedef struct LedgeResult {
    LedgeKind kind;
    vec3 wall_point, wall_normal;
    vec3 top;              // point on the ledge top surface just past the edge
    float height;          // top.y - feet.y
    vec3 target;           // final feet position
    bool target_crouched;
    vec3 land;             // vault landing
    vec3 hang_pos;
} LedgeResult;

#define DBG_T 2.0f

static bool cap_free(PhysicsWorld *pw, vec3 feet, float height, BodyId ignore) {
    return !phys_capsule_overlap(pw, v3_add(feet, v3(0, 0.03f, 0)), PLAYER_RADIUS - 0.02f, height - 0.04f, PL_MASK_WORLD | PL_MASK(PL_CHARACTER), ignore);
}

// airborne: ledges up to mantle_max above the take-off height (ground_y) are mantled directly;
// higher ones (still within hang reach of the current feet) are grabbed and hung from.
static LedgeResult detect_ledge(PhysicsWorld *pw, vec3 feet, float ground_y, float yaw, BodyId ignore, bool allow_vault, bool airborne, bool dbg) {
    LedgeResult r;
    memset(&r, 0, sizeof(r));
    vec3 f = forward_from_yaw(yaw);
    float reach = PLAYER_RADIUS + T->mantle_reach->f;
    float min_h = T->mantle_min->f;
    float max_h = airborne ? T->air_mantle_max->f : T->mantle_max->f;
    float top_h = airborne ? T->hang_max->f : max_h;

    // 1) Forward wall probes at several heights (lowest hit wins).
    const float probe_r = 0.18f;
    bool wall = false;
    ShapeHit wh;
    for (float h = 0.3f; h <= top_h - 0.1f; h += 0.3f) {
        vec3 o = v3_add(feet, v3(0, h, 0));
        ShapeHit hit;
        if (phys_sphere_cast(pw, o, v3_scale(f, reach), probe_r, PL_MASK_WORLD, ignore, &hit) && fabsf(hit.normal.y) < 0.6f) {
            if (dbg) dbg_line(o, hit.point, DBG_RED, DBG_T, false);
            wh = hit;
            wall = true;
            break;
        }
        if (dbg) dbg_line(o, v3_madd(o, f, reach), DBG_GREY, DBG_T, false);
    }
    if (!wall) return r;
    r.wall_point = wh.point;
    r.wall_normal = v3_norm(v3_flat(wh.normal));
    if (v3_len2(r.wall_normal) < 0.5f) r.wall_normal = v3_neg(f);
    // Approach direction: straight into the wall.
    vec3 into = v3_neg(r.wall_normal);

    // 2) Downward probe just past the wall face to find the top surface.
    vec3 above = v3_add(v3_madd(v3(wh.point.x, feet.y, wh.point.z), into, 0.22f), v3(0, top_h + 0.25f, 0));
    float down_len = top_h + 0.25f - min_h + 0.05f;
    ShapeHit th;
    if (!phys_sphere_cast(pw, above, v3(0, -down_len, 0), 0.12f, PL_MASK_WORLD, ignore, &th)) {
        if (dbg) dbg_line(above, v3_add(above, v3(0, -down_len, 0)), DBG_GREY, DBG_T, false);
        return r;
    }
    if (th.fraction <= 0.0f || th.normal.y < 0.7f) {
        if (dbg) dbg_cross(th.point, 0.1f, DBG_RED, DBG_T);
        return r; // started inside geometry (wall too tall) or not a walkable top
    }
    if (dbg) {
        dbg_line(above, th.point, DBG_YELLOW, DBG_T, false);
        dbg_cross(th.point, 0.1f, DBG_YELLOW, DBG_T);
    }
    r.top = th.point;
    r.height = th.point.y - feet.y;
    if (r.height < min_h || r.height > top_h) return r;

    // 3) Head room: nothing directly above the player up to the ledge height (+ body).
    if (r.height > 1.2f) {
        vec3 head = v3_add(feet, v3(0, PLAYER_HEIGHT - 0.3f, 0));
        float rise = r.height + 0.3f - (PLAYER_HEIGHT - 0.3f) + 0.2f;
        if (rise > 0.0f) {
            ShapeHit hh;
            if (phys_sphere_cast(pw, head, v3(0, rise, 0), 0.25f, PL_MASK_WORLD, ignore, &hh)) {
                if (dbg) dbg_line(head, hh.point, DBG_RED, DBG_T, false);
                return r;
            }
        }
    }

    // 4) Vault: thin obstacle, sprinting, low enough, free landing beyond.
    if (allow_vault && r.height <= T->vault_max->f) {
        for (float depth = 0.45f; depth <= 1.05f; depth += 0.3f) {
            vec3 beyond = v3_add(v3_madd(v3(wh.point.x, r.top.y, wh.point.z), into, depth + 0.35f), v3(0, 0.25f, 0));
            ShapeHit gh;
            if (!phys_sphere_cast(pw, beyond, v3(0, -2.6f, 0), 0.15f, PL_MASK_WORLD, ignore, &gh)) continue;
            if (gh.fraction <= 0.0f || gh.point.y > r.top.y - 0.35f || gh.normal.y < 0.7f) continue;
            vec3 land = v3(gh.point.x, gh.point.y + 0.02f, gh.point.z);
            land = v3_madd(land, into, 0.15f);
            vec3 over = v3_add(v3_madd(v3(wh.point.x, r.top.y, wh.point.z), into, depth * 0.5f), v3(0, 0.05f, 0));
            if (cap_free(pw, land, PLAYER_HEIGHT, ignore) && cap_free(pw, over, PLAYER_CROUCH_HEIGHT, ignore)) {
                r.kind = LEDGE_VAULT;
                r.land = land;
                r.target = land;
                if (dbg) { dbg_capsule(land, PLAYER_RADIUS, PLAYER_HEIGHT, DBG_CYAN, DBG_T); dbg_capsule(over, PLAYER_RADIUS, PLAYER_CROUCH_HEIGHT, DBG_CYAN, DBG_T); }
                return r;
            }
        }
    }

    // 5) Clearance on top: standing, else crouched.
    vec3 target = v3_madd(v3(wh.point.x, r.top.y + 0.02f, wh.point.z), into, PLAYER_RADIUS + 0.12f);
    if (cap_free(pw, target, PLAYER_HEIGHT, ignore)) {
        r.target = target;
        r.target_crouched = false;
    } else if (cap_free(pw, target, PLAYER_CROUCH_HEIGHT, ignore)) {
        r.target = target;
        r.target_crouched = true;
    } else {
        if (dbg) dbg_capsule(target, PLAYER_RADIUS, PLAYER_HEIGHT, DBG_RED, DBG_T);
        return r;
    }
    if (dbg) dbg_capsule(r.target, PLAYER_RADIUS, r.target_crouched ? PLAYER_CROUCH_HEIGHT : PLAYER_HEIGHT, DBG_GREEN, DBG_T);

    if (r.height <= max_h) {
        r.kind = LEDGE_MANTLE;
    } else if (airborne) {
        // Hang so the hands sit on the edge: feet ~2.05 m below the top, body against the wall.
        vec3 hp = v3_madd(v3(wh.point.x, r.top.y - 2.05f, wh.point.z), r.wall_normal, PLAYER_RADIUS + 0.06f);
        if (!cap_free(pw, hp, PLAYER_HEIGHT, ignore)) {
            if (dbg) dbg_capsule(hp, PLAYER_RADIUS, PLAYER_HEIGHT, DBG_RED, DBG_T);
            return r;
        }
        r.kind = LEDGE_HANG;
        r.hang_pos = hp;
        if (dbg) dbg_capsule(hp, PLAYER_RADIUS, PLAYER_HEIGHT, DBG_MAGENTA, DBG_T);
    }
    return r;
}

static void set_hand_anchors(PlayerMove *m, vec3 wall_point, vec3 wall_normal, float top_y) {
    vec3 tangent = v3_norm(v3_cross(v3(0, 1, 0), wall_normal));
    vec3 edge = v3_madd(v3(wall_point.x, top_y + 0.02f, wall_point.z), wall_normal, -0.06f);
    // Left hand on the left side of the body (tangent points to the player's left when facing the wall).
    m->hand_anchor[0] = v3_madd(edge, tangent, -0.24f);
    m->hand_anchor[1] = v3_madd(edge, tangent, 0.24f);
    vec3 right_dir = right_from_yaw(atan2f(wall_normal.x, wall_normal.z));
    (void)right_dir;
    // Ensure index 0 is the player's left hand: compare against the player's right vector.
    vec3 face_right = v3_cross(v3_neg(wall_normal), v3(0, 1, 0));
    if (v3_dot(v3_sub(m->hand_anchor[0], edge), face_right) > 0) {
        vec3 t = m->hand_anchor[0];
        m->hand_anchor[0] = m->hand_anchor[1];
        m->hand_anchor[1] = t;
    }
    m->hand_anchor_valid = 1;
}

static void start_traverse(PlayerMove *m, MoveMode kind, vec3 start, vec3 mid, vec3 end, float dur, bool end_crouched) {
    m->mode = (u8)kind;
    m->m_kind = (u8)kind;
    m->m_start = start;
    m->m_mid = mid;
    m->m_end = end;
    m->m_t = 0;
    m->m_dur = fmaxf(0.15f, dur);
    m->m_end_crouched = end_crouched ? 1 : 0;
    m->vel = v3(0, 0, 0);
}

static void start_mantle(PlayerMove *m, const LedgeResult *lr, vec3 from) {
    float h = lr->top.y - from.y;
    // Rise in place (pressed against the wall) to just above the lip, then roll onto the ledge.
    vec3 against = v3_madd(v3(lr->wall_point.x, from.y, lr->wall_point.z), lr->wall_normal, PLAYER_RADIUS + 0.03f);
    vec3 mid = v3(against.x, lr->top.y + 0.06f, against.z);
    float dur = (0.32f + 0.22f * h) / T->mantle_speed->f;
    start_traverse(m, MOVE_MANTLE, from, mid, lr->target, dur, lr->target_crouched);
    set_hand_anchors(m, lr->wall_point, lr->wall_normal, lr->top.y);
}

static void start_vault(PlayerMove *m, const LedgeResult *lr, vec3 from) {
    vec3 into = v3_neg(lr->wall_normal);
    vec3 over = v3_add(v3_madd(v3(lr->wall_point.x, lr->top.y, lr->wall_point.z), into, 0.2f), v3(0, 0.12f, 0));
    float dur = (0.38f + 0.1f * lr->height) / T->vault_speed->f;
    start_traverse(m, MOVE_VAULT, from, over, lr->land, dur, false);
    // One hand planted on the obstacle (left), the right one free.
    set_hand_anchors(m, lr->wall_point, lr->wall_normal, lr->top.y);
    m->hand_anchor[1] = v3_add(m->hand_anchor[1], v3(0, 0.25f, 0));
}

static vec3 traverse_pos(const PlayerMove *m, float t) {
    t = saturate(t);
    if (m->m_kind == MOVE_VAULT) {
        // Quadratic arc start -> over -> land.
        vec3 a = v3_lerp(m->m_start, m->m_mid, t), b = v3_lerp(m->m_mid, m->m_end, t);
        vec3 p = v3_lerp(a, b, t);
        p.y += sinf(t * DG_PI) * 0.15f;
        return p;
    }
    // Mantle: 0..0.6 rise (ease-out), 0.6..1 forward onto ledge (ease-in-out).
    const float split = 0.6f;
    if (t < split) {
        float u = t / split;
        u = 1.0f - (1.0f - u) * (1.0f - u);
        vec3 horiz = v3_lerp(m->m_start, v3(m->m_mid.x, m->m_start.y, m->m_mid.z), saturate(u * 1.5f));
        return v3(horiz.x, lerpf(m->m_start.y, m->m_mid.y, u), horiz.z);
    }
    float u = smoothstepf(0, 1, (t - split) / (1.0f - split));
    vec3 p = v3_lerp(m->m_mid, m->m_end, u);
    p.y = fmaxf(p.y, lerpf(m->m_mid.y, m->m_end.y, u));
    return p;
}

// ------------------------------------------------------------------ main tick

static vec3 accelerate(vec3 vel, vec3 wish_dir, float wish_speed, float accel, float dt) {
    float cur = v3_dot(vel, wish_dir);
    float add = wish_speed - cur;
    if (add <= 0) return vel;
    float a = fminf(accel * wish_speed * dt, add);
    return v3_madd(vel, wish_dir, a);
}

void player_move_tick(World *w, int slot, PlayerSim *sim, PhysCharacter *ch, const UserCmd *cmd, float dt, bool first) {
    PlayerMove *m = &sim->move;
    PhysicsWorld *pw = w->phys;
    BodyId self = phys_character_inner_body(ch);
    bool dbg = first && T->debug_mantle->i;
    u16 buttons = cmd->buttons;
    u16 pressed = (u16)(buttons & ~sim->last_buttons);

    m->yaw = cmd->yaw;
    m->pitch = clampf(cmd->pitch, -89.0f * DG_DEG2RAD, 89.0f * DG_DEG2RAD);
    if (m->regrab_cooldown > 0) m->regrab_cooldown -= dt;
    if (m->jump_buffer > 0) m->jump_buffer -= dt;
    if (pressed & BTN_JUMP) m->jump_buffer = T->jump_buffer_time->f;

    vec3 fwd = forward_from_yaw(m->yaw), right = right_from_yaw(m->yaw);
    vec2 mv = v2(cmd->move_x / 127.0f, cmd->move_y / 127.0f);
    vec3 wish = v3_add(v3_scale(right, mv.x), v3_scale(fwd, mv.y));
    float wish_len = v3_len(wish);
    if (wish_len > 1.0f) wish = v3_scale(wish, 1.0f / wish_len), wish_len = 1.0f;
    vec3 wish_dir = wish_len > 1e-3f ? v3_scale(wish, 1.0f / wish_len) : v3(0, 0, 0);

    if ((pressed & BTN_NOCLIP)) {
        m->mode = m->mode == MOVE_NOCLIP ? MOVE_WALK : MOVE_NOCLIP;
        m->vel = v3(0, 0, 0);
    }

    // ---- dead: just fall
    if (m->mode == MOVE_DEAD) {
        m->vel.x = m->vel.z = 0;
        m->vel.y -= T->gravity->f * dt;
        CharacterMoveParams mp = { 0, 0.2f, false, true };
        phys_character_update(pw, ch, dt, m->vel, &mp);
        m->pos = phys_character_pos(ch);
        m->vel = phys_character_vel(ch);
        if (phys_character_ground(ch) == GROUND_ON) m->vel.y = 0;
        sim->last_buttons = buttons;
        return;
    }

    // ---- noclip
    if (m->mode == MOVE_NOCLIP) {
        vec3 look = dir_from_angles(m->yaw, m->pitch);
        vec3 fly = v3_add(v3_scale(right, mv.x), v3_scale(look, mv.y));
        if (buttons & BTN_JUMP) fly.y += 1;
        if (buttons & BTN_CROUCH) fly.y -= 1;
        float sp = T->noclip_speed->f * ((buttons & BTN_SPRINT) ? 3.0f : 1.0f);
        m->vel = v3_scale(fly, sp);
        m->pos = v3_madd(m->pos, m->vel, dt);
        phys_character_set_pos(ch, m->pos);
        m->on_ground = 0;
        sim->last_buttons = buttons;
        return;
    }

    // ---- crouch (hold or toggle); standing up needs head room
    bool want_crouch;
    if (T->crouch_toggle->i) {
        if (pressed & BTN_CROUCH) m->crouch_toggled ^= 1;
        want_crouch = m->crouch_toggled != 0;
    } else {
        want_crouch = (buttons & BTN_CROUCH) != 0;
    }
    if (m->mode == MOVE_WALK) {
        if (want_crouch && !m->crouched) {
            if (phys_character_set_crouched(pw, ch, true)) m->crouched = 1;
        } else if (!want_crouch && m->crouched) {
            if (phys_character_set_crouched(pw, ch, false)) m->crouched = 0;
        }
    }
    m->crouch_amount = approachf(m->crouch_amount, m->crouched ? 1.0f : 0.0f, dt / 0.16f);

    // ---- sprint
    bool sprint_input;
    if (T->sprint_toggle->i) {
        if (pressed & BTN_SPRINT) m->sprint_toggled ^= 1;
        if (mv.y <= 0.1f) m->sprint_toggled = 0;
        sprint_input = m->sprint_toggled != 0;
    } else {
        sprint_input = (buttons & BTN_SPRINT) != 0;
    }
    m->sprinting = (u8)(sprint_input && mv.y > 0.3f && !m->crouched && !(buttons & BTN_AIM));

    // ---- scripted traversal (mantle / vault)
    if (m->mode == MOVE_MANTLE || m->mode == MOVE_VAULT) {
        m->m_t += dt / m->m_dur;
        vec3 p = traverse_pos(m, m->m_t);
        phys_character_set_pos(ch, p);
        m->pos = p;
        m->vel = v3(0, 0, 0);
        m->on_ground = 0;
        // Release the hand anchors once the body is over the lip.
        if (m->m_t > 0.75f) m->hand_anchor_valid = 0;
        if (m->m_t >= 1.0f) {
            bool was_vault = m->mode == MOVE_VAULT;
            m->mode = MOVE_WALK;
            m->hand_anchor_valid = 0;
            if (m->m_end_crouched && !m->crouched) {
                phys_character_set_crouched(pw, ch, true);
                m->crouched = 1;
                m->crouch_amount = 1;
            }
            // Carry momentum out of a vault.
            if (was_vault) m->vel = v3_scale(fwd, T->sprint_speed->f * 0.85f);
            m->on_ground = 1;
            m->coyote = 0;
            m->fall_start_y = m->pos.y;
        }
        sim->last_buttons = buttons;
        return;
    }

    // ---- ledge hang
    if (m->mode == MOVE_HANG) {
        m->hang_time += dt;
        phys_character_set_pos(ch, m->hang_pos);
        m->pos = m->hang_pos;
        m->vel = v3(0, 0, 0);
        m->on_ground = 0;
        bool pull = ((pressed & BTN_JUMP) || (mv.y > 0.5f && m->hang_time > 0.25f));
        bool drop = (pressed & BTN_CROUCH) || (mv.y < -0.5f);
        if (pull && m->hang_time > 0.15f) {
            // Re-run detection from the hang position to get a fresh clearance check.
            float wall_yaw = atan2f(m->hang_normal.x, m->hang_normal.z);
            LedgeResult lr = detect_ledge(pw, m->hang_pos, m->hang_pos.y, wall_yaw, self, false, false, dbg);
            if (lr.kind == LEDGE_MANTLE) {
                start_mantle(m, &lr, m->hang_pos);
                m->m_dur = (0.75f) / T->mantle_speed->f;
            }
        } else if (drop) {
            m->mode = MOVE_WALK;
            m->hand_anchor_valid = 0;
            m->vel = v3_scale(m->hang_normal, 1.2f);
            m->regrab_cooldown = 0.5f;
            m->fall_start_y = m->pos.y;
        }
        sim->last_buttons = buttons;
        return;
    }

    // ---- walking / airborne
    GroundState gs = phys_character_ground(ch);
    bool on_ground = gs == GROUND_ON;
    bool steep = gs == GROUND_STEEP;
    m->ground_steep = steep ? 1 : 0;
    vec3 vel = m->vel;
    float speed = m->crouched ? T->crouch_speed->f
                : m->sprinting ? T->sprint_speed->f
                : (buttons & BTN_WALK) ? T->walk_slow_speed->f : T->walk_speed->f;
    if (buttons & BTN_AIM) speed = fminf(speed, T->walk_speed->f * 0.7f);

    // Ledge traversal triggers (before jumping so a jump press at a ledge mantles instead).
    bool moving_fwd = mv.y > 0.2f;
    bool jump_wanted = m->jump_buffer > 0.0f;
    if (moving_fwd && !m->crouched && m->regrab_cooldown <= 0) {
        bool try_ground = on_ground && (jump_wanted || m->sprinting);
        bool try_air = !on_ground && (buttons & BTN_JUMP) && vel.y < 3.0f;
        if (try_ground || try_air) {
            bool allow_vault = on_ground && m->sprinting;
            LedgeResult lr = detect_ledge(pw, m->pos, on_ground ? m->pos.y : m->fall_start_y, m->yaw, self, allow_vault, !on_ground, dbg);
            if (lr.kind == LEDGE_VAULT) {
                start_vault(m, &lr, m->pos);
                m->jump_buffer = 0;
                sim->last_buttons = buttons;
                return;
            }
            if (lr.kind == LEDGE_MANTLE && (jump_wanted || try_air)) {
                start_mantle(m, &lr, m->pos);
                m->jump_buffer = 0;
                sim->last_buttons = buttons;
                return;
            }
            if (lr.kind == LEDGE_HANG && !on_ground) {
                m->mode = MOVE_HANG;
                m->hang_pos = lr.hang_pos;
                m->hang_normal = lr.wall_normal;
                m->ledge_point = lr.top;
                m->hang_time = 0;
                m->vel = v3(0, 0, 0);
                set_hand_anchors(m, lr.wall_point, lr.wall_normal, lr.top.y);
                phys_character_set_pos(ch, lr.hang_pos);
                m->pos = lr.hang_pos;
                sim->last_buttons = buttons;
                return;
            }
        }
    }

    if (on_ground) {
        // Friction then acceleration on the horizontal plane.
        vec3 hv = v3_flat(vel);
        float hs = v3_len(hv);
        if (hs > 1e-4f) {
            float drop = fmaxf(hs, 1.0f) * T->friction->f * dt;
            float ns = fmaxf(hs - drop, 0.0f);
            hv = v3_scale(hv, ns / hs);
        }
        hv = accelerate(hv, wish_dir, speed * wish_len, T->accel->f, dt);
        vel = v3(hv.x, 0.0f, hv.z);
        m->coyote = T->coyote_time->f;
        m->air_time = 0;
    } else {
        // Air control: limited acceleration towards a fraction of ground speed.
        vec3 hv = v3_flat(vel);
        float air_cap = fmaxf(speed * T->air_control->f, v3_len(hv));
        vec3 nv = accelerate(hv, wish_dir, speed * wish_len, T->air_accel->f, dt);
        float nl = v3_len(nv);
        if (nl > air_cap && nl > 1e-4f) nv = v3_scale(nv, air_cap / nl);
        vel.x = nv.x;
        vel.z = nv.z;
        vel.y -= T->gravity->f * dt;
        if (m->coyote > 0) m->coyote -= dt;
        m->air_time += dt;
    }
    if (steep) vel = phys_character_cancel_steep(ch, vel);

    // Jump (buffered, with coyote time). Standing up first if crouched and possible.
    bool jumped = false;
    if (jump_wanted && (on_ground || m->coyote > 0.0f)) {
        vel.y = T->jump_speed->f * (m->crouched ? 0.8f : 1.0f);
        m->jump_buffer = 0;
        m->coyote = 0;
        jumped = true;
        on_ground = false;
        m->fall_start_y = m->pos.y;
    }

    bool was_ground = m->on_ground != 0;
    CharacterMoveParams mp;
    mp.step_up = T->step_height->f;
    mp.stick_down = 0.45f;
    mp.allow_stairs = on_ground && !jumped;
    mp.allow_stick = was_ground && !jumped;
    phys_character_set_max_slope(ch, T->max_slope->f);
    float prev_vy = vel.y;
    phys_character_update(pw, ch, dt, vel, &mp);
    m->pos = phys_character_pos(ch);
    m->vel = phys_character_vel(ch);
    GroundState after = phys_character_ground(ch);
    m->on_ground = (after == GROUND_ON) ? 1 : 0;
    if (m->on_ground && m->vel.y < 0) m->vel.y = 0;
    if (!was_ground && m->on_ground) {
        m->land_speed = -prev_vy;
        m->land_tick = w->tick;
    }
    if (!m->on_ground && was_ground && !jumped) m->fall_start_y = m->pos.y;
    (void)slot;
    sim->last_buttons = buttons;
}
