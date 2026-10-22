// Vehicles, game side (docs/VEHICLES.md): spawning (level "vehicles", spawn_vehicle), seats
// (enter / exit through clear exit sockets, seat change, ejection at speed), driving (host:
// Jolt VehicleConstraint fed from the driver's UserCmds; the driving client predicts its vehicle
// locally and reconciles against the host's state), fuel, tires (punctures from wheel hits, grip,
// pull, rims), damage (impacts, running over), replication (a vehicle block in every snapshot),
// presentation (body / wheels / steering wheel models, hands glued to the controls, seated pose,
// headlights, exhaust / dust / skids / sparks, engine audio), the vehicle camera, the HUD panel,
// console commands and the in-game tests.
#include "game_internal.h"
#include "player/camera_fp.h"
#include "game_water.h"
#include "physics/buoyancy.h"
#include "gameplay/vehicle_def.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "gameplay/characters.h"
#include "gameplay/weapons.h"
#include "gameplay/events.h"
#include "gameplay/zombies/horde.h"
#include "physics/physics.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "assets/assets.h"
#include "audio/audio.h"
#include "vfx/vfx.h"
#include "vfx/vfx_sim.h"
#include "vfx/vfx_library.h"
#include "net/bitbuf.h"
#include "net/replication.h"
#include "net/net.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui/gui/gui.h"
#include "ui/gui/gui_draw.h"
#include "ui/gui/gui_theme.h"
#include "ui/gui/gui_layout.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/platform.h"
#include "cJSON.h"
#include "world/level.h"
#include "world/openworld.h"
#include "world/mapgen_poi.h"
#include "editor/handpose/hp_system.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define MAX_VEH 16
#define VEH_HIST 64
#define VEH_PRED 128
#define MAX_SPIKES 16

typedef struct Vehicle {
    bool used;
    EntityId ent;
    int def;
    PhysVehicle *pv;              // host: the simulation; client: the locally predicted vehicle
    bool predicted;               // client: this machine drives it
    BodyId proxy[VEH_MAX_BOXES];  // client: kinematic stand-ins (collisions, wheel rays)
    int proxy_n;
    // replicated state
    float fuel;
    TireState tire[VEH_MAX_WHEELS];
    u8 occ[VEH_MAX_SEATS];        // player slot + 1
    float steer;                  // -1..1 (smoothed, clamped)
    float rpm;
    int gear;
    float susp[VEH_MAX_WHEELS];
    bool engine, lights, braking, handbrake, wrecked, onehand, horn;
    float flood;                  // 0..1 cabin full of water (docs/WATER.md "Vehicles")
    bool drowned;                 // the engine took water: dead until the cabin drains
    vec3 vel, angvel, prev_vel;
    // host: water
    float intake_t;               // s the engine intake has been under water
    // host: driver input
    float in_throttle, in_steer, in_hand, in_brake;   // in_brake: tests
    u8 in_lights_prev, in_horn_prev;
    float impact_cd;
    u32 hit_ent[8];
    u32 hit_tick[8];
    // host: lag compensation
    vec3 hpos[VEH_HIST];
    quat hrot[VEH_HIST];
    u32 htick[VEH_HIST];
    // client: last host state, prediction history, render interpolation
    vec3 s_pos, s_vel, s_ang;
    quat s_rot;
    struct { u32 seq; vec3 pos; quat rot; vec3 vel; } pred[VEH_PRED];
    vec3 pp_prev, pp_cur;
    quat pr_prev, pr_cur;
    vec3 corr;                    // visual correction offset (decays)
    // presentation
    float wheel_rot[VEH_MAX_WHEELS];
    float dust_acc[VEH_MAX_WHEELS], exhaust_acc, smoke_acc;
    vec3 skid_last[VEH_MAX_WHEELS];
    bool skid_on[VEH_MAX_WHEELS];
    float skid_amount;
    int ground_mat;               // audio: the surface under the wheels this frame (tire noise)
    float aud_susp[VEH_MAX_WHEELS], aud_flat[VEH_MAX_WHEELS];   // audio: suspension last frame, flat-tire rotation phase
    float steer_vis;
    bool test_steer_override;
} Vehicle;

typedef struct SpikeZone { float x, z, hw, hd; } SpikeZone;

typedef struct VehicleSys {
    Vehicle v[MAX_VEH];
    SpikeZone spikes[MAX_SPIKES];
    int spike_count;
    struct { float t; EntityId ent; } eject[MAX_PLAYERS];
} VehicleSys;

static struct {
    VehicleLib lib;
    bool lib_ok;
    u16 m_body[2], m_wheel[2], m_steer[2];
    Cvar *debug, *fuel_scale, *exit_speed_k;
    // local presentation
    bool cam_tp;
    bool have_prev_yaw;
    float prev_veh_yaw;
    EntityId view_veh;
    bool req_vehicle, lights;
    u8 req_seat;
    int runover_hits, runover_kills;
    // prediction stats
    float pred_err, pred_err_max, pred_err_sum;
    int pred_err_n, pred_snaps;
    // test
    struct {
        int mode, step, frames, fails, checks;
        bool quit;
        EntityId veh;
        float val[8];
        char name[16];
    } t;
} VG;

static void free_sys(World *w);
static bool vehicle_enter(World *w, int slot, struct Vehicle *v, int seat);
static bool vehicle_exit(World *w, int slot, bool force);

static VehicleSys *sys(World *w) {
    if (!w) return NULL;
    if (!w->vehicles) {
        w->vehicles = calloc(1, sizeof(VehicleSys));
        w->vehicles_free = free_sys;
    }
    return (VehicleSys *)w->vehicles;
}

static void ensure_lib(void) {
    if (VG.lib_ok) return;
    vehicle_lib_load(&VG.lib, "assets/vehicles");
    static const char *body[2] = { "car_body", "moto_body" }, *wheel[2] = { "car_wheel", "moto_wheel" }, *steer[2] = { "car_steering", "moto_bar" };
    for (int k = 0; k < 2; k++) {
        VG.m_body[k] = assets_model_id(body[k]);
        VG.m_wheel[k] = assets_model_id(wheel[k]);
        VG.m_steer[k] = assets_model_id(steer[k]);
    }
    VG.lib_ok = true;
}

static const VehicleDef *vdef(const Vehicle *v) { return &VG.lib.defs[DG_CLAMP(v->def, 0, VG.lib.count - 1)]; }

static Vehicle *veh_of(World *w, EntityId e) {
    VehicleSys *s = w ? (VehicleSys *)w->vehicles : NULL;
    if (!s || !e) return NULL;
    for (int i = 0; i < MAX_VEH; i++) if (s->v[i].used && s->v[i].ent == e) return &s->v[i];
    return NULL;
}

static Vehicle *veh_alloc(World *w, EntityId e) {
    VehicleSys *s = sys(w);
    for (int i = 0; i < MAX_VEH; i++)
        if (!s->v[i].used) {
            Vehicle *v = &s->v[i];
            memset(v, 0, sizeof(*v));
            v->used = true;
            v->ent = e;
            for (int k = 0; k < VEH_MAX_WHEELS; k++) tire_init(&v->tire[k]);
            v->pr_prev = v->pr_cur = v->s_rot = quat_identity();
            return v;
        }
    return NULL;
}

static void proxies_destroy(World *w, Vehicle *v) {
    for (int k = 0; k < v->proxy_n; k++) phys_remove_body(w->phys, v->proxy[k]);
    v->proxy_n = 0;
}

static void veh_release(World *w, Vehicle *v) {
    if (v->pv) {
        game_water_buoy_detach(phys_vehicle_body(v->pv));
        phys_vehicle_destroy(w->phys, v->pv);
    }
    v->pv = NULL;
    proxies_destroy(w, v);
    v->used = false;
}

static void free_sys(World *w) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    if (!s) return;
    for (int i = 0; i < MAX_VEH; i++) if (s->v[i].used) veh_release(w, &s->v[i]);
    free(s);
    w->vehicles = NULL;
}

// ------------------------------------------------------------------ poses

// Simulation pose (host: the body; driving client: its prediction; others: the replicated entity).
static void pose_sim(World *w, const Vehicle *v, vec3 *p, quat *r) {
    if (v->pv) phys_get_transform(w->phys, phys_vehicle_body(v->pv), p, r);
    else {
        int i = (int)ENT_INDEX(v->ent);
        *p = w->xf[i].pos;
        *r = w->xf[i].rot;
    }
}

// Render pose (interpolated between ticks; the predicted vehicle with its correction offset).
static void pose_render(World *w, const Vehicle *v, vec3 *p, quat *r) {
    float a = G ? saturate(G->alpha) : 1.0f;
    int i = (int)ENT_INDEX(v->ent);
    if (w->authority) {
        *p = v3_lerp(w->xf[i].prev_pos, w->xf[i].pos, a);
        *r = quat_nlerp(w->xf[i].prev_rot, w->xf[i].rot, a);
    } else if (v->predicted) {
        *p = v3_add(v3_lerp(v->pp_prev, v->pp_cur, a), v->corr);
        *r = quat_nlerp(v->pr_prev, v->pr_cur, a);
    } else {
        *p = w->xf[i].pos;
        *r = w->xf[i].rot;
    }
}

static vec3 to_world(vec3 p, quat r, vec3 local) { return v3_add(p, quat_rotate(r, local)); }

static int seat_of_slot(const Vehicle *v, int slot) {
    for (int s = 0; s < VEH_MAX_SEATS; s++) if (v->occ[s] == slot + 1) return s;
    return -1;
}

static Vehicle *veh_of_slot(World *w, int slot, int *seat) {
    VehicleSys *s = w ? (VehicleSys *)w->vehicles : NULL;
    if (!s || slot < 0) return NULL;
    for (int i = 0; i < MAX_VEH; i++) {
        if (!s->v[i].used) continue;
        int k = seat_of_slot(&s->v[i], slot);
        if (k >= 0) { if (seat) *seat = k; return &s->v[i]; }
    }
    return NULL;
}

static float front_steer_deg(const VehicleDef *d) {
    float m = 0;
    for (int i = 0; i < d->wheel_count; i++) m = fmaxf(m, d->wheels[i].max_steer_deg);
    return m;
}

// Steering wheel / handlebar frame: Y = steering axis, X = the vehicle's right (orthogonalized),
// rotated about the axis by the steering angle (clockwise for the driver when turning right). The
// driver case's anchor (docs/HANDPOSE.md, vehicle_<def>_driver) moves the whole frame: the drawn
// wheel / handlebar and both grips on it follow.
static void steer_frame(const VehicleDef *d, vec3 p, quat r, float steer, vec3 *pivot, vec3 *x, vec3 *y, vec3 *z) {
    Xform an = handpose_seat_anchor(d->name, true);
    quat ar = quat_mul(quat_mul(r, an.r), quat_conj(r));   // the anchor rotation, world space
    vec3 axis = v3_norm(quat_rotate(ar, quat_rotate(r, d->steer_axis)));
    vec3 xr = quat_rotate(ar, quat_rotate(r, v3(1, 0, 0)));
    xr = v3_norm(v3_sub(xr, v3_scale(axis, v3_dot(xr, axis))));
    float a = -steer * front_steer_deg(d) * DG_DEG2RAD * d->steer_ratio;
    quat q = quat_axis_angle(axis, a);
    *x = quat_rotate(q, xr);
    *y = axis;
    *z = v3_cross(*x, axis);
    *pivot = to_world(p, r, v3_add(d->steer_pivot, an.t));
}

// Rotation whose columns are the orthonormal basis x, y, z.
static quat quat_basis(vec3 x, vec3 y, vec3 z) {
    float tr = x.x + y.y + z.z;
    quat q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (y.z - z.y) / s; q.y = (z.x - x.z) / s; q.z = (x.y - y.x) / s;
    } else if (x.x > y.y && x.x > z.z) {
        float s = sqrtf(1.0f + x.x - y.y - z.z) * 2.0f;
        q.w = (y.z - z.y) / s; q.x = 0.25f * s; q.y = (y.x + x.y) / s; q.z = (z.x + x.z) / s;
    } else if (y.y > z.z) {
        float s = sqrtf(1.0f + y.y - x.x - z.z) * 2.0f;
        q.w = (z.x - x.z) / s; q.x = (y.x + x.y) / s; q.y = 0.25f * s; q.z = (z.y + y.z) / s;
    } else {
        float s = sqrtf(1.0f + z.z - x.x - y.y) * 2.0f;
        q.w = (x.y - y.x) / s; q.x = (z.x + x.z) / s; q.y = (z.y + y.z) / s; q.z = 0.25f * s;
    }
    return quat_norm(q);
}

static void grips_world(World *w, const Vehicle *v, vec3 out[2], quat rot[2]) {
    const VehicleDef *d = vdef(v);
    vec3 p, pv, x, y, z;
    quat r;
    pose_render(w, v, &p, &r);
    steer_frame(d, p, r, v->steer_vis, &pv, &x, &y, &z);
    // Hand frame convention (as the ledge grabs): identity = palm down (-Y), fingers forward (-Z).
    for (int h = 0; h < 2; h++) {
        float s = h ? 1.0f : -1.0f;
        vec3 u = v3_scale(x, s);   // pivot -> grip (turns with the wheel)
        out[h] = v3_madd(pv, u, d->steer_radius);
        vec3 ly, lz;
        if (d->kind == VK_MOTO) {
            // Handlebar: palms over the grips, fingers wrapping forward round the bar.
            ly = y;                          // back of the hand up the fork axis
            lz = v3_norm(v3_cross(x, ly));   // fingers along -lz: forward
        } else {
            // Steering wheel at 9 and 3: palms towards the hub, fingers wrapping forward round the
            // rim (away from the driver), thumbs along the rim.
            ly = u;                          // -ly = palm normal = towards the centre
            lz = y;                          // the wheel axis points at the driver: fingers forward
        }
        vec3 lx = v3_norm(v3_cross(ly, lz));
        lz = v3_cross(lx, ly);
        rot[h] = quat_basis(lx, ly, lz);
    }
}

// ------------------------------------------------------------------ hooks (movement, characters, hands, weapons)

static bool seat_pose_move(World *w, u32 vehicle, int seat, vec3 *feet, vec3 *vel) {
    Vehicle *v = veh_of(w, vehicle);
    if (!v) return false;
    const VehicleDef *d = vdef(v);
    seat = DG_CLAMP(seat, 0, d->seat_count - 1);
    vec3 p;
    quat r;
    pose_sim(w, v, &p, &r);
    vec3 eye = to_world(p, r, v3_add(d->seats[seat].pos, d->seats[seat].eye));
    *feet = v3_sub(eye, v3(0, PLAYER_EYE_STAND, 0));
    *vel = v->vel;
    return true;
}

static bool seat_pose_char(World *w, int slot, vec3 *hip, vec3 feet[2], float *yaw) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    if (!v) return false;
    const VehicleDef *d = vdef(v);
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    const VehicleSeatDef *sd = &d->seats[seat];
    *hip = to_world(p, r, sd->pos);
    if (sd->driver) {
        feet[0] = to_world(p, r, d->pedal_l);
        feet[1] = to_world(p, r, d->pedal_r);
    } else if (d->kind == VK_MOTO) {
        feet[0] = to_world(p, r, v3(sd->pos.x - 0.18f, 0.43f, sd->pos.z - 0.02f));
        feet[1] = to_world(p, r, v3(sd->pos.x + 0.18f, 0.43f, sd->pos.z - 0.02f));
    } else {
        feet[0] = to_world(p, r, v3(sd->pos.x - 0.11f, sd->pos.y - 0.34f, sd->pos.z - 0.5f));
        feet[1] = to_world(p, r, v3(sd->pos.x + 0.11f, sd->pos.y - 0.34f, sd->pos.z - 0.5f));
    }
    *yaw = quat_get_yaw(r);
    return true;
}

static bool slot_onehand(World *w, int slot, const Vehicle *v) {
    if (w->players[slot].local || w->authority) return w->players[slot].sim.move.veh_onehand != 0;
    return v->onehand;
}

static bool seat_hands(World *w, int slot, vec3 pos[2], quat rot[2], bool use[2]) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    if (!v) return false;
    if (!vdef(v)->seats[seat].driver) {
        // Passengers: hands rest on the thighs, only once the passenger case has a pose (its anchor
        // and keys place them relative to these points).
        u32 pc = handpose_seat_case(w, slot);
        const HandPoseCase *c = handpose_case_by_hash(pc);
        if (!c || !c->key_count) return false;
        vec3 p;
        quat r;
        pose_render(w, v, &p, &r);
        const VehicleSeatDef *sd = &vdef(v)->seats[seat];
        for (int h = 0; h < 2; h++) {
            // One base point (the lap) for both hands: the case's anchor and keys place each hand.
            pos[h] = to_world(p, r, v3_add(sd->pos, v3(0, 0.06f, -0.28f)));
            rot[h] = r;   // palm down, fingers forward
            use[h] = true;
        }
        return true;
    }
    grips_world(w, v, pos, rot);
    // Both hands on the controls; only the gun hand (right) lets go, and only with a gun in it.
    const WeaponDef *wd = weapon_equipped_def(w, slot);
    bool gun = wd && wd->kind != WPN_MELEE;
    use[0] = true;
    use[1] = !(gun && slot_onehand(w, slot, v));
    return true;
}

// ------------------------------------------------------------------ hand pose editor (simulated vehicle cases)

bool vehicles_editor_seat(World *w, EntityId veh, int slot, int seat) {
    Vehicle *v = veh_of(w, veh);
    return v && vehicle_enter(w, slot, v, seat);
}

void vehicles_editor_unseat(World *w, int slot) { vehicle_exit(w, slot, true); }

bool vehicles_steer_base(World *w, int slot, Xform *base) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    if (!v) return false;
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    *base = xf_make(to_world(p, r, vdef(v)->steer_pivot), r);
    return true;
}

bool vehicles_seat_frame(World *w, int slot, Xform *out) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    if (!v) return false;
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    *out = xf_make(to_world(p, r, v3_add(vdef(v)->seats[seat].pos, v3(0, 0.06f, -0.28f))), r);
    return true;
}

static bool seat_driver(World *w, int slot) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    return v && vdef(v)->seats[seat].driver;
}

static const char *seat_vehicle(World *w, int slot) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    return v ? vdef(v)->name : NULL;
}

static bool vehicle_xform(World *w, int slot, Xform *out) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    if (!v) return false;
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    *out = xf_make(p, r);
    return true;
}

static bool holstered(World *w, int slot) {
    int seat;
    Vehicle *v = veh_of_slot(w, slot, &seat);
    return v && vdef(v)->seats[seat].driver && !slot_onehand(w, slot, v);
}

static bool hook_can_shoot(World *w, int slot, const PlayerSim *sim, const WeaponDef *d) {
    int seat;
    Vehicle *v = veh_of(w, sim->move.vehicle);
    if (!v) return false;
    seat = DG_CLAMP((int)sim->move.seat, 0, vdef(v)->seat_count - 1);
    const VehicleSeatDef *sd = &vdef(v)->seats[seat];
    (void)slot;
    if (sd->driver) return sim->move.veh_onehand && d && d->one_hand_capable;
    return sd->can_shoot;
}

static float hook_spread(World *w, int slot) {
    const PlayerMove *m = &w->players[slot].sim.move;
    Vehicle *v = veh_of(w, m->vehicle);
    if (!v) return 1.0f;
    const VehicleDef *d = vdef(v);
    float speed = v3_len(v->vel);
    float motion = 1.0f + fminf(speed / 25.0f, 1.0f) * 0.4f;   // shooting from a moving vehicle
    return (m->veh_onehand ? d->one_hand_spread_mult : d->passenger_spread_mult) * motion;
}

