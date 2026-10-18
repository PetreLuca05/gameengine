// First-person movement on top of Jolt CharacterVirtual: walk/sprint/crouch, acceleration +
// friction, air control, coyote time, jump buffering, stairs/slopes, ledge traversal
// (mantle, sprint-vault, ledge hang with pull-up / drop), and water: wading, swimming at the
// surface, diving, climbing out through the mantle (docs/WATER.md "Swimming").
#include "player.h"
#include "anim/anim_library.h"
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
    t->walk_speed = cvar_reg_float("pl_walk_speed", 4.2f, 0.5f, 20, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "walk speed (m/s)");
    t->sprint_speed = cvar_reg_float("pl_sprint_speed", 6.8f, 0.5f, 30, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "sprint speed (m/s)");
    t->crouch_speed = cvar_reg_float("pl_crouch_speed", 2.0f, 0.2f, 10, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "crouch speed (m/s)");
    t->walk_slow_speed = cvar_reg_float("pl_slow_walk_speed", 1.8f, 0.2f, 10, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "slow walk speed (m/s)");
    t->accel = cvar_reg_float("pl_accel", 11.0f, 0.1f, 100, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "ground acceleration");
    t->air_accel = cvar_reg_float("pl_air_accel", 2.5f, 0.0f, 100, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "air acceleration");
    t->air_control = cvar_reg_float("pl_air_control", 0.35f, 0.0f, 1, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "fraction of ground speed reachable in air");
    t->friction = cvar_reg_float("pl_friction", 8.0f, 0.0f, 50, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "ground friction");
    t->gravity = cvar_reg_float("pl_gravity", 18.0f, 1.0f, 60, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "player gravity (m/s^2)");
    t->jump_speed = cvar_reg_float("pl_jump_speed", 6.0f, 0.5f, 30, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "jump impulse (m/s), ~1.0m jump");
    t->coyote_time = cvar_reg_float("pl_coyote_time", 0.12f, 0.0f, 1, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "grace time to jump after leaving ground");
    t->jump_buffer_time = cvar_reg_float("pl_jump_buffer", 0.15f, 0.0f, 1, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "jump press buffer time");
    t->step_height = cvar_reg_float("pl_step_height", 0.32f, 0.0f, 1, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "max stair step height (m)");
    t->max_slope = cvar_reg_float("pl_max_slope", 47.0f, 1.0f, 89, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "max walkable slope (deg)");
    t->mantle_min = cvar_reg_float("pl_mantle_min", 0.45f, 0.1f, 3, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "min ledge height for mantle (m)");
    t->mantle_max = cvar_reg_float("pl_mantle_max", 2.4f, 0.5f, 4, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "max ledge height for direct mantle (m)");
    t->hang_max = cvar_reg_float("pl_hang_reach", 2.3f, 1.0f, 4, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "airborne: max ledge height above feet that can be grabbed (m)");
    t->air_mantle_max = cvar_reg_float("pl_air_mantle_max", 1.5f, 0.3f, 3, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "airborne: max ledge height above feet for a direct mantle (m)");
    t->vault_max = cvar_reg_float("pl_vault_max", 1.2f, 0.3f, 2, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "max obstacle height for sprint vault (m)");
    t->mantle_reach = cvar_reg_float("pl_mantle_reach", 0.65f, 0.1f, 2, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "forward reach for ledge detection (m)");
    t->mantle_speed = cvar_reg_float("pl_mantle_speed", 1.0f, 0.2f, 4, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "mantle animation speed multiplier");
    t->vault_speed = cvar_reg_float("pl_vault_speed", 1.0f, 0.2f, 4, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "vault animation speed multiplier");
    t->hang_yaw_limit = cvar_reg_float("pl_hang_yaw_limit", 110.0f, 20.0f, 180.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "ledge hang: how far the view turns away from the wall (deg); the body and the grips stay on the ledge");
    t->debug_mantle = cvar_reg_bool("pl_debug_mantle", false, CVAR_LOCKED, "draw mantle/vault/hang shape casts");
    t->debug_melee = cvar_reg_bool("wpn_debug_melee", false, CVAR_LOCKED, "draw melee sweep rays");
    t->noclip_speed = cvar_reg_float("pl_noclip_speed", 10.0f, 1.0f, 100, CVAR_LOCKED | CVAR_RULE, "noclip fly speed");
    t->swim_speed = cvar_reg_float("pl_swim_speed", 1.7f, 0.2f, 10, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "swim speed at the surface (m/s)");
    t->swim_sprint_speed = cvar_reg_float("pl_swim_sprint_speed", 2.7f, 0.2f, 10, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "fast swim speed (m/s, drains stamina)");
    t->swim_dive_speed = cvar_reg_float("pl_swim_dive_speed", 1.9f, 0.2f, 10, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "swim speed under water (m/s)");
    t->swim_accel = cvar_reg_float("pl_swim_accel", 2.6f, 0.1f, 20, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "swim acceleration rate (1/s): water drag towards the wished velocity");
    t->swim_enter = cvar_reg_float("pl_swim_depth", 1.35f, 0.5f, 3, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "water depth at which wading becomes swimming (m)");
    t->swim_exit = cvar_reg_float("pl_swim_exit_depth", 1.15f, 0.3f, 3, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "water depth below which a swimmer stands up (m)");
    t->swim_eye = cvar_reg_float("pl_swim_eye", 0.16f, -0.2f, 0.6f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "eye height above the surface while swimming (m)");
    t->swim_spring = cvar_reg_float("pl_swim_spring", 30.0f, 1.0f, 200, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "stiffness holding a swimmer at the surface (1/s^2)");
    t->swim_breath = cvar_reg_float("pl_swim_breath", 45.0f, 1.0f, 600, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "breath under water (s) before drowning damage");
    t->swim_stamina = cvar_reg_float("pl_swim_stamina", 14.0f, 1.0f, 120, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "fast swim stamina (s)");
    t->swim_rise = cvar_reg_float("pl_swim_rise", 0.6f, 0.0f, 5, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "rising speed of an idle diver (m/s): air in the lungs");
    t->wade_slow = cvar_reg_float("pl_wade_slow", 0.45f, 0.05f, 1, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "walk speed multiplier in water at swimming depth (wading curve end)");
    t->wade_crouch_max = cvar_reg_float("pl_wade_crouch_max", 0.55f, 0.0f, 2, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "deepest water (m) a player can crouch in");
    t->wade_sprint_max = cvar_reg_float("pl_wade_sprint_max", 0.8f, 0.0f, 2, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "deepest water (m) a player can sprint in");
    t->crouch_toggle = cvar_reg_bool("cl_crouch_toggle", false, CVAR_ARCHIVE | CVAR_EDITABLE, "crouch: toggle instead of hold");
    t->sprint_toggle = cvar_reg_bool("cl_sprint_toggle", false, CVAR_ARCHIVE | CVAR_EDITABLE, "sprint: toggle instead of hold");
}

