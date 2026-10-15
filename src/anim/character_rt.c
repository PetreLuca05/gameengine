#include "character_rt.h"
#include <math.h>

void charrt_init(CharRuntime *r, const AnimLibrary *lib, const CharacterDef *def) {
    memset(r, 0, sizeof(*r));
    r->lib = lib;
    r->def = def;
    int set = def ? def->anim_set_index : 0;
    if (set < 0 || set >= lib->set_count) set = 0;
    ctrl_init(&r->ctrl, &lib->ctrl, &lib->sets[set]);
    proc_reset(&r->proc);
    pose_bind(&lib->skel, &r->base);
    pose_bind(&lib->skel, &r->final);
    pose_to_model(&lib->skel, &r->base, r->base_model);
    memcpy(r->final_model, r->base_model, sizeof(Xform) * (size_t)lib->skel.count);
    r->in.grounded = true;
    r->override_clip = -1;
    r->in.health = def ? def->hp : 100;
}

// Lower-body yaw for a move direction (x right, y forward): forward sector -> forward clip turned
// by the angle, backward sector -> backward clip turned, sideways -> strafe clips straight.
static float leg_yaw_for(vec2 m, float speed) {
    if (speed < 0.3f) return 0.0f;
    float a = atan2f(m.x, m.y);                       // 0 forward, +90 right, 180 back
    const float lim = 60.0f * DG_DEG2RAD;
    if (fabsf(a) <= lim) return a;                    // forward diagonals
    if (fabsf(a) >= DG_PI - lim) return wrap_angle(a - DG_PI);   // backward diagonals
    return 0.0f;                                      // strafe
}

void charrt_set_inputs(CharRuntime *r, const CharInputs *in) {
    r->in = *in;
    ControllerInst *c = &r->ctrl;
    ctrl_set(c, AP_SPEED, in->speed);
    r->leg_yaw_target = leg_yaw_for(in->move_local, in->speed);
    // The blend tree gets the direction left after the legs turned (pure forward on diagonals);
    // the smoothed leg yaw makes sector changes blend instead of pop.
    float cs = cosf(r->leg_yaw), sn = sinf(r->leg_yaw);
    vec2 m = v2(in->move_local.x * cs - in->move_local.y * sn, in->move_local.x * sn + in->move_local.y * cs);
    float ml = v2_len(m);
    if (ml > 1e-4f) m = v2_scale(m, 1.0f / ml);
    r->blend_dir_target = m;
    if (v2_len(r->blend_dir) < 1e-3f) r->blend_dir = m;
    ctrl_set(c, AP_MOVE_X, r->blend_dir.x);
    ctrl_set(c, AP_MOVE_Y, r->blend_dir.y);
    ctrl_set(c, AP_GROUNDED, in->grounded ? 1.0f : 0.0f);
    ctrl_set(c, AP_CROUCH, in->crouch ? 1.0f : 0.0f);
    ctrl_set(c, AP_PRONE, in->prone ? 1.0f : 0.0f);
    ctrl_set(c, AP_SPRINT, in->sprint ? 1.0f : 0.0f);
    ctrl_set(c, AP_ADS, in->ads ? 1.0f : 0.0f);
    ctrl_set(c, AP_MANTLE, in->mantling ? 1.0f : 0.0f);
    ctrl_set(c, AP_MANTLE_T, in->mantle_t);
    ctrl_set(c, AP_HANG, in->hanging ? 1.0f : 0.0f);
    ctrl_set(c, AP_VAULT, in->vaulting ? 1.0f : 0.0f);
    ctrl_set(c, AP_VERTICAL_VEL, in->vertical_vel);
    ctrl_set(c, AP_TURN_ANGLE, wrap_angle(in->aim_yaw - in->body_yaw));
    ctrl_set(c, AP_AIM_PITCH, in->aim_pitch);
    ctrl_set(c, AP_AIM_YAW, in->aim_yaw);
    ctrl_set(c, AP_WEAPON_TYPE, (float)in->weapon_type);
    ctrl_set(c, AP_WEAPON_ID, (float)in->weapon_id);
    ctrl_set(c, AP_HEALTH, in->health);
    ctrl_set(c, AP_LIMB_STATUS, (float)in->limb_status);
    ctrl_set(c, AP_DEAD, in->dead ? 1.0f : 0.0f);
    ctrl_set(c, AP_ATTACKING, in->attacking ? 1.0f : 0.0f);
    int tc = ctrl_param_index(c->def, "turn_clips");
    if (tc >= 0) ctrl_set(c, tc, r->lib->proc.turn_mode == 1 ? 1.0f : 0.0f);
    int hc = ctrl_param_index(c->def, "hit_clips");
    if (hc >= 0) ctrl_set(c, hc, r->lib->proc.hit_mode != 0 ? 1.0f : 0.0f);
}

void charrt_trigger(CharRuntime *r, int p) { ctrl_trigger(&r->ctrl, p); }