static BodyId hook_ignore(World *w, int slot) {
    Vehicle *v = veh_of(w, w->players[slot].sim.move.vehicle);
    if (!v) return PHYS_INVALID_BODY;
    if (v->pv) return phys_vehicle_body(v->pv);
    return v->proxy_n ? v->proxy[0] : PHYS_INVALID_BODY;
}

// Ray vs a wheel cylinder (axle along ax): smallest t > 0 or -1.
static float ray_cyl(vec3 o, vec3 d, vec3 c, vec3 ax, float r, float hw) {
    vec3 oc = v3_sub(o, c);
    vec3 dp = v3_sub(d, v3_scale(ax, v3_dot(d, ax))), op = v3_sub(oc, v3_scale(ax, v3_dot(oc, ax)));
    float A = v3_dot(dp, dp), B = 2.0f * v3_dot(dp, op), C = v3_dot(op, op) - r * r;
    float best = -1;
    if (A > 1e-8f) {
        float disc = B * B - 4 * A * C;
        if (disc >= 0) {
            float sq = sqrtf(disc);
            for (int k = 0; k < 2; k++) {
                float t = (-B + (k ? sq : -sq)) / (2 * A);
                if (t <= 0) continue;
                float h = v3_dot(v3_add(oc, v3_scale(d, t)), ax);
                if (fabsf(h) <= hw && (best < 0 || t < best)) best = t;
            }
        }
    }
    // Side discs.
    float dn = v3_dot(d, ax);
    if (fabsf(dn) > 1e-6f)
        for (int k = 0; k < 2; k++) {
            float t = ((k ? hw : -hw) - v3_dot(oc, ax)) / dn;
            if (t <= 0) continue;
            vec3 q = v3_add(oc, v3_scale(d, t));
            vec3 qp = v3_sub(q, v3_scale(ax, v3_dot(q, ax)));
            if (v3_dot(qp, qp) <= r * r && (best < 0 || t < best)) best = t;
        }
    return best;
}

static bool hist_pose(const Vehicle *v, u32 tick, vec3 *p, quat *r) {
    const int k = (int)(tick % VEH_HIST);
    if (v->htick[k] != tick) return false;
    *p = v->hpos[k];
    *r = v->hrot[k];
    return true;
}

static vec3 wheel_center_local(const Vehicle *v, int i) {
    const VehicleDef *d = vdef(v);
    const VehicleWheelDef *wd = &d->wheels[i];
    float k = tire_radius_scale(&v->tire[i]);
    return v3(wd->pos.x, wd->pos.y - v->susp[i] - wd->radius * (1.0f - k), wd->pos.z);
}

static int hook_ray_tires(World *w, vec3 o, vec3 dir, float max_t, u32 view_tick, EntityId *veh, float *t_out) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    if (!s) return -1;
    int best_w = -1;
    float best = max_t;
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used || !ent_valid(w, v->ent)) continue;
        const VehicleDef *d = vdef(v);
        vec3 p;
        quat r;
        if (!(view_tick && hist_pose(v, view_tick, &p, &r))) pose_sim(w, v, &p, &r);
        if (v3_dist(p, o) > max_t + 4.0f) continue;
        vec3 ax = quat_rotate(r, v3(1, 0, 0));
        for (int k = 0; k < d->wheel_count; k++) {
            vec3 c = to_world(p, r, wheel_center_local(v, k));
            float rad = d->wheels[k].radius * tire_radius_scale(&v->tire[k]);
            float t = ray_cyl(o, dir, c, ax, rad, d->wheels[k].width * 0.5f);
            if (t > 0 && t < best) { best = t; best_w = k; *veh = v->ent; }
        }
    }
    if (best_w >= 0) *t_out = best;
    return best_w;
}

static void hook_tire_hit(World *w, EntityId veh, int wheel, vec3 point, vec3 dir) {
    (void)dir;
    Vehicle *v = veh_of(w, veh);
    if (!v || !w->authority || wheel < 0 || wheel >= vdef(v)->wheel_count) return;
    bool was_flat = tire_on_rim(&v->tire[wheel]);
    bool burst = tire_hit(&v->tire[wheel], vdef(v));
    ev_sound(w, burst && !was_flat ? "tire_burst" : "impact_soft", point, burst ? 1.0f : 0.6f, EVENT_ALL);
    if (VG.debug->i) log_info("vehicle %u: tire %d hit: pressure %.2f health %.2f leak %.3f%s", v->ent, wheel, v->tire[wheel].pressure, v->tire[wheel].health,
                              v->tire[wheel].leak, burst ? " BURST" : "");
}

// ------------------------------------------------------------------ spawning

static void build_desc(const VehicleDef *d, vec3 pos, quat rot, u64 user, PhysVehicleDesc *pd) {
    memset(pd, 0, sizeof(*pd));
    pd->kind = d->kind == VK_MOTO ? 1 : 0;
    pd->pos = pos;
    pd->rot = rot;
    pd->mass = d->mass;
    pd->box_count = d->box_count;
    for (int i = 0; i < d->box_count; i++) { pd->box_center[i] = d->box_center[i]; pd->box_half[i] = d->box_half[i]; }
    pd->com_offset = d->com_offset;
    pd->wheel_count = d->wheel_count;
    for (int i = 0; i < d->wheel_count; i++) {
        const VehicleWheelDef *wd = &d->wheels[i];
        PhysWheelDesc *o = &pd->wheels[i];
        o->pos = wd->pos;
        o->radius = wd->radius;
        o->width = wd->width;
        o->susp_min = wd->susp_min;
        o->susp_max = wd->susp_max;
        o->susp_freq = wd->susp_freq;
        o->susp_damp = wd->susp_damp;
        o->max_steer_deg = wd->max_steer_deg;
        o->max_brake = wd->max_brake;
        o->max_handbrake = wd->max_handbrake;
    }
    // Driven axles: pairs of driven wheels (a motorcycle's driven rear wheel alone).
    pd->diff_left[0] = pd->diff_left[1] = -1;
    int axles = 0;
    if (d->kind == VK_MOTO) {
        for (int i = 0; i < d->wheel_count; i++) if (d->wheels[i].driven) { pd->diff_left[0] = pd->diff_right[0] = i; break; }
    } else {
        for (int a = 0; a < 2 && a * 2 + 1 < d->wheel_count; a++)
            if (d->wheels[a * 2].driven || d->wheels[a * 2 + 1].driven) {
                pd->diff_left[axles] = a * 2;
                pd->diff_right[axles] = a * 2 + 1;
                axles++;
            }
    }
    pd->diff_ratio = d->diff_ratio;
    pd->max_torque = d->max_torque;
    pd->min_rpm = d->min_rpm;
    pd->max_rpm = d->max_rpm;
    for (int i = 0; i < d->gear_count && i < 8; i++) pd->gear_ratios[i] = d->gears[i];
    pd->gear_count = DG_MIN(d->gear_count, 8);
    pd->reverse_ratio = d->reverse_ratio;
    pd->max_lean_deg = d->max_lean_deg;
    pd->user = user;
    pd->material = SURF_METAL_THIN;
}

EntityId vehicle_spawn(World *w, const char *name, vec3 pos, float yaw) {
    ensure_lib();
    if (!w || !w->authority) return ENT_NONE;
    int di = vehicle_lib_index(&VG.lib, name);
    if (di < 0) { log_info("spawn_vehicle: unknown vehicle '%s' (car, moto)", name); return ENT_NONE; }
    const VehicleDef *d = &VG.lib.defs[di];
    EntityId e = ent_spawn(w, ET_VEHICLE);
    if (!e) return ENT_NONE;
    Vehicle *v = veh_alloc(w, e);
    if (!v) { ent_destroy(w, e); return ENT_NONE; }
    int i = (int)ENT_INDEX(e);
    quat rot = quat_yaw(yaw);
    w->xf[i].pos = w->xf[i].prev_pos = pos;
    w->xf[i].rot = w->xf[i].prev_rot = rot;
    w->item[i] = (u16)di;
    w->comps[i] |= C_HEALTH;
    health_init(&w->health[i], d->hp, 0);
    snprintf(w->names[i], sizeof(w->names[i]), "%s", d->name);
    w->vis[i].half = d->box_half[0];
    entity_setup(w, e);
    w->inter[i].range = 3.6f;
    v->def = di;
    v->fuel = d->fuel_capacity * 0.6f;
    PhysVehicleDesc pd;
    build_desc(d, pos, rot, e, &pd);
    v->pv = phys_vehicle_create(w->phys, &pd);
    log_info("vehicle: %s spawned at %.1f %.1f %.1f (entity %u)", d->display, pos.x, pos.y, pos.z, e);
    return e;
}

static void spawn_pump(World *w, vec3 pos, float yaw) {
    EntityId e = ent_spawn(w, ET_FUEL_PUMP);
    if (!e) return;
    int i = (int)ENT_INDEX(e);
    w->xf[i].pos = w->xf[i].prev_pos = v3_add(pos, v3(0, 0.75f, 0));
    w->xf[i].rot = w->xf[i].prev_rot = quat_yaw(yaw);
    w->vis[i].shape = VIS_BOX;
    w->vis[i].half = v3(0.3f, 0.75f, 0.22f);
    w->vis[i].color = 0xFF2A2AC8;   // red pump
    w->comps[i] |= C_VISUAL;
    snprintf(w->names[i], sizeof(w->names[i]), "fuel_pump");
    entity_setup(w, e);
    w->inter[i].range = 2.4f;
}

// ------------------------------------------------------------------ seats

static bool exit_clear(World *w, const Vehicle *v, vec3 p, quat r, vec3 local, vec3 *feet) {
    vec3 x = to_world(p, r, local);
    BodyId ign = v->pv ? phys_vehicle_body(v->pv) : (v->proxy_n ? v->proxy[0] : PHYS_INVALID_BODY);
    RayHit rh;
    vec3 top = v3_add(x, v3(0, 1.2f, 0));
    if (phys_raycast(w->phys, top, v3(0, -3.5f, 0), PL_MASK_WORLD, ign, &rh)) x.y = rh.point.y;
    // Never through the vehicle: the path from the seat to the exit must be open (door side).
    if (phys_capsule_overlap(w->phys, v3_add(x, v3(0, 0.06f, 0)), PLAYER_RADIUS, PLAYER_HEIGHT, PL_MASK_WORLD, PHYS_INVALID_BODY)) return false;
    *feet = x;
    return true;
}

static bool vehicle_enter(World *w, int slot, Vehicle *v, int seat) {
    Player *p = &w->players[slot];
    PlayerMove *m = &p->sim.move;
    if (!p->active || m->mode != MOVE_WALK || m->veh_stun > 0) {
        if (VG.debug->i) log_info("vehicle %u: player %d can't enter (mode %d, stun %.2f)", v->ent, slot, m->mode, m->veh_stun);
        return false;
    }
    const VehicleDef *d = vdef(v);
    if (seat < 0) {
        for (int s = 0; s < d->seat_count && seat < 0; s++) if (!v->occ[s]) seat = s;
    }
    if (seat < 0 || seat >= d->seat_count || v->occ[seat]) { ev_hint(w, slot, "%s: no free seat", d->display); return false; }
    v->occ[seat] = (u8)(slot + 1);
    if (p->character) {
        phys_character_set_crouched(w->phys, p->character, false);
        phys_set_body_enabled(w->phys, phys_character_inner_body(p->character), false);
    }
    m->mode = MOVE_SEATED;
    m->vehicle = v->ent;
    m->seat = (u8)seat;
    m->veh_onehand = 0;
    m->crouched = 0;
    m->crouch_amount = 0;
    vec3 vp;
    quat vr;
    pose_sim(w, v, &vp, &vr);
    ev_sound(w, "vehicle_door", to_world(vp, vr, d->seats[seat].pos), 0.8f, EVENT_ALL);
    if (d->seats[seat].driver && !v->wrecked && v->fuel > 0) ev_sound(w, "engine_start", vp, 0.9f, EVENT_ALL);
    if (VG.debug->i) log_info("vehicle %u: player %d -> seat %d", v->ent, slot, seat);
    return true;
}

static bool vehicle_exit(World *w, int slot, bool force) {
    Player *p = &w->players[slot];
    PlayerMove *m = &p->sim.move;
    Vehicle *v = veh_of(w, m->vehicle);
    if (m->mode != MOVE_SEATED) return false;
    int seat = m->seat;
    vec3 feet = m->pos;
    bool ok = false;
    if (v) {
        const VehicleDef *d = vdef(v);
        vec3 vp;
        quat vr;
        pose_sim(w, v, &vp, &vr);
        // The own door first, then the other seats' exits, then over the roof.
        ok = exit_clear(w, v, vp, vr, d->seats[seat].exit, &feet);
        for (int s = 0; s < d->seat_count && !ok; s++) if (s != seat) ok = exit_clear(w, v, vp, vr, d->seats[s].exit, &feet);
        if (!ok) {
            float roof = 0;
            for (int b = 0; b < d->box_count; b++) roof = fmaxf(roof, d->box_center[b].y + d->box_half[b].y);
            ok = exit_clear(w, v, vp, vr, v3(d->seats[seat].pos.x, roof + 0.05f, d->seats[seat].pos.z), &feet);
        }
        if (!ok && !force) { ev_hint(w, slot, "No room to get out"); return false; }
        if (!ok) feet = to_world(vp, vr, d->seats[seat].exit);
        v->occ[seat] = 0;
        ev_sound(w, "vehicle_door", feet, 0.8f, EVENT_ALL);
    }
    m->mode = MOVE_WALK;
    m->vehicle = 0;
    m->seat = 0;
    m->veh_onehand = 0;
    m->pos = feet;
    vec3 vel = v ? v->vel : v3(0, 0, 0);
    m->vel = vel;
    if (p->character) {
        phys_set_body_enabled(w->phys, phys_character_inner_body(p->character), true);
        phys_character_set_pos(p->character, feet);
        phys_character_set_vel(p->character, vel);
    }
    w->flags[ENT_INDEX(p->ent)] &= ~(u32)EF_SEATED;
    // Bailing out at speed: thrown, a ragdoll tumble and damage.
    float speed = v3_len(vel);
    float lim = v ? vdef(v)->exit_ragdoll_speed : 6.0f;
    if (speed > lim) {
        m->veh_stun = fminf(1.2f + speed * 0.06f, 3.0f);
        apply_damage(w, p->ent, (speed - lim) * 4.0f, DMG_FALL, p->ent, feet, v3_scale(vel, 1.0f / speed), false);
        GameEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_VEHICLE;
        ev.target = EVENT_ALL;
        ev.except = EVENT_ALL;
        ev.ent = p->ent;
        ev.id = 1;   // eject
        ev.value = m->veh_stun;
        ev.dir = vel;
        ev.pos = feet;
        ev_push(w, &ev);
    }
    if (VG.debug->i) log_info("vehicle: player %d out at %.2f %.2f %.2f (%.1f m/s)", slot, feet.x, feet.y, feet.z, speed);
    return true;
}

static Vehicle *nearest_vehicle(World *w, vec3 pos, float max_d, float *dist) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    Vehicle *best = NULL;
    float bd = max_d;
    for (int i = 0; s && i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used || !ent_valid(w, v->ent)) continue;
        vec3 p;
        quat r;
        pose_sim(w, v, &p, &r);
        // Distance to the body box (not the centre): a long car is reachable from its doors.
        vec3 l = quat_rotate(quat_conj(r), v3_sub(pos, p));
        vec3 h = vdef(v)->box_half[0], c = vdef(v)->box_center[0];
        vec3 q = v3(fmaxf(fabsf(l.x - c.x) - h.x, 0), fmaxf(fabsf(l.y - c.y) - h.y, 0), fmaxf(fabsf(l.z - c.z) - h.z, 0));
        float d = v3_len(q);
        if (d < bd) { bd = d; best = v; }
    }
    if (dist) *dist = bd;
    return best;
}

// Host: one UserCmd of a player (enter / exit, seat change, driver input). The predicting client
// runs it too but only for the parts that are deterministic from the command (one-hand flag).
void vehicles_player_cmd(World *w, int slot, const UserCmd *cmd, u16 pressed, bool first) {
    (void)first;
    if (!w->vehicles) return;
    Player *p = &w->players[slot];
    PlayerMove *m = &p->sim.move;
    Vehicle *v = m->mode == MOVE_SEATED ? veh_of(w, m->vehicle) : NULL;
    if (w->authority) {
        if (pressed & BTN_VEHICLE) {
            if (m->mode == MOVE_SEATED) vehicle_exit(w, slot, false);
            else {
                Vehicle *t = veh_of(w, cmd->use_target);
                float dist = 0;
                if (!t) t = nearest_vehicle(w, m->pos, 1.6f, &dist);
                if (t) vehicle_enter(w, slot, t, -1);
            }
            v = m->mode == MOVE_SEATED ? veh_of(w, m->vehicle) : NULL;
        }
        if (v && cmd->seat_req) {
            int want = cmd->seat_req - 1;
            const VehicleDef *d = vdef(v);
            if (want >= 0 && want < d->seat_count && want != m->seat && !v->occ[want]) {
                v->occ[m->seat] = 0;
                v->occ[want] = (u8)(slot + 1);
                m->seat = (u8)want;
                m->veh_onehand = 0;
            }
        }
    }
    if (!v) { m->veh_onehand = 0; return; }
    const VehicleDef *d = vdef(v);
    bool driver = d->seats[DG_CLAMP(m->seat, 0, d->seat_count - 1)].driver;
    // Driver, one-hand-capable gun: right click holds it out (the on-foot hip hold), fire raises it by
    // itself and shoots at once; it stays out 1.5 s after the last shot, then the hand returns.
    const WeaponDef *wd = weapon_equipped_def(w, slot);
    bool capable = driver && wd && wd->one_hand_capable;
    if (capable && (cmd->buttons & BTN_ATTACK)) m->veh_raise = (u8)(1.5f * SIM_HZ);
    else if (m->veh_raise) m->veh_raise--;
    if (!capable) m->veh_raise = 0;
    m->veh_onehand = capable && ((cmd->buttons & BTN_AIM) || m->veh_raise > 0) ? 1 : 0;
    if (driver && (w->authority || v->predicted) && !(VG.t.mode && v->ent == VG.t.veh)) {   // tests drive the vehicle themselves
        v->in_throttle = (float)cmd->move_y / 127.0f;
        v->in_steer = (float)cmd->move_x / 127.0f;
        v->in_hand = (cmd->buttons & BTN_JUMP) ? 1.0f : 0.0f;
        v->onehand = m->veh_onehand != 0;
        if (w->authority) {
            u8 l = (cmd->buttons & BTN_LIGHTS) ? 1 : 0;
            v->lights = l;
            u8 hn = (cmd->buttons & BTN_HORN) ? 1 : 0;
            if (hn && !v->in_horn_prev) {
                vec3 vp;
                quat vr;
                pose_sim(w, v, &vp, &vr);
                ev_sound(w, "horn", vp, 1.0f, EVENT_ALL);
            }
            v->in_horn_prev = hn;
        }
    }
}

// ------------------------------------------------------------------ driving (host + predicting client)

static int surface_at(World *w, const PhysWheelState *ws) {
    if (!ws->contact || ws->contact_body == PHYS_INVALID_BODY) return SURF_DEFAULT;
    return phys_body_material(w->phys, ws->contact_body);
}

static float surface_grip(int m) {
    switch (m) {
    case SURF_DIRT: return 0.78f;
    case SURF_SAND: return 0.62f;
    case SURF_GRAVEL: return 0.8f;
    case SURF_GRASS: return 0.72f;
    case SURF_WATER: return 0.55f;
    case SURF_METAL_THIN: case SURF_METAL_THICK: case SURF_TILE: return 0.85f;
    default: return 1.0f;
    }
}