const PlayerTuning *player_tuning(void) { return T; }

float player_eye_height(const PlayerMove *m) {
    return lerpf(PLAYER_EYE_STAND, PLAYER_EYE_CROUCH, smoothstepf(0, 1, m->crouch_amount));
}

float player_hang_wall_yaw(const PlayerMove *m) { return atan2f(m->hang_normal.x, m->hang_normal.z); }

float player_hang_clamp_yaw(const PlayerMove *m, float yaw) {
    float lim = (T && T->hang_yaw_limit ? T->hang_yaw_limit->f : 110.0f) * DG_DEG2RAD;
    float rel = wrap_angle(yaw - player_hang_wall_yaw(m));
    return yaw - (rel - clampf(rel, -lim, lim));
}

static vec3 (*g_eye_offset)(float yaw, float pitch);
void movement_set_eye_offset(vec3 (*fn)(float yaw, float pitch)) { g_eye_offset = fn; }
vec3 player_eye_offset(float yaw, float pitch) { return g_eye_offset ? g_eye_offset(yaw, pitch) : v3(0, 0, 0); }
// Eye = standing / crouched eye height + the spine bend of the aim (looking down leans forward).
vec3 player_eye_pos(const PlayerMove *m) {
    return v3_add(v3_add(m->pos, v3(0, player_eye_height(m), 0)), player_eye_offset(m->yaw, m->pitch));
}