void charrt_tick(CharRuntime *r, float dt) {
    // Ease the blend-tree direction (angle) and speed: direction / speed changes blend the clips
    // over ~0.15 s instead of switching (deterministic, per tick).
    {
        float a0 = atan2f(r->blend_dir.x, r->blend_dir.y), a1 = atan2f(r->blend_dir_target.x, r->blend_dir_target.y);
        float a = wrap_angle(a0 + wrap_angle(a1 - a0) * (1.0f - expf(-dt * 14.0f)));
        r->blend_dir = v2(sinf(a), cosf(a));
        float sp = r->in.speed;
        r->blend_speed += (sp - r->blend_speed) * (1.0f - expf(-dt * 12.0f));
        ctrl_set(&r->ctrl, AP_MOVE_X, r->blend_dir.x);
        ctrl_set(&r->ctrl, AP_MOVE_Y, r->blend_dir.y);
        ctrl_set(&r->ctrl, AP_SPEED, r->blend_speed);
    }
    ctrl_tick(&r->ctrl, &r->lib->lookup, dt);
    // Legs turn towards the move direction at up to 360 deg/s with an exponential ease.
    float d = wrap_angle(r->leg_yaw_target - r->leg_yaw);
    float step = d * (1.0f - expf(-dt * 10.0f));
    float maxs = 6.2832f * dt;
    r->leg_yaw = wrap_angle(r->leg_yaw + clampf(step, -maxs, maxs));
}

// Hips (and everything below) yawed by leg_yaw about the hips; Spine counter-rotated so the
// upper body is unchanged.
static void apply_leg_yaw(CharRuntime *r) {
    if (fabsf(r->leg_yaw) < 1e-4f) return;
    const Skeleton *s = &r->lib->skel;
    int hips = s->role[BR_HIPS], spine = s->role[BR_SPINE];
    if (hips < 0 || spine < 0 || s->parent[spine] != hips) return;
    // Character faces -Z: a positive (rightward) leg yaw is a negative rotation about +Y.
    quat q = quat_axis_angle(v3(0, 1, 0), -r->leg_yaw);
    Xform *H = &r->base.local[hips], *S = &r->base.local[spine];
    quat hr = H->r;
    Xform spine_model = xf_mul(*H, *S);
    H->r = quat_norm(quat_mul(q, hr));
    Xform inv_h = xf_inverse(*H);
    *S = xf_mul(inv_h, spine_model);
    S->s = v3(1, 1, 1);
}

void charrt_eval_base(CharRuntime *r) {
    if (r->override_clip >= 0 && r->override_clip < r->lib->clip_count) {
        Pose tmp;
        clip_sample(&r->lib->clips[r->override_clip], r->override_time, r->override_mirror ? &tmp : &r->base);
        if (r->override_mirror) pose_mirror(&r->lib->skel, &tmp, &r->base);
    } else {
        ctrl_evaluate(&r->ctrl, &r->lib->skel, &r->lib->lookup, &r->base);
        apply_leg_yaw(r);
        // Holding an item: the arms (clavicles down) don't play the locomotion clip - they take a
        // fixed relaxed pose (first frame of idle) and the procedural hold places the hands.
        float hw = 0;
        const Skeleton *s = &r->lib->skel;
        int idle = r->ctrl.bind ? r->ctrl.bind->fallback_clip : -1;
        if (r->in.weapon_id && ctrl_hand_ik_weight(&r->ctrl, &hw) && hw > 0 && idle >= 0 && idle < r->lib->clip_count) {
            static Pose still;
            clip_sample(&r->lib->clips[idle], 0.0f, &still);
            int roots[2] = { s->role[BR_L_SHOULDER], s->role[BR_R_SHOULDER] };
            for (int b = 0; b < s->count; b++)
                if ((roots[0] >= 0 && skel_is_descendant(s, b, roots[0])) || (roots[1] >= 0 && skel_is_descendant(s, b, roots[1])))
                    r->base.local[b] = xf_lerp(r->base.local[b], still.local[b], hw);
        }
    }
    pose_to_model(&r->lib->skel, &r->base, r->base_model);
    r->base_valid = true;
    r->evals++;
}

void charrt_to_world(const CharRuntime *r, const Xform *model, Xform root, float scale, Xform *world) {
    Xform rs = root;
    rs.s = v3(scale, scale, scale);
    for (int i = 0; i < r->lib->skel.count; i++) world[i] = xf_mul(rs, model[i]);
}

void charrt_proc_input(const CharRuntime *r, ProcInput *in, Xform root, float dt) {
    memset(in, 0, sizeof(*in));
    in->skel = &r->lib->skel;
    in->params = &r->lib->proc;
    in->hands = &r->lib->hands;
    in->root = root;
    in->dt = dt;
    in->body_yaw = r->in.body_yaw;
    in->aim_yaw = r->in.aim_yaw;
    in->aim_pitch = r->in.aim_pitch;
    in->grounded = r->in.grounded && !r->in.hanging && !r->in.mantling;
    in->crouched = r->in.crouch;
    in->sprinting = r->in.sprint;
    in->moving = r->in.speed > 0.25f;
    in->ads_t = r->in.ads ? 1.0f : 0.0f;
    in->hold_style = r->in.weapon_type;
    ctrl_hand_ik_weight(&r->ctrl, &in->hand_ik_weight);
    in->foot_ik = in->spine = in->turn = in->hold = in->lean = in->damage = true;
}

void charrt_post(CharRuntime *r, ProcInput *in) {
    const Skeleton *s = &r->lib->skel;
    pose_copy(&r->final, &r->base);
    memcpy(r->final_model, r->base_model, sizeof(Xform) * (size_t)s->count);
    // Additive layers before the hand solve (the solve corrects for them). Hand / finger bones
    // are masked out of the reaction while an item is held.
    proc_damage_apply(&r->proc, in, &r->final, r->final_model);
    proc_run(&r->proc, in, &r->final, r->final_model);
}