static void drive(World *w, Vehicle *v, float dt) {
    const VehicleDef *d = vdef(v);
    vec3 p;
    quat r;
    pose_sim(w, v, &p, &r);
    vec3 fwd = quat_rotate(r, v3(0, 0, -1));
    float vf = v3_dot(v->vel, fwd), speed = v3_len(v->vel);
    int driver = -1;
    for (int s = 0; s < d->seat_count; s++) if (d->seats[s].driver && v->occ[s]) driver = s;
    v->engine = driver >= 0 && v->fuel > 0.0f && !v->wrecked && !v->drowned;
    float throttle = driver >= 0 ? v->in_throttle : 0.0f, steer_in = driver >= 0 ? v->in_steer : 0.0f;
    float hand = driver >= 0 ? v->in_hand : (speed < 1.5f ? 1.0f : 0.0f);
    if (!v->engine) throttle = fminf(throttle, 0.0f) * 0.0f;
    float brake = 0.0f;
    if (driver < 0 && speed >= 1.5f) brake = 0.15f;
    brake = fmaxf(brake, v->in_brake);
    if (driver >= 0 && v->in_throttle > 0.05f && vf < -1.0f) { brake = v->in_throttle; throttle = 0; }
    else if (driver >= 0 && v->in_throttle < -0.05f && vf > 1.0f) { brake = -v->in_throttle; throttle = 0; }
    // Tires: grip (pressure / health x surface x weather), pull from uneven deflation.
    PhysVehicleState st;
    phys_vehicle_state(v->pv, &st);
    for (int k = 0; k < d->wheel_count; k++) {
        float gl, gt;
        tire_grip(&v->tire[k], &gl, &gt);
        float sg = surface_grip(surface_at(w, &st.wheel[k]));
        float wg = st.wheel[k].contact ? weather_grip(w, st.wheel[k].contact_pos) : 1.0f;
        phys_vehicle_set_grip(v->pv, k, gl * sg * wg, gt * sg * wg);
    }
    float target = steer_in + vehicle_tire_pull(d, v->tire);
    if (!v->test_steer_override) {
        v->steer = vehicle_steer_smooth(v->steer, clampf(target, -1, 1), speed, dt);
        v->steer = vehicle_steer_clamp(d, v->steer, v->onehand);
    }
    v->handbrake = hand > 0.5f;
    v->braking = brake > 0.1f || v->handbrake;
    phys_vehicle_input(w->phys, v->pv, throttle, v->steer, brake, hand);
    phys_vehicle_moto_lean(w->phys, v->pv, speed, dt);
    // Fuel.
    if (v->engine && w->authority) {
        float rf = st.rpm > 0 ? saturate((st.rpm - d->min_rpm) / fmaxf(d->max_rpm - d->min_rpm, 1.0f)) : 0.0f;
        v->fuel -= vehicle_fuel_rate(d, fabsf(throttle), rf) * dt * VG.fuel_scale->f;
        if (v->fuel <= 0.0f) {
            v->fuel = 0.0f;
            v->engine = false;
            ev_sound(w, "engine_stall", p, 0.9f, EVENT_ALL);
            if (VG.debug->i) log_info("vehicle %u: out of fuel", v->ent);
        }
    }
    for (int k = 0; k < d->wheel_count; k++) tire_tick(&v->tire[k], dt);
}

// Water (docs/WATER.md "Vehicles"): buoyancy probes over the body boxes; the displaced volume
// shrinks as the cabin floods through the door sills, so a car floats for a while and then sinks;
// the engine dies once its intake has been under water for flood_time (or the cabin is half full),
// the electrics with it. A boat hull never floods; its throttle pushes at the stern (stub). Host:
// flooding; every machine simulating the vehicle: the buoyancy (the flood level is replicated).
static void vehicle_water(World *w, Vehicle *v, bool host) {
    const VehicleDef *d = vdef(v);
    BodyId body = phys_vehicle_body(v->pv);
    Buoyancy *b = game_water_buoy_of(body);
    if (!b) {
        if (!game_water()->count) return;
        BuoyProbe pr[BUOY_MAX_PROBES];
        int n = 0;
        for (int k = 0; k < d->box_count && n < BUOY_MAX_PROBES; k++) {
            int nx, ny, nz;
            buoy_box_grid(d->box_half[k], 16, &nx, &ny, &nz);
            n += buoy_probes_box(d->box_center[k], d->box_half[k], nx, ny, nz, pr + n, BUOY_MAX_PROBES - n);
        }
        BuoyDesc bd = buoy_desc_default();
        bd.drag = 0.6f;
        bd.angular_drag = 1.0f;
        b = game_water_buoy_attach(w, body, pr, n, &bd);
        if (!b) return;
    }
    b->volume = vehicle_float_volume(d, v->flood);
    if (d->boat && b->in_water) {
        float throttle = v->engine ? v->in_throttle : 0.0f;
        vec3 p;
        quat r;
        phys_get_transform(w->phys, body, &p, &r);
        vec3 fwd = quat_rotate(r, v3(0, 0, -1)), stern = v3_add(p, quat_rotate(r, v3(0, 0, d->box_half[0].z)));
        phys_add_impulse(w->phys, body, v3_scale(fwd, throttle * d->boat_thrust * SIM_DT), stern);
    }
    if (!host) return;
    vec3 p;
    quat r;
    pose_sim(w, v, &p, &r);
    float surf = game_water_height(p.x, p.z, (double)w->tick);
    bool wet = surf > -1e8f;
    v->flood = vehicle_flood_step(d, v->flood, wet ? surf - (p.y + d->flood_inlet_y) : -1.0f, SIM_DT);
    bool intake = wet && surf > p.y + d->engine_intake_y;
    v->intake_t = intake ? v->intake_t + SIM_DT : fmaxf(0.0f, v->intake_t - SIM_DT);
    if (!v->drowned && !d->boat && (v->intake_t >= d->flood_time || v->flood > 0.5f)) {
        v->drowned = true;
        v->engine = false;
        ev_sound(w, "engine_stall", p, 0.9f, EVENT_ALL);
        if (VG.debug->i) log_info("vehicle %u: engine flooded", v->ent);
    }
    if (v->drowned && v->flood <= 0.0f && !intake) v->drowned = false;   // dried out
    if (v->drowned) v->lights = false;
}

// Reads the simulated state back (after the physics step).
static void read_state(World *w, Vehicle *v) {
    BodyId b = phys_vehicle_body(v->pv);
    v->vel = phys_get_velocity(w->phys, b);
    v->angvel = phys_get_angular_velocity(w->phys, b);
    PhysVehicleState st;
    phys_vehicle_state(v->pv, &st);
    v->rpm = v->engine ? st.rpm : 0.0f;
    v->gear = st.gear;
    const VehicleDef *d = vdef(v);
    for (int k = 0; k < d->wheel_count; k++) v->susp[k] = clampf(st.wheel[k].suspension, 0.0f, d->wheels[k].susp_max);
}

// ------------------------------------------------------------------ host tick

static bool in_box_local(vec3 l, const VehicleDef *d, float pad) {
    vec3 c = d->box_center[0], h = d->box_half[0];
    return fabsf(l.x - c.x) < h.x + pad && fabsf(l.z - c.z) < h.z + pad && l.y > -0.2f && l.y < c.y + h.y + 1.6f;
}

static void run_over(World *w, Vehicle *v, vec3 p, quat r) {
    float speed = v3_len(v->vel);
    if (speed < 3.0f) return;
    const VehicleDef *d = vdef(v);
    quat ir = quat_conj(r);
    float reach = d->box_half[0].z + 2.0f;
    int driver_slot = v->occ[0] ? v->occ[0] - 1 : -1;
    EntityId attacker = driver_slot >= 0 ? w->players[driver_slot].ent : v->ent;
    ENT_FOREACH(w, i) {
        EntityType t = (EntityType)w->type[i];
        if (t != ET_ZOMBIE && t != ET_PLAYER && t != ET_DUMMY) continue;
        if (!(w->comps[i] & C_HEALTH) || (w->flags[i] & (EF_DEAD | EF_SEATED))) continue;
        vec3 tp = w->xf[i].pos;
        if (t == ET_DUMMY) tp = v3_sub(tp, v3(0, 0.9f, 0));
        if (fabsf(tp.x - p.x) > reach || fabsf(tp.z - p.z) > reach) continue;
        vec3 l = quat_rotate(ir, v3_sub(tp, p));
        if (!in_box_local(l, d, 0.45f)) continue;
        EntityId e = ENT_MAKE(i, w->gen[i]);
        bool cooling = false;
        for (int k = 0; k < 8; k++) if (v->hit_ent[k] == e && w->tick - v->hit_tick[k] < 30) cooling = true;
        if (cooling) continue;
        vec3 dir = v3_scale(v->vel, 1.0f / speed);
        float dmg = speed * speed * 1.2f;   // 6 m/s: 43, 10 m/s: 120 (most zombies), 15 m/s: 270
        apply_damage(w, e, dmg, DMG_GENERIC, attacker, tp, dir, false);
        VG.runover_hits++;
        if (w->flags[i] & EF_DEAD) VG.runover_kills++;
        int slot = (int)(w->tick % 8);
        v->hit_ent[slot] = e;
        v->hit_tick[slot] = w->tick;
        ev_sound(w, "impact_flesh", tp, fminf(speed / 12.0f, 1.0f), EVENT_ALL);
        // A little of the car's momentum goes into the body.
        phys_set_velocity(w->phys, phys_vehicle_body(v->pv), v3_scale(v->vel, 0.985f), v->angvel);
    }
}

static void spikes(World *w, Vehicle *v) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    if (!s->spike_count) return;
    PhysVehicleState st;
    phys_vehicle_state(v->pv, &st);
    for (int k = 0; k < st.wheel_count; k++) {
        if (!st.wheel[k].contact || tire_on_rim(&v->tire[k])) continue;
        vec3 c = st.wheel[k].contact_pos;
        for (int z = 0; z < s->spike_count; z++) {
            const SpikeZone *sz = &s->spikes[z];
            if (fabsf(c.x - sz->x) < sz->hw && fabsf(c.z - sz->z) < sz->hd) {
                v->tire[k].health = 0;
                v->tire[k].pressure = 0;
                v->tire[k].leak = 0.2f;
                ev_sound(w, "tire_burst", c, 1.0f, EVENT_ALL);
            }
        }
    }
}

void vehicles_host_tick(World *w) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    if (!s || !w->authority) return;
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used) continue;
        if (!ent_valid(w, v->ent)) { veh_release(w, v); continue; }
        // Occupants still valid (dead / respawned / disconnected players leave their seat).
        for (int k = 0; k < VEH_MAX_SEATS; k++) {
            if (!v->occ[k]) continue;
            Player *p = &w->players[v->occ[k] - 1];
            if (!p->active || p->sim.move.mode != MOVE_SEATED || p->sim.move.vehicle != v->ent || p->sim.move.seat != k) {
                v->occ[k] = 0;
                if (p->active && p->character && p->sim.move.mode != MOVE_SEATED) phys_set_body_enabled(w->phys, phys_character_inner_body(p->character), true);
            }
        }
        if (!v->pv) continue;
        drive(w, v, SIM_DT);
        vehicle_water(w, v, true);
    }
}

void vehicles_host_post(World *w) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    if (!s || !w->authority) return;
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used || !v->pv || !ent_valid(w, v->ent)) continue;
        int ei = (int)ENT_INDEX(v->ent);
        const VehicleDef *d = vdef(v);
        vec3 p;
        quat r;
        phys_get_transform(w->phys, phys_vehicle_body(v->pv), &p, &r);
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
            log_warn("vehicle %u: invalid transform, reset", v->ent);
            p = w->xf[ei].prev_pos;
            r = quat_yaw(quat_get_yaw(w->xf[ei].prev_rot));
            phys_set_transform(w->phys, phys_vehicle_body(v->pv), p, r, true);
            phys_set_velocity(w->phys, phys_vehicle_body(v->pv), v3(0, 0, 0), v3(0, 0, 0));
        }
        if (p.y < -60.0f) {   // fell out of the world
            p = v3(p.x, 5.0f, p.z);
            r = quat_yaw(quat_get_yaw(r));
            phys_set_transform(w->phys, phys_vehicle_body(v->pv), p, r, true);
            phys_set_velocity(w->phys, phys_vehicle_body(v->pv), v3(0, 0, 0), v3(0, 0, 0));
        }
        w->xf[ei].pos = p;
        w->xf[ei].rot = r;
        read_state(w, v);
        // Impacts: a velocity change no engine / brake can produce in one tick.
        vec3 dv = v3_sub(v->vel, v->prev_vel);
        float dvl = v3_len(dv);
        v->impact_cd -= SIM_DT;
        if (dvl > 4.0f && v->impact_cd <= 0.0f && !v->wrecked) {
            float dmg = vehicle_impact_damage(d, dvl * 1.6f);
            v->impact_cd = 0.25f;
            ev_sound(w, "vehicle_crash", p, clampf(dvl / 12.0f, 0.3f, 1.0f), EVENT_ALL);
            if (dmg > 0) {
                apply_damage(w, v->ent, dmg, DMG_GENERIC, v->ent, p, v3_scale(dv, -1.0f / dvl), false);
                for (int k = 0; k < VEH_MAX_SEATS; k++)
                    if (v->occ[k] && dvl > 8.0f) apply_damage(w, w->players[v->occ[k] - 1].ent, (dvl - 8.0f) * 4.0f, DMG_FALL, v->ent, p, v3(0, 1, 0), false);
            }
            if (VG.debug->i) log_info("vehicle %u: impact dv %.1f m/s -> %.0f damage (hp %.0f)", v->ent, dvl, dmg, w->health[ei].hp);
        }
        v->prev_vel = v->vel;
        // Wrecked: no engine, no more damage (and no respawn of the entity's health).
        if (!v->wrecked && ((w->flags[ei] & EF_DEAD) || w->health[ei].hp <= 0.0f)) {
            v->wrecked = true;
            w->flags[ei] &= ~(u32)EF_DEAD;
            w->comps[ei] &= ~(u32)C_HEALTH;
            w->health[ei].hp = 0;
            ev_sound(w, "vehicle_crash", p, 1.0f, EVENT_ALL);
            log_info("vehicle %u: wrecked", v->ent);
        }
        run_over(w, v, p, r);
        spikes(w, v);
        int hk = (int)(w->tick % VEH_HIST);
        v->hpos[hk] = p;
        v->hrot[hk] = r;
        v->htick[hk] = w->tick;
        u32 f = w->flags[ei] & ~(u32)(EF_HEADLIGHTS | EF_BRAKING | EF_WRECKED | EF_ENGINE);
        if (v->lights) f |= EF_HEADLIGHTS;
        if (v->braking) f |= EF_BRAKING;
        if (v->wrecked) f |= EF_WRECKED;
        if (v->engine) f |= EF_ENGINE;
        w->flags[ei] = f;
    }
}

// ------------------------------------------------------------------ client: proxies, prediction

static void proxies_ensure(World *w, Vehicle *v) {
    if (v->proxy_n || v->pv) return;
    const VehicleDef *d = vdef(v);
    int i = (int)ENT_INDEX(v->ent);
    for (int b = 0; b < d->box_count; b++) {
        BodyDesc bd;
        memset(&bd, 0, sizeof(bd));
        bd.shape.kind = SHAPE_BOX;
        bd.shape.half = d->box_half[b];
        bd.pos = to_world(w->xf[i].pos, w->xf[i].rot, d->box_center[b]);
        bd.rot = w->xf[i].rot;
        bd.motion = PM_KINEMATIC;
        bd.layer = PL_MOVING;
        bd.user = v->ent;
        bd.material = SURF_METAL_THIN;
        v->proxy[v->proxy_n++] = phys_add_body(w->phys, &bd);
    }
}

static void proxies_move(World *w, Vehicle *v) {
    const VehicleDef *d = vdef(v);
    int i = (int)ENT_INDEX(v->ent);
    for (int b = 0; b < v->proxy_n; b++)
        phys_move_kinematic(w->phys, v->proxy[b], to_world(w->xf[i].pos, w->xf[i].rot, d->box_center[b]), w->xf[i].rot, SIM_DT);
}

static void predict_start(World *w, Vehicle *v) {
    proxies_destroy(w, v);
    PhysVehicleDesc pd;
    int i = (int)ENT_INDEX(v->ent);
    vec3 p = v3_len(v->s_pos) > 0 ? v->s_pos : w->xf[i].pos;
    quat r = v3_len(v->s_pos) > 0 ? v->s_rot : w->xf[i].rot;
    build_desc(vdef(v), p, r, v->ent, &pd);
    v->pv = phys_vehicle_create(w->phys, &pd);
    if (!v->pv) return;
    phys_set_velocity(w->phys, phys_vehicle_body(v->pv), v->s_vel, v->s_ang);
    v->predicted = true;
    v->pp_prev = v->pp_cur = p;
    v->pr_prev = v->pr_cur = r;
    v->corr = v3(0, 0, 0);
    memset(v->pred, 0, sizeof(v->pred));
    log_info("vehicle %u: predicting locally (driver)", v->ent);
}

static void predict_stop(World *w, Vehicle *v) {
    if (v->pv) {
        game_water_buoy_detach(phys_vehicle_body(v->pv));
        phys_vehicle_destroy(w->phys, v->pv);
    }
    v->pv = NULL;
    v->predicted = false;
    v->corr = v3(0, 0, 0);
}

static bool owned_hook(World *w, int index) {
    if (w->type[index] != ET_VEHICLE) return false;
    Vehicle *v = veh_of(w, ENT_MAKE(index, w->gen[index]));
    return v && v->predicted;
}

bool vehicles_client_tick(World *w, const UserCmd *cmd) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    if (!s || w->authority) return false;
    Player *lp = game_local_player();
    bool need = false;
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used) continue;
        if (!ent_valid(w, v->ent) || w->type[ENT_INDEX(v->ent)] != ET_VEHICLE) { veh_release(w, v); continue; }
        const VehicleDef *d = vdef(v);
        bool drive_me = lp && lp->sim.move.mode == MOVE_SEATED && lp->sim.move.vehicle == v->ent &&
                        d->seats[DG_CLAMP(lp->sim.move.seat, 0, d->seat_count - 1)].driver && !v->wrecked;
        if (drive_me && !v->predicted) predict_start(w, v);
        if (!drive_me && v->predicted) predict_stop(w, v);
        if (v->predicted && v->pv) {
            // The same driving model as the host from this tick's command.
            v->occ[0] = (u8)(G->local_slot + 1);
            v->in_throttle = (float)cmd->move_y / 127.0f;
            v->in_steer = (float)cmd->move_x / 127.0f;
            v->in_hand = (cmd->buttons & BTN_JUMP) ? 1.0f : 0.0f;
            v->onehand = lp->sim.move.veh_onehand != 0;
            drive(w, v, SIM_DT);
            vehicle_water(w, v, false);
            game_water_buoy_step_one(w, phys_vehicle_body(v->pv));   // the host steps it in water_tick
            need = true;
        } else {
            proxies_ensure(w, v);
            proxies_move(w, v);
        }
    }
    // The local character's inner body never collides with its own vehicle.
    if (lp && lp->character) phys_set_body_enabled(w->phys, phys_character_inner_body(lp->character), lp->sim.move.mode != MOVE_SEATED);
    return need;
}

void vehicles_client_post(World *w, u32 seq) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    if (!s || w->authority) return;
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used || !v->predicted || !v->pv) continue;
        vec3 p;
        quat r;
        phys_get_transform(w->phys, phys_vehicle_body(v->pv), &p, &r);
        read_state(w, v);
        v->pp_prev = v->pp_cur;
        v->pr_prev = v->pr_cur;
        v->pp_cur = p;
        v->pr_cur = r;
        v->corr = v3_scale(v->corr, 0.88f);
        int ei = (int)ENT_INDEX(v->ent);
        w->xf[ei].pos = w->xf[ei].prev_pos = p;
        w->xf[ei].rot = w->xf[ei].prev_rot = r;
        int k = (int)(seq % VEH_PRED);
        v->pred[k].seq = seq;
        v->pred[k].pos = p;
        v->pred[k].rot = r;
        v->pred[k].vel = v->vel;
    }
}