void player_move_reset(PlayerSim *sim, vec3 feet, float yaw) {
    memset(&sim->move, 0, sizeof(sim->move));
    sim->move.pos = feet;
    sim->move.yaw = yaw;
    sim->move.body_yaw = yaw;
    sim->move.mode = MOVE_WALK;
    sim->move.fall_start_y = feet.y;
    sim->move.oxygen = player_breath_max();
    sim->move.stamina = 1.0f;
}

float player_breath_max(void) { return T ? T->swim_breath->f : 45.0f; }

float player_fall_water_scale(float depth) { return 1.0f - smoothstepf(0.0f, 2.5f, depth); }

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

// Inverse of the hang placement in detect_ledge (feet 2.05 m under the top, PLAYER_RADIUS + 0.06 m
// out from the wall point).
void player_hang_grips(vec3 feet, float wall_yaw, vec3 grip[2]) {
    PlayerMove m;
    vec3 n = v3(sinf(wall_yaw), 0.0f, cosf(wall_yaw));
    set_hand_anchors(&m, v3_madd(feet, n, -(PLAYER_RADIUS + 0.06f)), n, feet.y + 2.05f);
    grip[0] = m.hand_anchor[0];
    grip[1] = m.hand_anchor[1];
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

static SeatPoseFn g_seat_pose;
void movement_set_seat_pose(SeatPoseFn fn) { g_seat_pose = fn; }

static MoveWaterFn g_water;
void movement_set_water(MoveWaterFn fn) { g_water = fn; }

// The water at the player: depth at the feet, the state, oxygen, wetness (every mode but noclip).
typedef struct WaterHere { bool in; float surface, column; vec3 flow; } WaterHere;

static WaterHere water_here(World *w, const PlayerMove *m, const UserCmd *cmd) {
    WaterHere h;
    memset(&h, 0, sizeof(h));
    // The command's view tick (identical on the host and the predicting client); the host's own
    // commands carry none: its current tick.
    double tick = cmd->view_tick ? (double)cmd->view_tick / 256.0 : (double)w->tick;
    if (g_water) h.in = g_water(w, m->pos.x, m->pos.z, tick, &h.surface, &h.column, &h.flow);
    if (h.in && h.surface <= m->pos.y) h.in = false;
    return h;
}

static void water_update(PlayerMove *m, const WaterHere *h, float dt) {
    float eye = m->pos.y + player_eye_height(m);
    m->water_depth = h->in ? fmaxf(h->surface - m->pos.y, 0.0f) : 0.0f;
    m->water_surface = h->in ? h->surface : 0.0f;
    m->water_column = h->in ? h->column : 0.0f;
    m->head_under = (u8)(h->in && eye < h->surface - 0.02f);
    if (m->mode != MOVE_SWIM) m->water = (u8)(m->water_depth > 0.05f ? WATER_WADE : WATER_DRY);
    // Breath: runs down with the head under, comes back four times as fast in the air.
    float bmax = T->swim_breath->f;
    m->oxygen = m->head_under ? fmaxf(m->oxygen - dt, 0.0f) : fminf(m->oxygen + dt * 4.0f, bmax);
    // Wetness: soaks up to the waterline's share of the body, dries over ~90 s.
    float soak = saturate(m->water_depth / 1.6f);
    if (m->head_under || m->mode == MOVE_SWIM) soak = 1.0f;
    m->wet = soak > m->wet ? fminf(m->wet + dt * 2.0f, soak) : fmaxf(m->wet - dt / 90.0f, 0.0f);
}

// MOVE_SWIM: at the surface a spring holds the eye pl_swim_eye above the (moving) surface; diving
// moves freely along the view, an idle diver rises slowly. Water drag pulls the velocity relative to
// the flow towards the wished one (quadratic at high speed: a fall into deep water stops within a
// couple of metres). Forward + jump at a wall climbs out through the mantle.
static void swim_tick(World *w, PlayerSim *sim, PhysCharacter *ch, u16 buttons, u16 pressed, vec2 mv, const WaterHere *h, float dt, bool dbg) {
    PlayerMove *m = &sim->move;
    PhysicsWorld *pw = w->phys;
    BodyId self = phys_character_inner_body(ch);
    m->on_ground = 0;
    m->coyote = 0;
    m->air_time = 0;
    m->sprinting = 0;
    m->fall_start_y = m->pos.y;
    float eye_h = PLAYER_EYE_STAND;
    float float_feet = h->surface - (eye_h - T->swim_eye->f);
    float eye_y = m->pos.y + eye_h;
    bool down = (buttons & BTN_CROUCH) != 0, up = (buttons & BTN_JUMP) != 0;
    bool dive = m->water == WATER_UNDER;
    // surface -> dive: crouch, or swimming forward looking well down; dive -> surface: the head is
    // back at the surface and nothing pushes it down.
    if (!dive && (down || (mv.y > 0.3f && m->pitch < -0.6f))) dive = true;
    else if (dive && eye_y > h->surface - 0.12f && !down && !(mv.y > 0.3f && m->pitch < -0.3f) && m->vel.y > -0.5f) dive = false;
    // Fast swim on stamina (surface only).
    bool fast = !dive && (buttons & BTN_SPRINT) && mv.y > 0.3f && m->stamina > 0.0f;
    m->stamina = fast ? fmaxf(m->stamina - dt / T->swim_stamina->f, 0.0f) : fminf(m->stamina + dt / (T->swim_stamina->f * 0.6f), 1.0f);
    m->swim_sprint = (u8)fast;

    vec3 fwd = forward_from_yaw(m->yaw), right = right_from_yaw(m->yaw);
    vec3 rel = v3_sub(m->vel, h->flow);
    float k = fminf(1.0f, T->swim_accel->f * dt);
    if (!dive) {
        float sp = fast ? T->swim_sprint_speed->f : T->swim_speed->f;
        vec3 wish = v3_add(v3_scale(right, mv.x), v3_scale(fwd, mv.y));
        float wl = v3_len(wish);
        if (wl > 1.0f) wish = v3_scale(wish, 1.0f / wl);
        rel.x += (wish.x * sp - rel.x) * k;
        rel.z += (wish.z * sp - rel.z) * k;
        // Held at the surface: a damped spring on the feet height (bobs with the waves).
        float ks = T->swim_spring->f, c = 2.0f * sqrtf(ks) * 0.75f;
        rel.y += (ks * (float_feet - m->pos.y) - c * rel.y) * dt;
    } else {
        vec3 look = dir_from_angles(m->yaw, m->pitch);
        vec3 wish = v3_add(v3_scale(right, mv.x), v3_scale(look, mv.y));
        if (up) wish.y += 1.0f;
        if (down) wish.y -= 1.0f;
        float wl = v3_len(wish);
        if (wl > 1.0f) wish = v3_scale(wish, 1.0f / wl);
        vec3 target = v3_scale(wish, T->swim_dive_speed->f);
        if (!down) target.y += T->swim_rise->f;   // the lungs' air lifts an idle diver
        rel = v3_add(rel, v3_scale(v3_sub(target, rel), k));
        // Never pop out of the water from below: the surface spring takes over at the top.
        if (m->pos.y > float_feet && rel.y > 0.0f) rel.y *= 0.5f;
    }
    // Quadratic drag above swimming speeds: a dive from height loses its speed within a couple of metres.
    float rs = v3_len(rel);
    if (rs > 3.0f) rel = v3_scale(rel, 1.0f / (1.0f + 0.6f * (rs - 3.0f) * dt));
    // Back to world velocity: a swimmer drifts with the current and rides the waves' vertical motion.
    vec3 vel = v3_add(rel, h->flow);

    // Climb out: forward + jump against a ledge, from the surface (the mantle from a swimming pose).
    if (!dive && mv.y > 0.2f && (up || m->jump_buffer > 0.0f) && m->regrab_cooldown <= 0.0f) {
        LedgeResult lr = detect_ledge(pw, m->pos, m->pos.y, m->yaw, self, false, true, dbg);
        if (lr.kind == LEDGE_MANTLE) {
            start_mantle(m, &lr, m->pos);
            m->jump_buffer = 0;
            m->water = WATER_SURFACE;
            return;
        }
        if (lr.kind == LEDGE_HANG) {
            m->mode = MOVE_HANG;
            m->hang_pos = lr.hang_pos;
            m->hang_normal = lr.wall_normal;
            m->ledge_point = lr.top;
            m->hang_time = 0;
            m->vel = v3(0, 0, 0);
            set_hand_anchors(m, lr.wall_point, lr.wall_normal, lr.top.y);
            phys_character_set_pos(ch, lr.hang_pos);
            m->pos = lr.hang_pos;
            m->jump_buffer = 0;
            return;
        }
    }

    CharacterMoveParams mp = { 0.0f, 0.0f, false, false };
    phys_character_update(pw, ch, dt, vel, &mp);
    m->pos = phys_character_pos(ch);
    m->vel = phys_character_vel(ch);
    m->water = (u8)(dive ? WATER_UNDER : WATER_SURFACE);
    // Stroke cycle: faster with speed, a slow scull when idle.
    float hs = v3_len(v3_flat(v3_sub(m->vel, h->flow)));
    m->swim_phase = fmodf(m->swim_phase + dt * (1.6f + hs * 1.9f), 2.0f * DG_PI * 1024.0f);

    // Out of the water: stand up where the water is shallow and the feet are near the bottom.
    float bottom = h->surface - h->column;
    if (!h->in || (h->column < T->swim_exit->f && m->pos.y - bottom < 0.35f)) {
        m->mode = MOVE_WALK;
        m->water = WATER_WADE;
        m->fall_start_y = m->pos.y;
    }
}

void player_move_tick(World *w, int slot, PlayerSim *sim, PhysCharacter *ch, const UserCmd *cmd, float dt, bool first) {
    PlayerMove *m = &sim->move;
    PhysicsWorld *pw = w->phys;
    BodyId self = phys_character_inner_body(ch);
    bool dbg = first && T->debug_mantle->i;
    u16 buttons = cmd->buttons;
    u16 pressed = (u16)(buttons & ~sim->last_buttons);

    m->yaw = cmd->yaw;
    {
        // Body yaw (turn in place): deterministic, predicted locally, replicated by the host.
        const struct AnimLibrary *lib = g_engine.anim;
        float hspeed = v3_len(v3_flat(m->vel));
        m->body_yaw = anim_body_yaw_step(m->body_yaw, m->yaw, hspeed > 0.3f || m->mode != MOVE_WALK, lib ? &lib->proc : NULL, dt, &m->body_turning);
        // Hanging: the body faces the wall whatever the view does (the grips stay on the ledge).
        if (m->mode == MOVE_HANG) { m->body_yaw = player_hang_wall_yaw(m); m->body_turning = 0; }
        // Walk-cycle phase for the held item / camera bob: stepped from the replicated movement, so
        // every machine simulating this player (owner, host, spectators) bobs in step
        // (docs/SPECTATE.md "Determinism audit"). Wrapped at 1024 cycles (exact for bob freq 1).
        m->stride = fmodf(m->stride + dt * (4.5f + hspeed * 1.1f), 2.0f * DG_PI * 1024.0f);
    }
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

    // ---- seated in a vehicle: follow the seat (the vehicle owns the motion)
    if (m->mode == MOVE_SEATED) {
        vec3 feet, vel;
        if (g_seat_pose && g_seat_pose(w, m->vehicle, m->seat, &feet, &vel)) {
            m->pos = feet;
            (void)vel;
            m->vel = v3(0, 0, 0);   // the player is still relative to the seat: no walk bob / sway / run cycle
            phys_character_set_pos(ch, feet);
            phys_character_set_vel(ch, v3(0, 0, 0));
        }
        m->on_ground = 1;
        m->sprinting = 0;
        m->air_time = 0;
        m->fall_start_y = m->pos.y;
        m->coyote = 0;
        // A vehicle under water: the occupants hold their breath (and drown; get out and swim).
        WaterHere swh = water_here(w, m, cmd);
        water_update(m, &swh, dt);
        sim->last_buttons = buttons;
        return;
    }
    // ---- thrown out of a vehicle: tumbling, no control until the stun ends
    if (m->veh_stun > 0.0f) {
        m->veh_stun -= dt;
        float k = phys_character_ground(ch) == GROUND_ON ? expf(-4.0f * dt) : 1.0f;
        m->vel.x *= k;
        m->vel.z *= k;
        m->vel.y -= T->gravity->f * dt;
        CharacterMoveParams mp = { 0.3f, 0.2f, true, true };
        phys_character_update(pw, ch, dt, m->vel, &mp);
        m->pos = phys_character_pos(ch);
        m->vel = phys_character_vel(ch);
        if (phys_character_ground(ch) == GROUND_ON && m->vel.y < 0) m->vel.y = 0;
        sim->last_buttons = buttons;
        return;
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

    // ---- water: depth, state, breath; deep enough water turns walking into swimming
    WaterHere wh = water_here(w, m, cmd);
    if (m->mode != MOVE_NOCLIP) water_update(m, &wh, dt);
    if (m->mode == MOVE_WALK && wh.in && wh.column >= T->swim_enter->f && m->water_depth >= T->swim_enter->f - 0.1f) {
        m->mode = MOVE_SWIM;
        m->water = m->head_under ? WATER_UNDER : WATER_SURFACE;
        if (m->crouched && phys_character_set_crouched(pw, ch, false)) m->crouched = 0;
        m->crouch_toggled = 0;
        m->sprint_toggled = 0;
    }
    if (m->mode == MOVE_SWIM) {
        m->crouch_amount = approachf(m->crouch_amount, 0.0f, dt / 0.16f);
        swim_tick(w, sim, ch, buttons, pressed, mv, &wh, dt, dbg);
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
    if (m->water_depth > T->wade_crouch_max->f) want_crouch = false;   // the head would go under
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
    m->sprinting = (u8)(sprint_input && mv.y > 0.3f && !m->crouched && !(buttons & BTN_AIM) && m->water_depth <= T->wade_sprint_max->f);

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
        m->yaw = player_hang_clamp_yaw(m, m->yaw);   // the client's view is clamped the same way (game.c)
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
    // Wading: slower with depth (knee deep barely, chest deep pl_wade_slow).
    if (m->water_depth > 0.05f) speed *= lerpf(1.0f, T->wade_slow->f, smoothstepf(0.15f, T->swim_enter->f, m->water_depth));

    // Ledge traversal triggers (before jumping so a jump press at a ledge mantles instead).
    bool moving_fwd = mv.y > 0.2f;
    bool jump_wanted = m->jump_buffer > 0.0f;
    if (moving_fwd && !m->crouched && m->regrab_cooldown <= 0) {
        // Vaults and mantles only on a jump press (never automatic from sprinting into a table).
        bool try_ground = on_ground && jump_wanted;
        bool try_air = !on_ground && (buttons & BTN_JUMP) && vel.y < 3.0f;
        if (try_ground || try_air) {
            bool allow_vault = on_ground;
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
        vel.y = T->jump_speed->f * (m->crouched ? 0.8f : 1.0f) * lerpf(1.0f, 0.55f, saturate(m->water_depth / 1.2f));
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
        m->land_tick = cmd->seq;   // the command, not the machine's world tick: identical on every machine
    }
    if (!m->on_ground && was_ground && !jumped) m->fall_start_y = m->pos.y;
    (void)slot;
    sim->last_buttons = buttons;
}