// Reconciliation: the host's state after the last command it processed (ack_seq) against what this
// client predicted for the same command. Small errors are blended in (a fraction per snapshot,
// rendered smoothly through the correction offset), large ones snap.
static void reconcile(World *w, Vehicle *v, u32 ack_seq) {
    if (!v->pv || !ack_seq) return;
    int k = (int)(ack_seq % VEH_PRED);
    if (v->pred[k].seq != ack_seq) return;
    vec3 err = v3_sub(v->s_pos, v->pred[k].pos);
    float e = v3_len(err);
    VG.pred_err = e;
    VG.pred_err_max = fmaxf(VG.pred_err_max, e);
    VG.pred_err_sum += e;
    VG.pred_err_n++;
    BodyId b = phys_vehicle_body(v->pv);
    vec3 p;
    quat r;
    phys_get_transform(w->phys, b, &p, &r);
    if (e > 3.0f) {
        u32 ahead = G->client_seq > ack_seq ? G->client_seq - ack_seq : 0;
        vec3 np = v3_madd(v->s_pos, v->s_vel, (float)ahead * SIM_DT);
        phys_set_transform(w->phys, b, np, v->s_rot, true);
        phys_set_velocity(w->phys, b, v->s_vel, v->s_ang);
        v->corr = v3(0, 0, 0);
        VG.pred_snaps++;
        for (int i = 0; i < VEH_PRED; i++) v->pred[i].seq = 0;
        return;
    }
    float kk = 0.2f;
    vec3 shift = v3_scale(err, kk);
    // Rotation error: the host's orientation relative to the predicted one, partially applied.
    quat dq = quat_mul(v->s_rot, quat_conj(v->pred[k].rot));
    quat part = quat_nlerp(quat_identity(), dq, kk);
    phys_set_transform(w->phys, b, v3_add(p, shift), quat_norm(quat_mul(part, r)), true);
    vec3 vel = phys_get_velocity(w->phys, b);
    phys_set_velocity(w->phys, b, v3_add(vel, v3_scale(v3_sub(v->s_vel, v->pred[k].vel), kk)), phys_get_angular_velocity(w->phys, b));
    v->corr = v3_sub(v->corr, shift);   // the drawn car glides (the body jumped by shift)
    for (int i = 0; i < VEH_PRED; i++) if (v->pred[i].seq > ack_seq) v->pred[i].pos = v3_add(v->pred[i].pos, shift);
}

// ------------------------------------------------------------------ replication

#define VF_ENGINE 1
#define VF_LIGHTS 2
#define VF_BRAKE 4
#define VF_WRECK 8
#define VF_ONEHAND 16
#define VF_HAND 32
#define VF_DROWNED 64

void vehicles_net_write(World *w, Writer *wr) {
    VehicleSys *s = (VehicleSys *)w->vehicles;
    int n = 0;
    for (int i = 0; s && i < MAX_VEH; i++) if (s->v[i].used && ent_valid(w, s->v[i].ent)) n++;
    w_u8(wr, (u8)n);
    for (int i = 0; s && i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used || !ent_valid(w, v->ent)) continue;
        const VehicleDef *d = vdef(v);
        w_u32(wr, v->ent);
        w_u8(wr, (u8)v->def);
        w_u8(wr, veh_q01(d->fuel_capacity > 0 ? v->fuel / d->fuel_capacity : 0));
        for (int k = 0; k < VEH_MAX_WHEELS; k++) { w_u8(wr, veh_q01(v->tire[k].pressure)); w_u8(wr, veh_q01(v->tire[k].health)); }
        for (int k = 0; k < VEH_MAX_SEATS; k++) w_u8(wr, v->occ[k]);
        w_u8(wr, (u8)veh_qs(v->steer));
        w_u8(wr, veh_q01(d->max_rpm > 0 ? v->rpm / d->max_rpm : 0));
        w_u8(wr, (u8)(i8)DG_CLAMP(v->gear, -1, 9));
        for (int k = 0; k < VEH_MAX_WHEELS; k++) w_u8(wr, veh_q01(k < d->wheel_count && d->wheels[k].susp_max > 0 ? v->susp[k] / d->wheels[k].susp_max : 0));
        u8 f = (v->engine ? VF_ENGINE : 0) | (v->lights ? VF_LIGHTS : 0) | (v->braking ? VF_BRAKE : 0) | (v->wrecked ? VF_WRECK : 0) | (v->onehand ? VF_ONEHAND : 0) |
               (v->handbrake ? VF_HAND : 0) | (v->drowned ? VF_DROWNED : 0);
        w_u8(wr, f);
        w_u8(wr, veh_q01(v->flood));
        vec3 p;
        quat r;
        pose_sim(w, v, &p, &r);
        w_vec3(wr, p);
        w_f32(wr, r.x); w_f32(wr, r.y); w_f32(wr, r.z); w_f32(wr, r.w);
        w_vec3(wr, v->vel);
        w_vec3(wr, v->angvel);
    }
}

void vehicles_net_read(World *w, Reader *rd, bool apply, u32 ack_seq) {
    ensure_lib();
    int n = r_u8(rd);
    for (int i = 0; i < n; i++) {
        u32 ent = r_u32(rd);
        int def = r_u8(rd);
        float fuel = veh_dq01(r_u8(rd));
        TireState tires[VEH_MAX_WHEELS];
        for (int k = 0; k < VEH_MAX_WHEELS; k++) { tires[k].pressure = veh_dq01(r_u8(rd)); tires[k].health = veh_dq01(r_u8(rd)); tires[k].leak = 0; }
        u8 occ[VEH_MAX_SEATS];
        for (int k = 0; k < VEH_MAX_SEATS; k++) occ[k] = r_u8(rd);
        float steer = veh_dqs((i8)r_u8(rd));
        float rpm = veh_dq01(r_u8(rd));
        int gear = (i8)r_u8(rd);
        float susp[VEH_MAX_WHEELS];
        for (int k = 0; k < VEH_MAX_WHEELS; k++) susp[k] = veh_dq01(r_u8(rd));
        u8 f = r_u8(rd);
        float flood = veh_dq01(r_u8(rd));
        vec3 p = r_vec3(rd);
        quat r;
        r.x = r_f32(rd); r.y = r_f32(rd); r.z = r_f32(rd); r.w = r_f32(rd);
        vec3 vel = r_vec3(rd), ang = r_vec3(rd);
        if (!apply || rd->overflow) continue;
        Vehicle *v = veh_of(w, ent);
        if (!v) {
            if (!ent_valid(w, ent)) continue;   // the entity has not replicated yet
            v = veh_alloc(w, ent);
            if (!v) continue;
        }
        v->def = DG_CLAMP(def, 0, VG.lib.count - 1);
        const VehicleDef *d = vdef(v);
        v->fuel = fuel * d->fuel_capacity;
        memcpy(v->tire, tires, sizeof(tires));
        memcpy(v->occ, occ, sizeof(occ));
        v->rpm = rpm * d->max_rpm;
        v->gear = gear;
        v->engine = (f & VF_ENGINE) != 0;
        v->lights = (f & VF_LIGHTS) != 0;
        v->braking = (f & VF_BRAKE) != 0;
        v->wrecked = (f & VF_WRECK) != 0;
        v->onehand = (f & VF_ONEHAND) != 0;
        v->handbrake = (f & VF_HAND) != 0;
        v->drowned = (f & VF_DROWNED) != 0;
        v->flood = flood;
        v->s_pos = p;
        v->s_rot = quat_norm(r);
        v->s_vel = vel;
        v->s_ang = ang;
        if (!v->predicted) {
            v->steer = steer;
            for (int k = 0; k < VEH_MAX_WHEELS; k++) v->susp[k] = k < d->wheel_count ? susp[k] * d->wheels[k].susp_max : 0;
            v->vel = vel;
            v->angvel = ang;
        } else reconcile(w, v, ack_seq);
    }
}

// ------------------------------------------------------------------ events (every machine)

void vehicles_handle_event(const GameEvent *ev) {
    World *w = G ? G->world : NULL;
    if (!w || ev->type != EV_VEHICLE) return;
    if (ev->id == 1) {
        // Ejected at speed: a ragdoll tumble for the stun time (the capsule slides under it).
        VehicleSys *s = sys(w);
        chars_ragdoll_now(w, ev->ent, v3_scale(ev->dir, 6.0f));
        for (int k = 0; k < MAX_PLAYERS; k++)
            if (!s->eject[k].ent || s->eject[k].ent == ev->ent) { s->eject[k].ent = ev->ent; s->eject[k].t = ev->value; break; }
    }
}

// ------------------------------------------------------------------ interactions

// Standing behind a car (more than a metre past its centre, against its forward): E opens the
// trunk, the vehicle's world inventory (docs/INVENTORY.md). Motorbikes (one seat) have none.
static bool at_trunk(World *w, EntityId self, int player, const VehicleDef *d) {
    if (d->seat_count < 2) return false;
    int i = (int)ENT_INDEX(self);
    vec3 fwd = forward_from_yaw(quat_get_yaw(w->xf[i].rot));
    vec3 to = v3_sub(w->players[player].sim.move.pos, w->xf[i].pos);
    return v3_dot(to, fwd) < -1.0f;
}

static void veh_focus(World *w, EntityId self, int player, char *prompt, int size) {
    Vehicle *v = veh_of(w, self);
    if (!v) { snprintf(prompt, (size_t)size, "Vehicle"); return; }
    const VehicleDef *d = vdef(v);
    if (at_trunk(w, self, player, d)) { snprintf(prompt, (size_t)size, "Open %s trunk", d->display); return; }
    int free_seats = 0, free_driver = 0;
    for (int s = 0; s < d->seat_count; s++) if (!v->occ[s]) { free_seats++; if (d->seats[s].driver) free_driver = 1; }
    if (v->wrecked) snprintf(prompt, (size_t)size, "%s (wrecked)", d->display);
    else if (!free_seats) snprintf(prompt, (size_t)size, "%s (full)", d->display);
    else snprintf(prompt, (size_t)size, "%s %s  (F)", free_driver ? "Drive" : "Ride in", d->display);
}

static bool veh_can_use(World *w, EntityId self, int player) {
    Vehicle *v = veh_of(w, self);
    return v && w->players[player].sim.move.mode == MOVE_WALK;
}

static void veh_use(World *w, EntityId self, int player) {
    Vehicle *v = veh_of(w, self);
    if (!v) return;
    if (at_trunk(w, self, player, vdef(v))) {
        if (!w->authority) return;
        if (w->cont[ENT_INDEX(self)].root < 0) container_set_root(w, self, "trunk");
        container_send(w, self, player);
        return;
    }
    vehicle_enter(w, player, v, -1);
}

static const InteractDef k_vehicle_def = { "vehicle", veh_focus, veh_can_use, veh_use, NULL, NULL, 0.0f, HANDS_RIGHT };

static void pump_focus(World *w, EntityId self, int player, char *prompt, int size) {
    (void)self;
    float d = 0;
    Vehicle *v = nearest_vehicle(w, w->players[player].sim.move.pos, 8.0f, &d);
    if (v) snprintf(prompt, (size_t)size, "Refuel %s (%.0f / %.0f l)", vdef(v)->display, v->fuel, vdef(v)->fuel_capacity);
    else snprintf(prompt, (size_t)size, "Fuel pump (bring a vehicle)");
}

static bool pump_can_use(World *w, EntityId self, int player) {
    (void)self;
    float d;
    return nearest_vehicle(w, w->players[player].sim.move.pos, 8.0f, &d) != NULL;
}

static void pump_hold(World *w, EntityId self, int player, float progress) {
    if (progress < 1.0f) return;
    float d;
    Vehicle *v = nearest_vehicle(w, w->players[player].sim.move.pos, 8.0f, &d);
    if (!v) return;
    v->fuel = vdef(v)->fuel_capacity;
    ev_hint(w, player, "%s refueled", vdef(v)->display);
    ev_sound(w, "pickup", w->xf[ENT_INDEX(self)].pos, 0.7f, EVENT_ALL);
}

static const InteractDef k_pump_def = { "fuel_pump", pump_focus, pump_can_use, NULL, pump_hold, NULL, 2.5f, HANDS_RIGHT };

// Consumables used on a vehicle (host): fuel can (nearest vehicle), tire kit / spare (nearest wheel).
static bool hook_use_item(World *w, int slot, const ItemDef *it) {
    Player *p = &w->players[slot];
    vec3 pos = p->sim.move.pos;
    float dist = 0;
    Vehicle *v = nearest_vehicle(w, pos, 2.5f, &dist);
    if (!v) { ev_hint(w, slot, "%s: stand next to a vehicle", it->name); return false; }
    const VehicleDef *d = vdef(v);
    if (it->vehicle_use == VUSE_FUEL) {
        if (v->fuel >= d->fuel_capacity - 0.05f) { ev_hint(w, slot, "The tank is full"); return false; }
        v->fuel = fminf(d->fuel_capacity, v->fuel + it->fuel);
        ev_hint(w, slot, "%s: %.0f / %.0f l", d->display, v->fuel, d->fuel_capacity);
        ev_sound(w, "pickup", pos, 0.7f, EVENT_ALL);
        return true;
    }
    // Wheels: the nearest one to the player.
    vec3 vp;
    quat vr;
    pose_sim(w, v, &vp, &vr);
    int best = -1;
    float bd = 2.2f;
    for (int k = 0; k < d->wheel_count; k++) {
        float dd = v3_dist(v3_flat(to_world(vp, vr, d->wheels[k].pos)), v3_flat(pos));
        if (dd < bd) { bd = dd; best = k; }
    }
    if (best < 0) { ev_hint(w, slot, "%s: stand next to the wheel", it->name); return false; }
    TireState *t = &v->tire[best];
    if (it->vehicle_use == VUSE_TIRE_REPAIR) {
        if (t->health <= d->tire_burst_health) { ev_hint(w, slot, "The tire is destroyed: use a spare tire"); return false; }
        if (t->pressure >= 0.85f && t->leak <= 0) { ev_hint(w, slot, "The tire is fine"); return false; }
        tire_repair(t);
    } else {
        if (t->pressure >= 0.99f && t->health >= 0.99f) { ev_hint(w, slot, "The tire is fine"); return false; }
        tire_replace(t);
    }
    ev_hint(w, slot, "Wheel %d repaired", best + 1);
    ev_sound(w, "reload", pos, 0.6f, EVENT_ALL);
    return true;
}

// ------------------------------------------------------------------ level

void vehicles_level_loaded(World *w, const char *map) {
    ensure_lib();
    VehicleSys *s = sys(w);
    s->spike_count = 0;
    VG.have_prev_yaw = false;
    VG.cam_tp = false;
    cJSON *root = level_json_load(map);   // includes resolved
    if (!root) return;
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "vehicle_spikes")) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "pos"), *z = cJSON_GetObjectItemCaseSensitive(it, "size");
        if (s->spike_count >= MAX_SPIKES || cJSON_GetArraySize(p) < 2 || cJSON_GetArraySize(z) < 2) continue;
        SpikeZone *sz = &s->spikes[s->spike_count++];
        sz->x = (float)cJSON_GetArrayItem(p, 0)->valuedouble;
        sz->z = (float)cJSON_GetArrayItem(p, cJSON_GetArraySize(p) >= 3 ? 2 : 1)->valuedouble;
        sz->hw = (float)cJSON_GetArrayItem(z, 0)->valuedouble * 0.5f;
        sz->hd = (float)cJSON_GetArrayItem(z, cJSON_GetArraySize(z) >= 3 ? 2 : 1)->valuedouble * 0.5f;
    }
    if (w->authority) {
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "vehicles")) {
            const cJSON *dn = cJSON_GetObjectItemCaseSensitive(it, "def"), *p = cJSON_GetObjectItemCaseSensitive(it, "pos"), *y = cJSON_GetObjectItemCaseSensitive(it, "yaw");
            if (!cJSON_IsString(dn) || cJSON_GetArraySize(p) < 3) continue;
            vec3 pos = v3((float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 1)->valuedouble, (float)cJSON_GetArrayItem(p, 2)->valuedouble);
            EntityId e = vehicle_spawn(w, dn->valuestring, pos, cJSON_IsNumber(y) ? (float)y->valuedouble * DG_DEG2RAD : 0.0f);
            const cJSON *fu = cJSON_GetObjectItemCaseSensitive(it, "fuel");
            Vehicle *v = veh_of(w, e);
            if (v && cJSON_IsNumber(fu)) v->fuel = fminf((float)fu->valuedouble, vdef(v)->fuel_capacity);
        }
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "fuel_pumps")) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "pos"), *y = cJSON_GetObjectItemCaseSensitive(it, "yaw");
            if (cJSON_GetArraySize(p) < 3) continue;
            vec3 pos = v3((float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 1)->valuedouble, (float)cJSON_GetArrayItem(p, 2)->valuedouble);
            spawn_pump(w, pos, cJSON_IsNumber(y) ? (float)y->valuedouble * DG_DEG2RAD : 0.0f);
        }
        // open-world maps: the generator's vehicles (barns, fuel stations, hangars) and pumps
        const MapPois *mp = openworld_pois(w);
        for (int i = 0; mp && i < mp->ent_count; i++) {
            const MpEnt *e = &mp->ents[i];
            if (e->kind == MPE_PUMP) spawn_pump(w, e->pos, e->yaw);
            if (e->kind != MPE_VEHICLE) continue;
            EntityId id = vehicle_spawn(w, e->item, e->pos, e->yaw);
            Vehicle *v = veh_of(w, id);
            if (v) v->fuel = fminf(4.0f + (float)(i % 5) * 3.0f, vdef(v)->fuel_capacity);
        }
    }
    cJSON_Delete(root);
}

// ------------------------------------------------------------------ local input / camera

static Vehicle *player_vehicle(World *w, const Player *lp, int *seat) {
    if (!w || !lp || lp->sim.move.mode != MOVE_SEATED) return NULL;
    Vehicle *v = veh_of(w, lp->sim.move.vehicle);
    if (v && seat) *seat = DG_CLAMP(lp->sim.move.seat, 0, vdef(v)->seat_count - 1);
    return v;
}

static Vehicle *local_vehicle(World *w, int *seat) { return player_vehicle(w, game_local_player(), seat); }
// The viewed player's vehicle (the local player's, or a spectated player's: camera / HUD).
static Vehicle *view_vehicle(World *w, int *seat) { return player_vehicle(w, game_view_player(), seat); }

// Frame input (before the hotbar): F enters / exits (it is the flashlight otherwise), 1-4 change
// seats while seated, V vehicle camera, H headlights, Q horn.
void vehicles_input(void) {
    World *w = G ? G->world : NULL;
    if (!w) return;
    int seat = 0;
    Vehicle *v = local_vehicle(w, &seat);
    bool focus_vehicle = !v && veh_of(w, G->focus) != NULL;
    if ((v || focus_vehicle) && input_consume_press(ACT_FLASHLIGHT)) VG.req_vehicle = true;
    if (v) {
        for (int i = 0; i < 4; i++)
            if (input_consume_press((Action)(ACT_SLOT1 + i))) VG.req_seat = (u8)(i + 1);
        if (input_consume_press(ACT_VEHICLE_CAM)) VG.cam_tp = !VG.cam_tp;
        if (input_consume_press(ACT_LIGHTS)) VG.lights = !VG.lights;
    }
}

void vehicles_fill_cmd(UserCmd *c) {
    if (VG.req_vehicle) { c->buttons |= BTN_VEHICLE; VG.req_vehicle = false; }
    if (VG.req_seat) { c->seat_req = VG.req_seat; VG.req_seat = 0; }
    if (VG.lights) c->buttons |= BTN_LIGHTS;
    if (input_down(ACT_HORN)) c->buttons |= BTN_HORN;
    if (VG.cam_tp) c->buttons |= BTN_VEH_CAM;   // spectators follow the driver's camera choice
}

// The view turns with the vehicle (free look relative to it, limited per seat).
void vehicles_view_frame(ViewState *view) {
    World *w = G ? G->world : NULL;
    int seat = 0;
    Vehicle *v = local_vehicle(w, &seat);
    if (!v) { VG.have_prev_yaw = false; return; }
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    float yaw = quat_get_yaw(r);
    if (VG.have_prev_yaw && VG.view_veh == v->ent) {
        float dy = yaw - VG.prev_veh_yaw;
        while (dy > DG_PI) dy -= 2 * DG_PI;
        while (dy < -DG_PI) dy += 2 * DG_PI;
        view->yaw += dy;
    } else view->yaw = yaw;
    VG.prev_veh_yaw = yaw;
    VG.view_veh = v->ent;
    VG.have_prev_yaw = true;
    if (!VG.cam_tp) {
        float lim = vdef(v)->seats[seat].yaw_limit_deg * DG_DEG2RAD;
        float rel = view->yaw - yaw;
        while (rel > DG_PI) rel -= 2 * DG_PI;
        while (rel < -DG_PI) rel += 2 * DG_PI;
        rel = clampf(rel, -lim, lim);
        view->yaw = yaw + rel;
    }
}

static bool cam_cast(PhysicsWorld *pw, vec3 from, vec3 to, BodyId ign, vec3 *out) {
    ShapeHit sh;
    vec3 d = v3_sub(to, from);
    if (phys_sphere_cast(pw, from, d, 0.2f, PL_MASK_WORLD, ign, &sh)) { *out = v3_madd(from, d, fmaxf(sh.fraction - 0.02f, 0.0f)); return true; }
    *out = to;
    return false;
}

// Seated camera: the head eye (first person, no capsule collision: the cabin is around it) or a
// third-person orbit behind the vehicle. Returns the camera mode (1 head, 3 third person) or 0.
int vehicles_camera(World *w, RenderCamera *cam) {
    int seat = 0;
    Vehicle *v = view_vehicle(w, &seat);
    if (!v) return 0;
    const VehicleDef *d = vdef(v);
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    bool own = game_view_slot() == G->local_slot;
    bool cam_tp = own ? VG.cam_tp : (game_view_cmd()->buttons & BTN_VEH_CAM) != 0;
    if (cam_tp || cvar_find("cl_thirdperson")->i) {
        chars_set_thirdperson(true);
        vec3 target = to_world(p, r, v3(0, d->camera_tp.y * 0.55f, 0));
        vec3 dir = dir_from_angles(cam->yaw, cam->pitch);
        vec3 want = v3_add(v3_madd(target, dir, -d->camera_tp.z), v3(0, d->camera_tp.y * 0.35f, 0));
        BodyId ign = v->pv ? phys_vehicle_body(v->pv) : (v->proxy_n ? v->proxy[0] : PHYS_INVALID_BODY);
        cam_cast(w->phys, target, want, ign, &cam->pos);
        return 3;
    }
    chars_set_thirdperson(false);
    // First person (docs/CAMERA_HOLDPOINT.md "Rule 2"): the seat's eye point + the view pitch's eye
    // shift (the nominal eye table), never the animated head; the body's frame re-solve still runs
    // (chars_fp_camera: the hold point and the held gun live on its pose eye).
    Xform sock;
    if (cvar_find("cl_camera_socket")->i) chars_fp_camera(w, G->alpha, game_view_slot(), G->view.yaw, G->view.pitch, &sock);
    cam->pos = to_world(p, r, v3_add(d->seats[seat].pos, d->seats[seat].eye));
    const EyeNominalTable *et = camera_fp_eye_table();
    if (et->valid) {
        vec3 dd = v3_sub(eye_table_sample(et, EYE_STAND, 0, 0, G->view.pitch), eye_table_sample(et, EYE_STAND, 0, 0, 0));
        vec3 fwd = v3(-sinf(G->view.yaw), 0, -cosf(G->view.yaw));
        cam->pos = v3_add(cam->pos, v3_add(v3_add(v3_scale(right_from_yaw(G->view.yaw), dd.x), v3(0, dd.y, 0)), v3_scale(fwd, dd.z)));
    }
    cam->roll = 0.0f;
    return 1;
}

// ------------------------------------------------------------------ presentation

static u32 obj_id(const Vehicle *v, int part) { return 0x40000000u | (ENT_INDEX(v->ent) << 6) | (u32)part; }

static mat4 basis_mat(vec3 pos, vec3 x, vec3 y, vec3 z) {
    mat4 m = mat4_identity();
    m.m[0] = x.x; m.m[1] = x.y; m.m[2] = x.z;
    m.m[4] = y.x; m.m[5] = y.y; m.m[6] = y.z;
    m.m[8] = z.x; m.m[9] = z.y; m.m[10] = z.z;
    m.m[12] = pos.x; m.m[13] = pos.y; m.m[14] = pos.z;
    return m;
}

// Audio (docs/AUDIO_PROCEDURAL.md "Vehicles"): the synthesized engine (firing pulses at rpm through
// the exhaust, over the engine_loop sample; doppler, the exhaust radiates backwards), tire noise from
// speed and the surface under the wheels (+ skids), the wind past the body (louder inside at speed),
// thumps when the suspension bottoms and from a flat tire's rim once per turn.
static void vehicle_audio(World *w, Vehicle *v, const VehicleDef *d, vec3 p, quat r, float vol, float dt) {
    vec3 fwd = quat_rotate(r, v3(0, 0, -1));
    float speed = v3_len(v->vel);
    u32 key = (v->ent * 2654435761u) | 1u;
    float far = v3_dist(p, audio_listener_pos());
    if (far > 170.0f) return;   // beyond every vehicle recipe's distance law
    bool moto = d->kind == VK_MOTO;
    const Player *lp = game_local_player();
    bool inside = lp && lp->sim.move.mode == MOVE_SEATED && lp->sim.move.vehicle == v->ent;
    AudioEmit e;
    AudioParams ap = { 0 };
    audio_emit_init(&e, "vehicle_engine", to_world(p, r, d->exhaust));
    e.fwd = fwd;
    e.vel = v->vel;
    e.volume = vol;
    e.seed = v->ent;
    e.self_radius = 2.8f;   // its own body does not occlude it
    audio_param(&ap, "rpm", v->rpm);
    audio_param(&ap, "max_rpm", d->max_rpm);
    audio_param(&ap, "throttle", fabsf(v->in_throttle));
    audio_param(&ap, "cyl", moto ? 2.0f : 4.0f);
    audio_param(&ap, "moto", moto ? 1.0f : 0.0f);
    audio_param(&ap, "running", vol > 0 ? 1.0f : 0.0f);
    if (vol > 0) audio_loop_recipe(key, &e, &ap);
    if (speed > 0.5f || v->skid_amount > 0) {
        AudioParams tp = { 0 };
        audio_emit_init(&e, "vehicle_tires", p);
        e.vel = v->vel;
        e.seed = v->ent ^ 0x7123u;
        e.self_radius = 2.8f;
        audio_param(&tp, "speed", speed);
        audio_param(&tp, "material", (float)(v->ground_mat >= 0 ? v->ground_mat : SURF_ASPHALT));
        audio_param(&tp, "skid", v->skid_amount);
        audio_loop_recipe(key ^ 0x5000u, &e, &tp);
    }
    if (speed > 5.0f) {
        AudioParams wp = { 0 };
        audio_emit_init(&e, "vehicle_wind", p);
        e.seed = v->ent ^ 0x9A1Du;
        e.self_radius = 2.8f;
        audio_param(&wp, "speed", speed);
        audio_param(&wp, "inside", inside ? 1.0f : 0.0f);
        audio_loop_recipe(key ^ 0xA000u, &e, &wp);
    }
    for (int i = 0; i < d->wheel_count; i++) {
        const VehicleWheelDef *wd = &d->wheels[i];
        float rate = dt > 1e-4f ? (v->susp[i] - v->aud_susp[i]) / dt : 0.0f;
        v->aud_susp[i] = v->susp[i];
        vec3 wc = to_world(p, r, wd->pos);
        AudioParams hp = { 0 };
        // Parked / remote cars settle and their replicated suspension steps: only a real hit, close by.
        if (fabsf(rate) > 2.5f && wd->susp_max > 0 && speed > 1.0f && far < 60.0f) {
            audio_emit_init(&e, "vehicle_thump", wc);
            e.seed = v->ent ^ (u32)i ^ (u32)(w->tick * 31u);
            e.self_radius = 2.8f;
            audio_param(&hp, "force", fabsf(rate) / 3.0f);
            audio_emit(&e, &hp);
        }
        if (tire_on_rim(&v->tire[i]) && speed > 1.0f) {
            v->aud_flat[i] += speed / (6.2831853f * fmaxf(wd->radius, 0.1f)) * dt;
            if (v->aud_flat[i] >= 1.0f) {
                v->aud_flat[i] -= 1.0f;
                audio_emit_init(&e, "vehicle_thump", wc);
                e.seed = v->ent ^ (u32)(i * 977) ^ (u32)w->tick;
                e.self_radius = 2.8f;
                audio_param(&hp, "force", clampf(speed / 12.0f, 0.2f, 1.2f));
                audio_param(&hp, "flat", 1.0f);
                audio_emit(&e, &hp);
            }
        }
    }
}

static void draw_model(u16 id, mat4 base, const Vehicle *v, int part, vec3 paint, float damage, bool brake, bool lights, bool focused) {
    const Model *mdl = assets_model(id);
    if (!mdl) return;
    for (int k = 0; k < mdl->mesh_count; k++) {
        DrawItem di;
        memset(&di, 0, sizeof(di));
        di.mesh = mdl->meshes[k].mesh;
        di.model = mat4_mul(base, mdl->meshes[k].local);
        di.mat = mdl->meshes[k].mat;
        di.object_id = obj_id(v, part * 16 + k);
        if (mdl->meshes[k].paint) {
            di.mat.color = v4(di.mat.color.x * paint.x, di.mat.color.y * paint.y, di.mat.color.z * paint.z, 1.0f);
        }
        float dk = 1.0f - damage * 0.6f;
        di.mat.color = v4(di.mat.color.x * dk, di.mat.color.y * dk, di.mat.color.z * dk, di.mat.color.w);
        if (di.mat.emissive > 0) {
            bool red = di.mat.color.x > di.mat.color.y * 3.0f;
            di.mat.emissive *= red ? (brake ? 3.0f : 0.8f) : (lights ? 1.0f : 0.05f);
        }
        if (focused) di.mat.flags |= MAT_HIGHLIGHT;
        render_draw(&di);
    }
}

static void spawn_fx(const char *name, vec3 pos, vec3 normal, vec3 vel, float scale) {
    VfxSpawnParams sp = vfx_spawn_params(pos);
    sp.normal = normal;
    sp.velocity = vel;
    sp.scale = scale;
    vfx_game_spawn(name, &sp);
}

static void skid_decal(vec3 a, vec3 b, vec3 normal, float width) {
    VfxLibrary *lib = vfx_game_library();
    if (!lib) return;
    int e = vfx_lib_find_effect(lib, "vehicle_skid");
    if (e < 0 || !lib->effects[e].decal.enabled) return;
    vec3 d = v3_sub(b, a);
    float len = v3_len(d);
    if (len < 0.05f) return;
    VfxDecalSpawn sp;
    memset(&sp, 0, sizeof(sp));
    sp.pos = v3_madd(v3_lerp(a, b, 0.5f), normal, 0.01f);
    sp.normal = normal;
    sp.tangent = v3_scale(d, 1.0f / len);
    sp.size = width;
    sp.aspect = fmaxf(len / width, 1.0f);
    sp.kind = VFX_DECAL_STATIC;
    sp.tint = v4(1, 1, 1, 1);
    vfx_sys_add_decal_ex(vfx_game_system(), &lib->effects[e].decal, &sp, (u32)(a.x * 131.0f + a.z * 71.0f));
}

static void present(World *w, Vehicle *v, vec3 cam_pos, float dt, int *spots_left) {
    const VehicleDef *d = vdef(v);
    int ei = (int)ENT_INDEX(v->ent);
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    v->steer_vis = v->steer_vis + (v->steer - v->steer_vis) * (1.0f - expf(-dt * 18.0f));
    if (v->test_steer_override) v->steer_vis = v->steer;
    float hpk = w->health[ei].max_hp > 0 ? 1.0f - saturate(w->health[ei].hp / w->health[ei].max_hp) : 0.0f;
    float damage = v->wrecked ? 1.0f : hpk * 0.5f;
    bool focused = G->focus == v->ent;
    int k = d->kind == VK_MOTO ? 1 : 0;
    mat4 base = mat4_trs(p, r, v3(1, 1, 1));
    draw_model(VG.m_body[k], base, v, 0, d->color, damage, v->braking, v->lights, focused);
    // Steering wheel / handlebar.
    vec3 pv, x, y, z;
    steer_frame(d, p, r, v->steer_vis, &pv, &x, &y, &z);
    draw_model(VG.m_steer[k], basis_mat(pv, x, y, z), v, 1, d->color, damage, false, v->lights, false);
    // Wheels: suspension, steering, roll, flat tires.
    vec3 fwd = quat_rotate(r, v3(0, 0, -1)), right = quat_rotate(r, v3(1, 0, 0)), up = quat_rotate(r, v3(0, 1, 0));
    float vf = v3_dot(v->vel, fwd);
    float smax = front_steer_deg(d);
    float speed = v3_len(v->vel);
    v->skid_amount = 0;
    v->ground_mat = -1;
    for (int i = 0; i < d->wheel_count; i++) {
        const VehicleWheelDef *wd = &d->wheels[i];
        float ks = tire_radius_scale(&v->tire[i]);
        v->wheel_rot[i] -= vf / fmaxf(wd->radius * ks, 0.05f) * dt;
        if (v->handbrake && !wd->max_steer_deg && wd->max_handbrake > 0 && speed > 1.0f) v->wheel_rot[i] += vf / fmaxf(wd->radius, 0.05f) * dt;   // locked
        v->wheel_rot[i] = fmodf(v->wheel_rot[i], 2 * DG_PI);
        float sa = wd->max_steer_deg > 0 ? -v->steer_vis * wd->max_steer_deg * DG_DEG2RAD : 0.0f;
        (void)smax;
        vec3 c = to_world(p, r, wheel_center_local(v, i));
        quat wr = quat_mul(r, quat_mul(quat_axis_angle(v3(0, 1, 0), sa), quat_axis_angle(v3(1, 0, 0), v->wheel_rot[i])));
        mat4 m = mat4_trs(c, wr, v3(1, ks, ks));
        draw_model(VG.m_wheel[k], m, v, 2 + i, d->color, damage * 0.5f, false, false, false);
        // Ground under the wheel (dust, skids, sparks): one short ray down.
        if (v3_dist(c, cam_pos) > 70.0f) continue;
        BodyId ign = v->pv ? phys_vehicle_body(v->pv) : (v->proxy_n ? v->proxy[0] : PHYS_INVALID_BODY);
        RayHit rh;
        bool ground = phys_raycast(w->phys, c, v3_scale(up, -(wd->radius * ks + 0.12f)), PL_MASK_WORLD, ign, &rh);
        if (!ground) { v->skid_on[i] = false; continue; }
        vec3 cp = rh.point;
        if (v->ground_mat < 0) v->ground_mat = rh.material;
        vec3 pv_vel = v3_add(v->vel, v3_cross(v->angvel, v3_sub(cp, p)));
        float lat = fabsf(v3_dot(pv_vel, right));
        bool locked = v->handbrake && !wd->max_steer_deg && speed > 2.0f;
        bool skid = (lat > 2.6f && speed > 3.0f) || locked || (v->braking && speed > 9.0f && fabsf(vf) > 9.0f && v->rpm < 1.0f);
        if (skid) v->skid_amount = fmaxf(v->skid_amount, clampf((lat - 2.0f) / 6.0f + (locked ? 0.6f : 0.0f), 0.2f, 1.0f));
        if (skid && rh.material != SURF_DIRT && rh.material != SURF_SAND && rh.material != SURF_GRASS && rh.material != SURF_WATER) {
            if (v->skid_on[i] && v3_dist(v->skid_last[i], cp) > 0.35f) {
                skid_decal(v->skid_last[i], cp, rh.normal, wd->width * ks);
                v->skid_last[i] = cp;
            } else if (!v->skid_on[i]) v->skid_last[i] = cp;
            v->skid_on[i] = true;
        } else v->skid_on[i] = false;
        // Dust on loose ground (and a little smoke from skids).
        bool loose = rh.material == SURF_DIRT || rh.material == SURF_SAND || rh.material == SURF_GRAVEL || rh.material == SURF_GRASS;
        float rate = loose ? fminf(speed, 25.0f) * 0.9f : (skid ? 8.0f : 0.0f);
        v->dust_acc[i] += rate * dt;
        while (v->dust_acc[i] >= 1.0f) {
            v->dust_acc[i] -= 1.0f;
            spawn_fx(loose ? "vehicle_dust" : "vehicle_exhaust", v3_madd(cp, up, 0.08f), up, v3_scale(v->vel, 0.3f), loose ? 1.0f : 1.6f);
        }
        // Sparks from a rim on the road.
        if (tire_on_rim(&v->tire[i]) && speed > 2.0f && !loose && ((int)(w->time * 30.0f + (float)i) % 3) == 0) spawn_fx("vehicle_sparks", cp, up, v->vel, 1.0f);
    }
    // Exhaust (engine running), smoke (badly damaged).
    if (v->engine || ((w->flags[ei] & EF_ENGINE) && !w->authority)) {
        float rf = d->max_rpm > 0 ? v->rpm / d->max_rpm : 0.0f;
        v->exhaust_acc += dt * (5.0f + rf * 22.0f);
        vec3 ex = to_world(p, r, d->exhaust);
        while (v->exhaust_acc >= 1.0f) {
            v->exhaust_acc -= 1.0f;
            spawn_fx("vehicle_exhaust", ex, v3_scale(fwd, -1.0f), v3_add(v3_scale(v->vel, 0.6f), v3_scale(fwd, -0.8f)), 1.0f);
        }
    }
    if (damage > 0.35f) {
        v->smoke_acc += dt * (damage - 0.3f) * 14.0f;
        vec3 hood = to_world(p, r, v3(0, d->box_center[0].y + d->box_half[0].y, d->kind == VK_MOTO ? -0.1f : -d->box_half[0].z * 0.7f));
        while (v->smoke_acc >= 1.0f) { v->smoke_acc -= 1.0f; spawn_fx("vehicle_smoke", hood, up, v->vel, 1.0f); }
    }
    // Headlights (two spot lights, nearest vehicles first: the renderer has 4 spots in all).
    if (v->lights && !v->wrecked && *spots_left > 0) {
        vec3 hl[2] = { d->headlight_l, d->headlight_r };
        int n = d->kind == VK_MOTO ? 1 : 2;
        for (int h = 0; h < n && *spots_left > 0; h++) {
            SpotLight sl;
            memset(&sl, 0, sizeof(sl));
            sl.pos = to_world(p, r, v3_add(hl[h], v3(0, 0, -0.05f)));
            sl.dir = v3_norm(v3_add(fwd, v3_scale(up, -0.08f)));
            sl.range = 45.0f;
            sl.inner_deg = 16.0f;
            sl.outer_deg = 34.0f;
            sl.intensity = 60.0f;
            sl.color = v3(1.0f, 0.95f, 0.85f);
            render_add_spot(&sl);
            (*spots_left)--;
        }
    }
    // Audio: engine, tires, wind, thumps (vehicle_audio).
    float vol = v->engine || (w->flags[ei] & EF_ENGINE) ? 0.45f + 0.35f * fminf(fabsf(v->in_throttle) + v->rpm / fmaxf(d->max_rpm, 1.0f), 1.0f) : 0.0f;
    vehicle_audio(w, v, d, p, r, vol, dt);
}

// Seated remote players follow the vehicle as it is drawn this frame (their replicated position
// lags behind a car this client predicts).
static void place_occupants(World *w, Vehicle *v) {
    const VehicleDef *d = vdef(v);
    vec3 p;
    quat r;
    pose_render(w, v, &p, &r);
    for (int s = 0; s < d->seat_count; s++) {
        if (!v->occ[s]) continue;
        int slot = v->occ[s] - 1;
        Player *pl = &w->players[slot];
        if (!pl->active || ((pl->local || slot == game_view_slot()) && !w->authority)) continue;   // predicted here (own / spectated replica)
        vec3 eye = to_world(p, r, v3_add(d->seats[s].pos, d->seats[s].eye));
        vec3 feet = v3_sub(eye, v3(0, PLAYER_EYE_STAND, 0));
        if (!w->authority) {
            pl->sim.move.pos = feet;
            int ei = (int)ENT_INDEX(pl->ent);
            w->xf[ei].pos = w->xf[ei].prev_pos = feet;
        }
    }
}

void vehicles_frame(World *w, vec3 cam_pos) {
    VehicleSys *s = w ? (VehicleSys *)w->vehicles : NULL;
    if (!s) return;
    float dt = fminf((float)g_engine.frame_dt, 0.1f);
    int spots = 2;
    // Eject ragdolls stand back up when the stun ends.
    for (int k = 0; k < MAX_PLAYERS; k++) {
        if (!s->eject[k].ent) continue;
        s->eject[k].t -= dt;
        if (s->eject[k].t <= 0) { chars_ragdoll_release(w, s->eject[k].ent); s->eject[k].ent = 0; }
    }
    // Nearest first (headlight budget).
    int order[MAX_VEH], n = 0;
    float dist[MAX_VEH];
    for (int i = 0; i < MAX_VEH; i++)
        if (s->v[i].used && ent_valid(w, s->v[i].ent)) {
            order[n] = i;
            dist[n++] = v3_dist(w->xf[ENT_INDEX(s->v[i].ent)].pos, cam_pos);
        }
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && dist[b] < dist[b - 1]; b--) {
            float t = dist[b]; dist[b] = dist[b - 1]; dist[b - 1] = t;
            int o = order[b]; order[b] = order[b - 1]; order[b - 1] = o;
        }
    for (int j = 0; j < n; j++) {
        Vehicle *v = &s->v[order[j]];
        place_occupants(w, v);
        present(w, v, cam_pos, dt, &spots);
    }
    // wakes in the wind field (docs/FOLIAGE_WIND.md): grass and bushes bend behind a moving vehicle
    static vec3 prev[MAX_VEH];
    static bool have_prev[MAX_VEH];
    static float wake_t;
    wake_t += dt;
    bool emit = wake_t >= 0.1f;
    if (emit) wake_t = 0;
    for (int i = 0; i < MAX_VEH; i++) {
        if (!s->v[i].used || !ent_valid(w, s->v[i].ent)) { have_prev[i] = false; continue; }
        vec3 p = w->xf[ENT_INDEX(s->v[i].ent)].pos;
        if (have_prev[i] && emit && dt > 0 && v3_dist(p, cam_pos) < 150.0f) wind_game_wake(p, v3_scale(v3_sub(p, prev[i]), 1.0f / 0.1f), 2.5f);
        if (emit || !have_prev[i]) prev[i] = p;
        have_prev[i] = true;
    }
}

// ------------------------------------------------------------------ HUD

// The game HUD's language (src/ui/hud.c): no boxes, soft shadows behind groups, small-caps labels in
// text_dim, big semibold numbers in hud_text, pill bars, theme colours for states, key caps like the
// interaction prompt. Bottom right, above the ammo block, inside the safe area.
#define VTH (gui_theme())

static void hud_caps(float x, float y, float size, u32 c, int align, const char *s) {
    GuiText t = { size, GW_SEMIBOLD, c, align, VTH->tracking_caps * 1.3f, true, true, 0 };
    gd_text(&t, x, y, s);
}

static void hud_bar(GuiRect r, float frac, u32 fill, u32 back) {
    gd_round(r, r.h * 0.5f, back);
    if (frac > 0.002f) gd_round(gui_rect(r.x, r.y, fmaxf(r.h, r.w * saturate(frac)), r.h), r.h * 0.5f, fill);
}

// A key cap ("F") and its action, right-aligned at x; returns the left edge.
static float hud_key(float x, float y, Action a, const char *fallback, const char *what) {
    char key[32];
    binding_name(input_get_binding(a, 0), key, sizeof(key));
    if (!key[0] || !strcmp(key, "?")) dg_strcpy(key, sizeof(key), fallback);
    GuiText t = { gui_s(VTH->t_tiny), GW_SEMIBOLD, VTH->text_dim, GUI_RIGHT, VTH->tracking_caps * 1.3f, true, true, 0 };
    float tw = gd_text_width(&t, what);
    gd_text(&t, x, y + gui_s(3), what);
    GuiText kt = { gui_s(VTH->t_tiny), GW_SEMIBOLD, VTH->text_dark, GUI_CENTER, 0, false, false, 0 };
    float ks = gui_s(18), kw = fmaxf(ks, gd_text_width(&kt, key) + gui_s(8));
    GuiRect kr = { x - tw - gui_s(6) - kw, y, kw, ks };
    gd_round(kr, gui_s(3), UI_RGBA(236, 236, 230, 225));
    gd_text(&kt, kr.x + kr.w * 0.5f, kr.y + (ks - gd_line_height(kt.size)) * 0.5f, key);
    return kr.x - gui_s(14);
}

static u32 tire_color(const TireState *t) {
    if (tire_on_rim(t)) return VTH->text_faint;
    if (t->pressure < 0.35f || t->health < 0.3f) return VTH->danger;
    if (t->pressure < 0.7f || t->health < 0.6f) return VTH->warning;
    return UI_RGBA(232, 232, 226, 220);
}

void vehicles_draw_hud(void) {
    World *w = G ? G->world : NULL;
    if (!w || !w->vehicles) return;
    int seat = 0;
    Vehicle *v = view_vehicle(w, &seat);
    // World labels for nearby vehicles (not the own one): name in caps, fuel / condition pills.
    Player *lp = game_view_player();
    VehicleSys *vs = (VehicleSys *)w->vehicles;
    for (int i = 0; lp && i < MAX_VEH; i++) {
        Vehicle *o = &vs->v[i];
        if (!o->used || o == v || !ent_valid(w, o->ent)) continue;
        vec3 p;
        quat r;
        pose_render(w, o, &p, &r);
        float d = v3_dist(p, lp->sim.move.pos);
        if (d > 18.0f) continue;
        const VehicleDef *df = vdef(o);
        vec2 px;
        float top = 0;
        for (int b = 0; b < df->box_count; b++) top = fmaxf(top, df->box_center[b].y + df->box_half[b].y);
        if (!render_world_to_screen(to_world(p, r, v3(0, top + 0.45f, 0)), &px)) continue;
        int ei = (int)ENT_INDEX(o->ent);
        float hp = w->health[ei].max_hp > 0 ? w->health[ei].hp / w->health[ei].max_hp : 0;
        float ff = df->fuel_capacity > 0 ? o->fuel / df->fuel_capacity : 0;
        float a = saturate((18.0f - d) / 6.0f);
        gd_alpha_push(a);
        hud_caps(px.x, px.y, gui_s(VTH->t_label), o->wrecked ? VTH->danger : VTH->hud_text, GUI_CENTER, o->wrecked ? "Wrecked" : df->display);
        float bw = gui_s(70), by = px.y + gui_s(20);
        hud_bar(gui_rect(px.x - bw - gui_s(3), by, bw, gui_s(4)), ff, ff < 0.15f ? VTH->warning : UI_RGBA(232, 232, 226, 220), UI_RGBA(255, 255, 255, 30));
        hud_bar(gui_rect(px.x + gui_s(3), by, bw, gui_s(4)), hp, hp < 0.3f ? VTH->danger : VTH->ok, UI_RGBA(255, 255, 255, 30));
        hud_caps(px.x - gui_s(3), by + gui_s(7), gui_s(VTH->t_tiny), VTH->text_faint, GUI_RIGHT, "Fuel");
        hud_caps(px.x + gui_s(3), by + gui_s(7), gui_s(VTH->t_tiny), VTH->text_faint, GUI_LEFT, "Condition");
        gd_alpha_pop();
    }
    if (!v) return;
    const VehicleDef *d = vdef(v);
    int ei = (int)ENT_INDEX(v->ent);
    LayoutParams lpar = gui_layout_params();
    GuiRect safe = gui_layout_safe_rect(&lpar);
    bool driver = d->seats[seat].driver;
    // Above the ammo block (draw_ammo: safe bottom - 84 px .. bottom).
    float x = safe.x + safe.w, y = safe.y + safe.h - gui_s(84) - gui_s(198);
    float colw = gui_s(250);
    gd_shadow(gui_rect(x - colw, y + gui_s(6), colw, gui_s(150)), 0, gui_s(34), UI_RGBA(0, 0, 0, 60));
    char b[64];
    // Name + seat, gear.
    snprintf(b, sizeof(b), "%s  -  %s", d->display, driver ? "Driver" : "Passenger");
    hud_caps(x, y, gui_s(VTH->t_label), VTH->text_dim, GUI_RIGHT, b);
    // Speed (km/h along the vehicle) and gear.
    vec3 fwd = quat_rotate(w->xf[ei].rot, v3(0, 0, -1));
    float kmh = fabsf(v3_dot(v->vel, fwd)) * 3.6f;
    GuiText unit = { gui_s(VTH->t_small), GW_MEDIUM, VTH->text_dim, GUI_RIGHT, 0, false, true, 0 };
    float uw = gd_text(&unit, x, y + gui_s(40), "km/h");
    snprintf(b, sizeof(b), "%.0f", kmh);
    GuiText sp = { gui_s(VTH->t_huge), GW_SEMIBOLD, VTH->hud_text, GUI_RIGHT, 0, false, true, 0 };
    float sw = gd_text(&sp, x - uw - gui_s(8), y + gui_s(16), b);
    const char *gear = v->gear < 0 ? "R" : (v->gear == 0 ? "N" : NULL);
    char gb[8];
    if (!gear) { snprintf(gb, sizeof(gb), "%d", v->gear); gear = gb; }
    GuiText gt = { gui_s(26), GW_SEMIBOLD, VTH->accent, GUI_RIGHT, 0, false, true, 0 };
    gd_text(&gt, x - uw - sw - gui_s(26), y + gui_s(26), gear);
    hud_caps(x - uw - sw - gui_s(26), y + gui_s(56), gui_s(VTH->t_tiny), VTH->text_faint, GUI_RIGHT, "Gear");
    // RPM pill.
    float rf = d->max_rpm > 0 ? saturate(v->rpm / d->max_rpm) : 0;
    float bx = x - colw, bwid = colw;
    hud_bar(gui_rect(bx, y + gui_s(70), bwid, gui_s(5)), rf, rf > 0.85f ? VTH->warning : UI_RGBA(232, 232, 226, 220), UI_RGBA(255, 255, 255, 30));
    // Fuel / condition rows: label left, value right, pill under.
    float ff = d->fuel_capacity > 0 ? saturate(v->fuel / d->fuel_capacity) : 0;
    float hp = w->health[ei].max_hp > 0 ? saturate(w->health[ei].hp / w->health[ei].max_hp) : 0;
    float ry = y + gui_s(84);
    hud_caps(bx, ry, gui_s(VTH->t_tiny), VTH->text_dim, GUI_LEFT, "Fuel");
    snprintf(b, sizeof(b), "%.1f l", v->fuel);
    GuiText val = { gui_s(VTH->t_small), GW_MEDIUM, ff < 0.15f ? VTH->warning : VTH->hud_text, GUI_RIGHT, 0, false, true, 0 };
    gd_text(&val, x, ry - gui_s(2), b);
    float low = ff < 0.1f ? 0.6f + 0.4f * sinf((float)gui_time() * 6.0f) : 1.0f;
    hud_bar(gui_rect(bx, ry + gui_s(14), bwid, gui_s(6)), ff, gui_rgba_mul(ff < 0.15f ? VTH->warning : UI_RGBA(232, 232, 226, 235), low), UI_RGBA(255, 255, 255, 30));
    ry += gui_s(28);
    hud_caps(bx, ry, gui_s(VTH->t_tiny), VTH->text_dim, GUI_LEFT, "Condition");
    snprintf(b, sizeof(b), "%.0f%%", hp * 100.0f);
    val.color = hp < 0.3f ? VTH->danger : VTH->hud_text;
    gd_text(&val, x, ry - gui_s(2), b);
    hud_bar(gui_rect(bx, ry + gui_s(14), bwid, gui_s(6)), hp, hp < 0.3f ? VTH->danger : VTH->ok, UI_RGBA(255, 255, 255, 30));
    // Tires (a small top view) and seats, left of the speed.
    float tx = bx, ty = y + gui_s(18);
    hud_caps(tx, y, gui_s(VTH->t_tiny), VTH->text_faint, GUI_LEFT, "Tires");
    for (int k = 0; k < d->wheel_count; k++) {
        const VehicleWheelDef *wd = &d->wheels[k];
        float wx = d->kind == VK_MOTO ? tx + gui_s(9) : (wd->pos.x < 0 ? tx : tx + gui_s(20));
        float wy = wd->pos.z < 0 ? ty + gui_s(4) : ty + gui_s(30);
        gd_round(gui_rect(wx, wy, gui_s(8), gui_s(16)), gui_s(4), tire_color(&v->tire[k]));
    }
    gd_round(gui_rect(tx + gui_s(9), ty + gui_s(8), gui_s(10), gui_s(34)), gui_s(5), UI_RGBA(255, 255, 255, 40));
    float sx = tx + gui_s(40);
    hud_caps(sx, y, gui_s(VTH->t_tiny), VTH->text_faint, GUI_LEFT, "Seats");
    for (int k = 0; k < d->seat_count; k++) {
        float cx = sx + gui_s(5) + (float)(k % 2) * gui_s(14), cy = ty + gui_s(10) + (float)(k / 2) * gui_s(16);
        u32 c = k == seat ? VTH->accent : (v->occ[k] ? UI_RGBA(232, 232, 226, 220) : UI_RGBA(255, 255, 255, 40));
        gd_circle(cx, cy, gui_s(4.5f), c);
    }
    // State line (only what matters right now).
    char st[96] = "";
    u32 sc = VTH->warning;
    if (v->wrecked) { dg_strcpy(st, sizeof(st), "Wrecked"); sc = VTH->danger; }
    else if (v->fuel <= 0) { dg_strcpy(st, sizeof(st), "No fuel"); sc = VTH->danger; }
    else if (!v->engine && driver) dg_strcpy(st, sizeof(st), "Engine off");
    else if (lp->sim.move.veh_onehand) { dg_strcpy(st, sizeof(st), "One-handed"); sc = VTH->accent; }
    else if (v->handbrake && kmh > 1.0f) dg_strcpy(st, sizeof(st), "Handbrake");
    if (st[0]) hud_caps(x, y + gui_s(142), gui_s(VTH->t_small), sc, GUI_RIGHT, st);
    if (v->lights) hud_caps(bx, y + gui_s(142), gui_s(VTH->t_small), VTH->info, GUI_LEFT, "Lights");
    // Key hints (actual bindings), one row under the cluster.
    float kx = x, ky = y + gui_s(162);
    kx = hud_key(kx, ky, ACT_FLASHLIGHT, "F", "Exit");
    kx = hud_key(kx, ky, ACT_VEHICLE_CAM, "V", "Camera");
    if (driver) {
        kx = hud_key(kx, ky, ACT_LIGHTS, "H", "Lights");
        hud_key(kx, ky, ACT_HORN, "Q", "Horn");
    } else hud_key(kx, ky, ACT_SLOT1, "1-4", "Seat");
}

// ------------------------------------------------------------------ debug overlay

static void draw_debug(World *w) {
    if (!VG.debug->i || !w->vehicles) return;
    VehicleSys *s = (VehicleSys *)w->vehicles;
    float y = 120 * gui_s(1.0f);
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used || !ent_valid(w, v->ent)) continue;
        const VehicleDef *d = vdef(v);
        int ei = (int)ENT_INDEX(v->ent);
        char b[256];
        snprintf(b, sizeof(b), "veh %u %s: %.1f m/s rpm %.0f gear %d steer %+.2f fuel %.1f hp %.0f occ %d%d%d%d %s%s%s | tires %.2f/%.2f %.2f/%.2f %.2f/%.2f %.2f/%.2f | pred err %.3f max %.3f snaps %d",
                 v->ent, d->name, v3_len(v->vel), v->rpm, v->gear, v->steer, v->fuel, w->health[ei].hp, v->occ[0], v->occ[1], v->occ[2], v->occ[3], v->engine ? "E" : "-",
                 v->predicted ? " P" : "", v->wrecked ? " W" : "", v->tire[0].pressure, v->tire[0].health, v->tire[1].pressure, v->tire[1].health, v->tire[2].pressure,
                 v->tire[2].health, v->tire[3].pressure, v->tire[3].health, VG.pred_err, VG.pred_err_max, VG.pred_snaps);
        ui_textf(12 * gui_s(1.0f), y, FONT_SMALL, UI_RGBA(255, 220, 160, 240), UI_ALIGN_LEFT, "%s", b);
        y += 15 * gui_s(1.0f);
        if (VG.debug->i >= 2) {
            vec3 p;
            quat r;
            pose_render(w, v, &p, &r);
            for (int k = 0; k < d->wheel_count; k++) dbg_cross(to_world(p, r, wheel_center_local(v, k)), 0.15f, tire_on_rim(&v->tire[k]) ? DBG_RED : DBG_GREEN, 0);
            vec3 g[2];
            quat gr[2];
            grips_world(w, v, g, gr);
            dbg_cross(g[0], 0.05f, DBG_YELLOW, 0);
            dbg_cross(g[1], 0.05f, DBG_YELLOW, 0);
            for (int k = 0; k < d->seat_count; k++) dbg_cross(to_world(p, r, d->seats[k].exit), 0.2f, DBG_WHITE, 0);
        }
    }
}

// ------------------------------------------------------------------ tests

static int test_check(bool ok, const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    VG.t.checks++;
    if (!ok) VG.t.fails++;
    log_info("vehicle_test %s: %s -> %s", VG.t.name, b, ok ? "ok" : "FAIL");
    return ok;
}

static void test_end(void) {
    log_info("vehicle_test %s: %d checks, %d failures -> %s", VG.t.name, VG.t.checks, VG.t.fails, VG.t.fails ? "FAIL" : "PASS");
    VG.t.mode = 0;
    if (VG.t.quit) cbuf_add("quit");
}

static Vehicle *test_vehicle(World *w) { return veh_of(w, VG.t.veh); }

static vec3 ahead_of_player(float dist) {
    Player *p = game_local_player();
    vec3 f = dir_from_angles(G->view.yaw, 0);
    return v3_madd(p->sim.move.pos, f, dist);
}

// Test modes (per tick on the host, see vehicles_test_tick):
//  1 enter  seats 0..3: enter, seat / eye position, exit through the clear socket, blocked exit,
//           ejection at speed (stun + ragdoll event)
//  2 glue   driver hands on the steering wheel: sweep -1..1, palm-to-grip error (third person)
//  3 clamp  one-handed steering clamp, spread multipliers, can-shoot rules (pistol / rifle)
//  4 fuel   full throttle until empty (fuel x200): engine dies, the car coasts to a stop
//  5 tires  bullet hits -> leak -> flat; pull towards the flat side; repair / spare items
//  6 drive  accelerate, steer right turns right, brake stops; motorcycle stays upright
//  7 zombies 50 zombies around the car: pushed but stable, driving through kills some
static void test_setup_vehicle(World *w, const char *def) {
    Player *p = game_local_player();
    if (p->sim.move.mode == MOVE_SEATED) {
        Vehicle *cur = veh_of(w, p->sim.move.vehicle);   // stop first: a test never starts by being thrown out
        if (cur) cur->vel = v3(0, 0, 0);
        vehicle_exit(w, G->local_slot, true);
    }
    p->sim.move.veh_stun = 0;
    {
        Health *hh = &w->health[ENT_INDEX(p->ent)];
        hh->hp = hh->max_hp;   // earlier tests' crashes / ejections do not carry over
    }
    VehicleSys *vs0 = sys(w);
    for (int k = 0; k < MAX_PLAYERS; k++) if (vs0->eject[k].ent == p->ent) { vs0->eject[k].t = 0; }
    VehicleSys *s = sys(w);
    for (int i = 0; i < MAX_VEH; i++)   // a clean yard: no earlier test vehicle in the way
        if (s->v[i].used) {
            EntityId e = s->v[i].ent;
            veh_release(w, &s->v[i]);
            if (ent_valid(w, e)) ent_destroy(w, e);
        }
    vec3 pos = ahead_of_player(6.0f);
    pos.y = p->sim.move.pos.y + 0.05f;
    VG.t.veh = vehicle_spawn(w, def, pos, G->view.yaw);
    Vehicle *v = test_vehicle(w);
    if (v) v->fuel = vdef(v)->fuel_capacity;
}

static void test_force_enter(World *w, int seat) {
    Vehicle *v = test_vehicle(w);
    if (v) vehicle_enter(w, G->local_slot, v, seat);
}

void vehicles_test_tick(World *w) {
    if (!VG.t.mode || !w->authority) return;
    Player *p = game_local_player();
    Vehicle *v = test_vehicle(w);
    VG.t.frames++;
    int f = VG.t.frames;
    switch (VG.t.mode) {
    case 1: {   // enter / exit
        int seat = VG.t.step / 3, phase = VG.t.step % 3;
        if (VG.t.step == 0 && f == 1) test_setup_vehicle(w, strstr(VG.t.name, "moto") ? "moto" : "car");
        v = test_vehicle(w);
        if (!v) { test_check(false, "vehicle spawned"); test_end(); return; }
        const VehicleDef *d = vdef(v);
        if (seat >= d->seat_count) {
            // Ejection at speed: driver seat, the car rolling at 14 m/s.
            if (f == 10) test_force_enter(w, 0);
            if (f == 40) {
                vec3 vv = v3_scale(quat_rotate(w->xf[ENT_INDEX(v->ent)].rot, v3(0, 0, -1)), 14.0f);
                phys_set_velocity(w->phys, phys_vehicle_body(v->pv), vv, v3(0, 0, 0));
                v->prev_vel = v->vel = vv;   // a set velocity, not an impact
                v->impact_cd = 1.0f;
            }
            if (f == 42) vehicle_exit(w, G->local_slot, false);
            if (f == 44) {
                test_check(p->sim.move.mode == MOVE_WALK && p->sim.move.veh_stun > 0.5f, "exit at 14 m/s: stunned %.2f s", p->sim.move.veh_stun);
                test_check(v3_len(p->sim.move.vel) > 8.0f, "thrown with the car's speed (%.1f m/s)", v3_len(p->sim.move.vel));
            }
            if (f == 260) {
                test_check(p->sim.move.veh_stun <= 0.0f && p->sim.move.mode == MOVE_WALK, "stun over, standing");
                test_end();
            }
            return;
        }
        if (phase == 0 && f == 10) test_force_enter(w, seat);
        if (phase == 0 && f == 20) {
            vec3 vp;
            quat vr;
            pose_sim(w, v, &vp, &vr);
            vec3 eye = to_world(vp, vr, v3_add(d->seats[seat].pos, d->seats[seat].eye));
            vec3 pe = v3_add(p->sim.move.pos, v3(0, PLAYER_EYE_STAND, 0));
            test_check(p->sim.move.mode == MOVE_SEATED && p->sim.move.seat == seat && v->occ[seat] == G->local_slot + 1, "seat %d occupied", seat);
            test_check(v3_dist(eye, pe) < 0.02f, "seat %d eye error %.1f mm", seat, v3_dist(eye, pe) * 1000.0f);
            VG.t.step++;
            VG.t.frames = 0;
        }
        if (phase == 1 && f == 5) vehicle_exit(w, G->local_slot, false);
        if (phase == 1 && f == 10) {
            vec3 vp;
            quat vr;
            pose_sim(w, v, &vp, &vr);
            vec3 ex = to_world(vp, vr, d->seats[seat].exit);
            float e = v3_dist(v3_flat(ex), v3_flat(p->sim.move.pos));
            test_check(p->sim.move.mode == MOVE_WALK && !v->occ[seat], "seat %d exit", seat);
            test_check(e < 0.05f, "seat %d exit socket (%.2f m from it)", seat, e);
            test_check(!phys_capsule_overlap(w->phys, v3_add(p->sim.move.pos, v3(0, 0.06f, 0)), PLAYER_RADIUS, PLAYER_HEIGHT, PL_MASK_WORLD, phys_character_inner_body(p->character)),
                       "seat %d exit is clear", seat);
            VG.t.step++;
            VG.t.frames = 0;
        }
        if (phase == 2) { VG.t.step++; VG.t.frames = 0; }
        return;
    }
    case 2: {   // hand glue sweep
        if (f == 1) { test_setup_vehicle(w, strstr(VG.t.name, "moto") ? "moto" : "car"); cvar_set_int(cvar_find("cl_thirdperson"), 1); }
        v = test_vehicle(w);
        if (!v) { test_end(); return; }
        if (f == 10) test_force_enter(w, 0);
        if (f > 40 && f <= 280) {
            // Nine steering angles from full left to full right, 26 frames each, measured once the
            // pose has settled on each (the hands follow the wheel at the pose rate).
            v->test_steer_override = true;
            int stepi = (f - 41) / 26;
            v->steer = -1.0f + 0.25f * (float)DG_MIN(stepi, 8);
            if ((f - 41) % 26 == 25) {
                vec3 g[2], pl[2];
                quat gr[2];
                grips_world(w, v, g, gr);
                for (int h = 0; h < 2; h++)
                    if (chars_palm_world(w, p->ent, h, &pl[h])) {
                        float e = v3_dist(pl[h], g[h]);
                        VG.t.val[h] = fmaxf(VG.t.val[h], e);
                        VG.t.val[2 + h] += e;
                    }
                VG.t.val[4] += 1.0f;
            }
        }
        if (f == 290) {
            v->test_steer_override = false;
            float n = fmaxf(VG.t.val[4], 1.0f);
            test_check(VG.t.val[0] < 0.03f && VG.t.val[1] < 0.03f, "palm to grip max L %.1f mm R %.1f mm (mean %.1f / %.1f) over the sweep", VG.t.val[0] * 1000.0f,
                       VG.t.val[1] * 1000.0f, VG.t.val[2] / n * 1000.0f, VG.t.val[3] / n * 1000.0f);
            cvar_set_int(cvar_find("cl_thirdperson"), 0);
            test_end();
        }
        return;
    }
    case 3: {   // one-hand clamp
        if (f == 1) {
            test_setup_vehicle(w, "car");
            inv_add(&p->inv, w->items, item_find(w->items, "pistol"), 1, 12);
            inv_add(&p->inv, w->items, item_find(w->items, "rifle"), 1, 30);
        }
        if (f == 5)
            for (int k = 0; k < INV_SEL_WEAPONS; k++) {
                const WeaponDef *wd = item_weapon(w->items, inv_sel_c(&p->inv, k)->item);
                if (wd && wd->one_hand_capable) { game_select(k); break; }
            }
        v = test_vehicle(w);
        if (!v) { test_end(); return; }
        if (f == 10) test_force_enter(w, 0);
        if (f == 20) {
            // The real path: commands with aim held (pistol in slot of the pistol).
            const VehicleDef *d = vdef(v);
            UserCmd c;
            memset(&c, 0, sizeof(c));
            c.move_x = 127;
            c.buttons = BTN_AIM;
            const WeaponDef *wd = weapon_equipped_def(w, G->local_slot);
            vehicles_player_cmd(w, G->local_slot, &c, 0, true);
            v->in_steer = 1.0f;
            v->onehand = p->sim.move.veh_onehand != 0;
            for (int k = 0; k < 60; k++) drive(w, v, SIM_DT);
            bool oh = p->sim.move.veh_onehand != 0;
            test_check(!wd || !wd->one_hand_capable || oh, "one-hand mode with %s", wd ? "the held weapon" : "no weapon");
            if (oh) {
                test_check(fabsf(v->steer) <= d->one_hand_steer_limit + 1e-3f, "steering clamped to %.2f (limit %.2f)", v->steer, d->one_hand_steer_limit);
                test_check(fabsf(hook_spread(w, G->local_slot) - d->one_hand_spread_mult) < 0.05f, "one-hand spread x%.2f", hook_spread(w, G->local_slot));
                test_check(hook_can_shoot(w, G->local_slot, &p->sim, wd), "driver may shoot one-handed");
            }
            c.buttons = 0;
            vehicles_player_cmd(w, G->local_slot, &c, 0, true);
            v->onehand = p->sim.move.veh_onehand != 0;
            for (int k = 0; k < 60; k++) drive(w, v, SIM_DT);
            v->in_steer = 0;
            test_check(fabsf(v->steer) > d->one_hand_steer_limit + 0.1f, "full steering with both hands (%.2f)", v->steer);
            test_check(!hook_can_shoot(w, G->local_slot, &p->sim, wd), "no shooting with both hands on the wheel");
            test_check(holstered(w, G->local_slot), "weapon holstered while driving");
            // A rifle is never one-hand capable.
            const WeaponDef *rifle = NULL;
            for (int k = 0; k < G->items.weapon_count; k++) if (!strcmp(G->items.weapons[k].id, "rifle")) rifle = &G->items.weapons[k];
            PlayerSim fake = p->sim;
            fake.move.veh_onehand = 1;
            test_check(rifle && !hook_can_shoot(w, G->local_slot, &fake, rifle), "rifle: no one-handed fire");
            // Passenger: may shoot, passenger spread.
            vehicle_exit(w, G->local_slot, true);
        }
        if (f == 30) test_force_enter(w, 1);
        if (f == 40) {
            test_check(hook_can_shoot(w, G->local_slot, &p->sim, weapon_equipped_def(w, G->local_slot)), "passenger may shoot");
            test_check(fabsf(hook_spread(w, G->local_slot) - vdef(v)->passenger_spread_mult) < 0.05f, "passenger spread x%.2f", hook_spread(w, G->local_slot));
            test_check(!holstered(w, G->local_slot), "passenger keeps the weapon");
            test_end();
        }
        return;
    }
    case 4: {   // fuel
        if (f == 1) { test_setup_vehicle(w, "car"); cvar_set_float(VG.fuel_scale, 60.0f); }
        v = test_vehicle(w);
        if (!v) { test_end(); return; }
        if (f == 10) { test_force_enter(w, 0); v->fuel = 1.5f; VG.t.val[0] = v->fuel; }
        if (f > 12 && f < 1500) {
            v->in_throttle = v->fuel > 0 ? 1.0f : 0.0f;
            if (v->fuel <= 0 && VG.t.val[1] == 0) { VG.t.val[1] = (float)f; VG.t.val[2] = v3_len(v->vel); }
        }
        if (f == 1500) {
            test_check(v->fuel <= 0 && !v->engine, "ran dry after %.1f s (engine %s)", (VG.t.val[1] - 12) / 60.0f, v->engine ? "on" : "off");
            test_check(VG.t.val[2] > 5.0f, "was driving (%.1f m/s at empty)", VG.t.val[2]);
            test_check(v3_len(v->vel) < VG.t.val[2] * 0.6f, "coasting down (%.1f m/s)", v3_len(v->vel));
            const VehicleDef *d = vdef(v);
            test_check(vehicle_fuel_rate(d, 1, 1) > vehicle_fuel_rate(d, 0.5f, 0.5f) && vehicle_fuel_rate(d, 0.5f, 0.5f) > vehicle_fuel_rate(d, 0, 0),
                       "consumption curve %.4f > %.4f > %.4f l/s", vehicle_fuel_rate(d, 1, 1), vehicle_fuel_rate(d, 0.5f, 0.5f), vehicle_fuel_rate(d, 0, 0));
            cvar_set_float(VG.fuel_scale, 1.0f);
            test_end();
        }
        return;
    }
    case 5: {   // tires
        if (f == 1) test_setup_vehicle(w, "car");
        v = test_vehicle(w);
        if (!v) { test_end(); return; }
        const VehicleDef *d = vdef(v);
        if (f == 10) {
            test_force_enter(w, 0);
            // Real bullets: a ray from the side into the front-left wheel.
            vec3 vp;
            quat vr;
            pose_sim(w, v, &vp, &vr);
            vec3 c = to_world(vp, vr, wheel_center_local(v, 0));
            vec3 o = v3_add(c, quat_rotate(vr, v3(-3.0f, 0.05f, 0)));
            vec3 dir = v3_norm(v3_sub(c, o));
            EntityId ve = 0;
            float t = 0;
            int wh = hook_ray_tires(w, o, dir, 10.0f, 0, &ve, &t);
            test_check(wh == 0 && ve == v->ent, "ray hits the front-left wheel (wheel %d at %.2f m)", wh, t);
            for (int k = 0; k < 3; k++) hook_tire_hit(w, v->ent, 0, c, dir);
            test_check(tire_on_rim(&v->tire[0]), "three hits: flat (pressure %.2f, health %.2f)", v->tire[0].pressure, v->tire[0].health);
            float gl, gt;
            tire_grip(&v->tire[0], &gl, &gt);
            test_check(gl < 0.5f && gt < 0.5f, "flat grip %.2f / %.2f", gl, gt);
            test_check(vehicle_tire_pull(d, v->tire) < -0.05f, "pull to the left %.2f", vehicle_tire_pull(d, v->tire));
            VG.t.val[0] = quat_get_yaw(vr);
        }
        if (f > 12 && f < 300) { v->in_throttle = 0.6f; v->in_steer = 0.0f; }
        if (f == 300) {
            float dy = quat_get_yaw(w->xf[ENT_INDEX(v->ent)].rot) - VG.t.val[0];
            while (dy > DG_PI) dy -= 2 * DG_PI;
            while (dy < -DG_PI) dy += 2 * DG_PI;
            test_check(dy > 0.02f, "drifted left with no steering (%.1f deg)", dy * DG_RAD2DEG);   // yaw grows turning left
            v->in_throttle = 0;
            vehicle_exit(w, G->local_slot, true);
        }
        if (f == 400) {
            const ItemDef *rep = NULL, *spare = NULL;
            for (int k = 0; k < G->items.item_count; k++) {
                if (G->items.items[k].vehicle_use == VUSE_TIRE_REPAIR) rep = &G->items.items[k];
                if (G->items.items[k].vehicle_use == VUSE_SPARE_TIRE) spare = &G->items.items[k];
            }
            vec3 vp;
            quat vr;
            pose_sim(w, v, &vp, &vr);
            p->sim.move.pos = to_world(vp, vr, v3(d->wheels[0].pos.x - 0.8f, 0, d->wheels[0].pos.z));
            bool r1 = rep && hook_use_item(w, G->local_slot, rep);
            test_check(!r1, "repair kit refused on a destroyed tire");
            bool r2 = spare && hook_use_item(w, G->local_slot, spare);
            test_check(r2 && v->tire[0].pressure > 0.99f, "spare tire fitted (pressure %.2f)", v->tire[0].pressure);
            hook_tire_hit(w, v->ent, 1, vp, v3(1, 0, 0));
            p->sim.move.pos = to_world(vp, vr, v3(d->wheels[1].pos.x + 0.8f, 0, d->wheels[1].pos.z));
            for (int k = 0; k < 120; k++) tire_tick(&v->tire[1], SIM_DT);
            float before = v->tire[1].pressure;
            bool r3 = rep && hook_use_item(w, G->local_slot, rep);
            test_check(r3 && v->tire[1].leak == 0 && v->tire[1].pressure >= 0.85f, "repair kit plugs a puncture (%.2f -> %.2f)", before, v->tire[1].pressure);
            test_end();
        }
        return;
    }
    case 6: {   // drive
        bool moto = strstr(VG.t.name, "moto") != NULL;
        if (f == 1) test_setup_vehicle(w, moto ? "moto" : "car");
        v = test_vehicle(w);
        if (!v) { test_end(); return; }
        int ei = (int)ENT_INDEX(v->ent);
        if (f == 10) { test_force_enter(w, 0); VG.t.val[0] = quat_get_yaw(w->xf[ei].rot); VG.t.val[5] = 0; }
        if (f > 12 && f <= 372) { v->in_throttle = 1.0f; v->in_steer = 0; }
        vec3 up = quat_rotate(w->xf[ei].rot, v3(0, 1, 0));
        if (f > 12) VG.t.val[5] = fmaxf(VG.t.val[5], acosf(clampf(up.y, -1, 1)) * DG_RAD2DEG);
        if (VG.debug->i && f % 15 == 0) {
            vec3 rt = quat_rotate(w->xf[ei].rot, v3(1, 0, 0));
            log_info("vehicle_test %s: f %d speed %.1f tilt %.1f roll %.1f steer %.2f gear %d rpm %.0f", VG.t.name, f, v3_len(v->vel), acosf(clampf(up.y, -1, 1)) * DG_RAD2DEG,
                     asinf(clampf(rt.y, -1, 1)) * DG_RAD2DEG, v->steer, v->gear, v->rpm);
        }
        if (f == 372) {
            VG.t.val[1] = v3_len(v->vel);
            test_check(VG.t.val[1] > (moto ? 14.0f : 12.0f), "6 s full throttle: %.1f m/s (%.0f km/h), gear %d", VG.t.val[1], VG.t.val[1] * 3.6f, v->gear);
            VG.t.val[2] = quat_get_yaw(w->xf[ei].rot);
            VG.t.val[6] = VG.t.val[5];
        }
        if (f > 372 && f <= 492) { v->in_throttle = 0.5f; v->in_steer = 1.0f; }
        if (f == 492) {
            float dy = quat_get_yaw(w->xf[ei].rot) - VG.t.val[2];
            while (dy > DG_PI) dy -= 2 * DG_PI;
            while (dy < -DG_PI) dy += 2 * DG_PI;
            test_check(dy < -0.3f, "steer right turns right (yaw %.0f deg)", dy * DG_RAD2DEG);
            test_check(VG.t.val[6] < (moto ? 25.0f : 10.0f), "upright going straight (max tilt %.1f deg)", VG.t.val[6]);
            VG.t.val[3] = v3_len(v->vel);
            VG.t.val[4] = 0;
        }
        if (f > 492 && f <= 852) {
            v->in_steer = 0;
            // Brake while rolling forwards; let go once stopped (holding it would reverse).
            v->in_throttle = 0;
            v->in_brake = 1.0f;
            if (v3_len(v->vel) < 0.3f && VG.t.val[4] == 0) VG.t.val[4] = (float)(f - 492) / 60.0f;
        }
        if (f == 852) {
            test_check(VG.t.val[4] > 0 && VG.t.val[4] < 5.0f, "braked from %.1f m/s to a stop in %.1f s", VG.t.val[3], VG.t.val[4]);
            test_check(VG.t.val[5] < 60.0f, "never rolled over (max tilt %.1f deg)", VG.t.val[5]);
            v->in_brake = 0;
            test_end();
        }
        return;
    }
    case 7: {   // zombies
        if (f == 1) test_setup_vehicle(w, "car");
        if (f == 12) {
            // A pack 18 m ahead (direct call: a queued command would wait for the script's own wait).
            vec3 ahead = to_world(w->xf[ENT_INDEX(v->ent)].pos, w->xf[ENT_INDEX(v->ent)].rot, v3(0, 0, -18.0f));
            horde_spawn(w, ahead, 50, 4.0f, HSPAWN_CLUSTER, "mixed");
        }
        v = test_vehicle(w);
        if (!v) { test_end(); return; }
        int ei = (int)ENT_INDEX(v->ent);
        if (f == 10) { test_force_enter(w, 0); VG.t.val[0] = 0; VG.runover_hits = VG.runover_kills = 0; }
        if (f > 10) {
            vec3 pp = w->xf[ei].pos;
            if (!isfinite(pp.x) || !isfinite(pp.y)) VG.t.val[0] = 1;
            vec3 up = quat_rotate(w->xf[ei].rot, v3(0, 1, 0));
            VG.t.val[1] = fmaxf(VG.t.val[1], acosf(clampf(up.y, -1, 1)) * DG_RAD2DEG);
        }
        if (f > 40 && f < 700) { v->in_throttle = f < 400 ? 1.0f : ((f / 90) % 2 ? -0.8f : 0.9f); v->in_steer = f < 400 ? 0.0f : sinf((float)f * 0.02f); }
        if (VG.debug->i && f % 30 == 0) {
            float nd = 1e9f;
            int nz = 0;
            vec3 vp = w->xf[ei].pos;
            ENT_FOREACH(w, i) if (w->type[i] == ET_ZOMBIE) { nz++; nd = fminf(nd, v3_dist(w->xf[i].pos, vp)); }
            log_info("vehicle_test zombies: f %d speed %.1f zombies %d nearest %.2f m hits %d", f, v3_len(v->vel), nz, nd, VG.runover_hits);
        }
        if (f == 700) {
            int alive = 0;
            ENT_FOREACH(w, i) if (w->type[i] == ET_ZOMBIE && !(w->flags[i] & EF_DEAD)) alive++;
            test_check(VG.t.val[0] == 0, "no invalid transform");
            test_check(VG.t.val[1] < 60.0f, "stayed upright (max tilt %.1f deg)", VG.t.val[1]);
            test_check(VG.runover_hits > 0, "drove through the horde: %d hits, %d kills (%d zombies standing)", VG.runover_hits, VG.runover_kills, alive);
            test_check(w->health[ei].hp > 0 || v->wrecked, "vehicle hp %.0f", w->health[ei].hp);
            v->in_throttle = 0;
            horde_despawn(w);
            test_end();
        }
        return;
    }
    case 8: {   // flood: dropped into the deepest lake with the driver in it
        const WaterWorld *ww = game_water();
        int lake = -1;
        float best = 0;
        for (int i = 0; i < ww->count; i++) {
            const WaterBody *b = &ww->body[i];
            float area = (b->bmax[0] - b->bmin[0]) * (b->bmax[1] - b->bmin[1]);
            if (b->kind == WATER_LAKE && area > best) { best = area; lake = i; }
        }
        if (lake < 0) { test_check(false, "a lake on this map (map wateryard)"); test_end(); return; }
        const WaterBody *lb = &ww->body[lake];
        vec3 c = v3((lb->bmin[0] + lb->bmax[0]) * 0.5f, lb->level, (lb->bmin[1] + lb->bmax[1]) * 0.5f);
        bool moto = strstr(VG.t.name, "moto") != NULL;
        if (f == 1) test_setup_vehicle(w, moto ? "moto" : "car");
        v = test_vehicle(w);
        if (!v || !v->pv) { test_end(); return; }
        const VehicleDef *d = vdef(v);
        BodyId body = phys_vehicle_body(v->pv);
        if (f == 2) {
            // Teleport over the lake's middle, then sit the driver in.
            phys_set_transform(w->phys, body, v3_add(c, v3(0, 0.6f, 0)), quat_yaw(0), true);
            phys_set_velocity(w->phys, body, v3(0, 0, 0), v3(0, 0, 0));
            v->flood = 0;
            v->drowned = false;
            v->intake_t = 0;
            p->sim.move.pos = v3_add(c, v3(3, 1, 0));
            if (p->character) phys_character_set_pos(p->character, p->sim.move.pos);
            p->sim.move.oxygen = player_breath_max();
        }
        if (f == 4) { test_force_enter(w, 0); VG.t.val[0] = -1; VG.t.val[1] = -1; }
        vec3 pos;
        quat rot;
        pose_sim(w, v, &pos, &rot);
        float t = (float)(f - 4) / (float)SIM_HZ;
        if (f > 4 && v->drowned && VG.t.val[0] < 0) VG.t.val[0] = t;
        float top = pos.y + d->box_center[d->box_count > 1 ? 1 : 0].y + d->box_half[d->box_count > 1 ? 1 : 0].y;
        if (f > 4 && top < lb->level - 0.2f && VG.t.val[1] < 0) VG.t.val[1] = t;
        if (VG.debug->i && f % 60 == 0) log_info("vehicle_test flood: t %.1f y %.2f flood %.2f engine %d drowned %d submerged %.2f", t, pos.y, v->flood, v->engine, v->drowned, game_water_buoy_of(body) ? game_water_buoy_of(body)->submerged : -1.0f);
        if (!moto && f == 4 + SIM_HZ) {
            test_check(pos.y > lb->level - 1.6f && v->engine, "1 s in: afloat (origin %.2f, surface %.2f), engine running", pos.y, lb->level);
        }
        if (f == 4 + 40 * SIM_HZ) {
            if (moto) {
                test_check(VG.t.val[1] >= 0 && VG.t.val[1] < 8.0f, "motorcycle under within 8 s (%.1f s)", VG.t.val[1]);
            } else {
                test_check(VG.t.val[0] >= d->flood_time - 0.5f && VG.t.val[0] <= d->flood_time + 8.0f, "engine flooded after %.1f s (flood_time %.1f)", VG.t.val[0], d->flood_time);
                test_check(VG.t.val[1] > 3.0f && VG.t.val[1] < 30.0f, "car floated, then sank under after %.1f s (flood %.2f)", VG.t.val[1], v->flood);
                test_check(!v->engine && !v->lights, "engine and electrics dead");
            }
            test_check(p->sim.move.oxygen < player_breath_max() - 2.0f, "driver holding breath in the sunk vehicle (%.1f s left)", p->sim.move.oxygen);
            vehicle_exit(w, G->local_slot, false);
        }
        if (f == 4 + 46 * SIM_HZ) {
            test_check(p->sim.move.mode == MOVE_SWIM, "got out and swims (mode %d, water %d)", p->sim.move.mode, p->sim.move.water);
            test_end();
        }
        return;
    }
    default: test_end(); return;
    }
}

static void cmd_vehicle_test(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority || !game_local_player()) { log_info("vehicle_test: host with a local player"); return; }
    const char *what = argc > 1 ? argv[1] : "";
    static const char *modes[] = { "", "enter", "glue", "clamp", "fuel", "tires", "drive", "zombies", "flood" };
    int m = 0;
    char base[16];
    dg_strcpy(base, sizeof(base), what);
    size_t L = strlen(base);
    if (L > 5 && !strcmp(base + L - 5, "_moto")) base[L - 5] = 0;
    for (int i = 1; i < 9; i++) if (!strcmp(base, modes[i])) m = i;
    if (!strcmp(what, "dump")) {
        VehicleSys *s = (VehicleSys *)w->vehicles;
        for (int i = 0; s && i < MAX_VEH; i++) {
            Vehicle *v = &s->v[i];
            if (!v->used) continue;
            vec3 p;
            quat r;
            pose_sim(w, v, &p, &r);
            log_info("vehicle_test dump: %s veh %u pos %.2f %.2f %.2f yaw %.1f vel %.2f fuel %.2f occ %d%d%d%d | pred err avg %.3f max %.3f snaps %d",
                     w->authority ? "host" : "client", v->ent, p.x, p.y, p.z, quat_get_yaw(r) * DG_RAD2DEG, v3_len(v->vel), v->fuel, v->occ[0], v->occ[1], v->occ[2], v->occ[3],
                     VG.pred_err_n ? VG.pred_err_sum / (float)VG.pred_err_n : 0.0f, VG.pred_err_max, VG.pred_snaps);
        }
        return;
    }
    if (!m) { log_info("usage: vehicle_test enter|enter_moto|glue|glue_moto|clamp|fuel|tires|drive|drive_moto|zombies|flood|flood_moto|dump [quit]"); return; }
    memset(&VG.t, 0, sizeof(VG.t));
    VG.t.mode = m;
    dg_strcpy(VG.t.name, sizeof(VG.t.name), what);
    VG.t.quit = argc > 2 && !strcmp(argv[2], "quit");
    log_info("vehicle_test %s: start", what);
}

// Client-side dump (the net test compares host and client).
static void cmd_vehicle_dump_client(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    World *w = G ? G->world : NULL;
    VehicleSys *s = w ? (VehicleSys *)w->vehicles : NULL;
    for (int i = 0; s && i < MAX_VEH; i++) {
        Vehicle *v = &s->v[i];
        if (!v->used) continue;
        vec3 p;
        quat r;
        pose_sim(w, v, &p, &r);
        log_info("vehicle_dump: %s veh %u pos %.2f %.2f %.2f yaw %.1f vel %.2f fuel %.2f occ %d%d%d%d predicted %d | pred err avg %.3f max %.3f n %d snaps %d",
                 w->authority ? "host" : "client", v->ent, p.x, p.y, p.z, quat_get_yaw(r) * DG_RAD2DEG, v3_len(v->vel), v->fuel, v->occ[0], v->occ[1], v->occ[2], v->occ[3],
                 v->predicted, VG.pred_err_n ? VG.pred_err_sum / (float)VG.pred_err_n : 0.0f, VG.pred_err_max, VG.pred_err_n, VG.pred_snaps);
    }
}

// ------------------------------------------------------------------ commands

static Vehicle *cmd_target(World *w) {
    Vehicle *v = local_vehicle(w, NULL);
    if (v) return v;
    Player *p = game_local_player();
    float d;
    return p ? nearest_vehicle(w, p->sim.move.pos, 30.0f, &d) : NULL;
}

static void cmd_spawn_vehicle(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority || !game_local_player()) { log_info("spawn_vehicle: host only"); return; }
    float dist = argc > 2 ? (float)atof(argv[2]) : 6.0f;
    vec3 pos = ahead_of_player(dist);
    pos.y = game_local_player()->sim.move.pos.y + 0.3f;
    vehicle_spawn(w, argc > 1 ? argv[1] : "car", pos, G->view.yaw);
}

static void cmd_vehicle_fuel(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    Vehicle *v = w && w->authority ? cmd_target(w) : NULL;
    if (!v) { log_info("vehicle_fuel: no vehicle (host)"); return; }
    if (argc > 1) {
        const char *a = argv[1];
        float x = (float)atof(a);
        v->fuel = clampf(a[strlen(a) - 1] == '%' ? x * 0.01f * vdef(v)->fuel_capacity : x, 0.0f, vdef(v)->fuel_capacity);
    }
    log_info("vehicle_fuel: %.2f / %.0f l", v->fuel, vdef(v)->fuel_capacity);
}

static void cmd_vehicle_tire(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    Vehicle *v = w && w->authority ? cmd_target(w) : NULL;
    if (!v || argc < 3) { log_info("usage: vehicle_tire <wheel 0..> <pressure 0..1> [health 0..1] (host)"); return; }
    int k = atoi(argv[1]);
    if (k < 0 || k >= vdef(v)->wheel_count) return;
    v->tire[k].pressure = clampf((float)atof(argv[2]), 0, 1);
    if (argc > 3) v->tire[k].health = clampf((float)atof(argv[3]), 0, 1);
    v->tire[k].leak = 0;
    log_info("vehicle_tire %d: pressure %.2f health %.2f", k, v->tire[k].pressure, v->tire[k].health);
}

static void cmd_vehicle_repair(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    World *w = G ? G->world : NULL;
    Vehicle *v = w && w->authority ? cmd_target(w) : NULL;
    if (!v) return;
    int ei = (int)ENT_INDEX(v->ent);
    for (int k = 0; k < VEH_MAX_WHEELS; k++) tire_init(&v->tire[k]);
    v->wrecked = false;
    w->comps[ei] |= C_HEALTH;
    w->health[ei].hp = w->health[ei].max_hp;
    v->fuel = vdef(v)->fuel_capacity;
}

static void cmd_vehicle_enter(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    if (!w) return;
    if (w->authority && argc > 1) {
        Vehicle *v = cmd_target(w);
        if (v) vehicle_enter(w, G->local_slot, v, atoi(argv[1]));
        return;
    }
    VG.req_vehicle = true;
}

static void cmd_vehicle_exit(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (local_vehicle(G ? G->world : NULL, NULL)) VG.req_vehicle = true;
}

// vehicle_put <player slot> [seat]: seats a player in the nearest vehicle to the host (tests).
static void cmd_vehicle_put(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority || argc < 2) { log_info("usage: vehicle_put <player slot> [seat] (host)"); return; }
    int slot = atoi(argv[1]);
    if (slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) { log_info("vehicle_put: no player %d", slot); return; }
    Vehicle *v = cmd_target(w);
    if (!v) { log_info("vehicle_put: no vehicle near"); return; }
    if (w->players[slot].sim.move.mode == MOVE_SEATED) vehicle_exit(w, slot, true);
    bool ok = vehicle_enter(w, slot, v, argc > 2 ? atoi(argv[2]) : -1);
    log_info("vehicle_put: player %d %s", slot, ok ? "seated" : "could not enter");
}

static void cmd_vehicle_seat(int argc, const char **argv) {
    if (argc > 1) VG.req_seat = (u8)DG_CLAMP(atoi(argv[1]), 1, 4);
}

static void cmd_vehicle_cam(int argc, const char **argv) { VG.cam_tp = argc > 1 ? atoi(argv[1]) != 0 : !VG.cam_tp; }
static void cmd_vehicle_lights(int argc, const char **argv) { VG.lights = argc > 1 ? atoi(argv[1]) != 0 : !VG.lights; }

void vehicles_draw(void) {
    World *w = G ? G->world : NULL;
    if (w) draw_debug(w);
}

void vehicles_register(void) {
    VG.debug = cvar_reg_int("vehicle_debug", 0, 0, 2, CVAR_LOCKED, "vehicle overlay: 1 state, 2 + wheels / grips / exits");
    VG.fuel_scale = cvar_reg_float("vehicle_fuel_scale", 1.0f, 0.0f, 1000.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "fuel consumption multiplier (0 = infinite fuel)");
    cmd_register("spawn_vehicle", cmd_spawn_vehicle, "spawn_vehicle <car|moto> [distance]: in front of you (host)");
    cmd_register("vehicle_fuel", cmd_vehicle_fuel, "vehicle_fuel [litres | N%]: current / nearest vehicle (host)");
    cmd_register("vehicle_tire", cmd_vehicle_tire, "vehicle_tire <wheel> <pressure> [health] (host)");
    cmd_register("vehicle_repair", cmd_vehicle_repair, "vehicle_repair: tires, hp, fuel (host)");
    cmd_register("vehicle_enter", cmd_vehicle_enter, "vehicle_enter [seat]: get in (the focused / nearest vehicle)");
    cmd_register("vehicle_exit", cmd_vehicle_exit, "vehicle_exit: get out");
    cmd_register("vehicle_put", cmd_vehicle_put, "vehicle_put <player slot> [seat]: seat a player in the nearest vehicle (host, tests)");
    cmd_register("vehicle_seat", cmd_vehicle_seat, "vehicle_seat <1-4>: change seats");
    cmd_register("vehicle_cam", cmd_vehicle_cam, "vehicle_cam [0|1]: third-person vehicle camera");
    cmd_register("vehicle_lights", cmd_vehicle_lights, "vehicle_lights [0|1]: headlights (driver)");
    cmd_register("vehicle_test", cmd_vehicle_test, "vehicle_test enter|glue|clamp|fuel|tires|drive|zombies[_moto]|dump [quit]");
    cmd_register("vehicle_dump", cmd_vehicle_dump_client, "vehicle_dump: vehicle state on this machine (net tests)");
    movement_set_seat_pose(seat_pose_move);
    g_vehicle_pose.seat_pose = seat_pose_char;
    g_vehicle_pose.seat_hands = seat_hands;
    g_vehicle_pose.holstered = holstered;
    g_vehicle_pose.vehicle_xform = vehicle_xform;
    g_vehicle_pose.vehicle_body = hook_ignore;
    g_vehicle_pose.seat_vehicle = seat_vehicle;
    g_vehicle_pose.seat_driver = seat_driver;
    WeaponVehicleHooks h = { hook_can_shoot, hook_spread, hook_ignore, hook_ray_tires, hook_tire_hit, hook_use_item };
    weapons_set_vehicle_hooks(&h);
    entities_set_interact_def(ET_VEHICLE, &k_vehicle_def);
    entities_set_interact_def(ET_FUEL_PUMP, &k_pump_def);
    repl_set_owned_hook(owned_hook);
    ensure_lib();
}

// Thermal scope (game_thermal.c, docs/NIGHT_OPTICS.md): each vehicle's heat sources in world space and
// what drives them (replicated: engine, rpm, brake flag, velocity). The engine sits at the end opposite
// the exhaust.
int vehicles_thermal(World *w, VehThermal *out, int max) {
    VehicleSys *s = w ? (VehicleSys *)w->vehicles : NULL;
    if (!s || !VG.lib_ok) return 0;
    int n = 0;
    for (int i = 0; i < MAX_VEH && n < max; i++) {
        const Vehicle *v = &s->v[i];
        if (!v->used || !ent_valid(w, v->ent)) continue;
        const VehicleDef *d = vdef(v);
        const Transform *x = &w->xf[ENT_INDEX(v->ent)];
        VehThermal *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->ent = v->ent;
        o->engine = v->engine && !v->wrecked && !v->drowned;
        o->load = d->max_rpm > 0.0f ? clampf(v->rpm / d->max_rpm, 0.0f, 1.0f) : 0.0f;
        o->braking = v->braking || v->handbrake;
        o->speed = v3_len(v->vel);
        vec3 c = d->box_count ? d->box_center[0] : v3(0, 0.5f, 0);
        vec3 h = d->box_count ? d->box_half[0] : v3(0.8f, 0.5f, 2.0f);
        float back = d->exhaust.z >= 0.0f ? 1.0f : -1.0f;
        o->engine_pos = v3_add(x->pos, quat_rotate(x->rot, v3(c.x, c.y + h.y * 0.2f, c.z - back * h.z * 0.6f)));
        o->engine_r = fmaxf(h.x, 0.4f);
        o->exhaust_pos = v3_add(x->pos, quat_rotate(x->rot, d->exhaust));
        o->wheel_count = DG_MIN(d->wheel_count, 4);
        for (int k = 0; k < o->wheel_count; k++) {
            o->wheel_pos[k] = v3_add(x->pos, quat_rotate(x->rot, d->wheels[k].pos));
            o->wheel_r[k] = d->wheels[k].radius;
        }
        o->ground = x->pos;
    }
    return n;
}
