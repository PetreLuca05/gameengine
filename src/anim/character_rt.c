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

void charrt_set_inputs(CharRuntime *r, const CharInputs *in) {
    r->in = *in;
    ControllerInst *c = &r->ctrl;
    ctrl_set(c, AP_SPEED, in->speed);
    ctrl_set(c, AP_MOVE_X, in->move_local.x);
    ctrl_set(c, AP_MOVE_Y, in->move_local.y);
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

void charrt_tick(CharRuntime *r, float dt) { ctrl_tick(&r->ctrl, &r->lib->lookup, dt); }

void charrt_eval_base(CharRuntime *r) {
    if (r->override_clip >= 0 && r->override_clip < r->lib->clip_count) {
        Pose tmp;
        clip_sample(&r->lib->clips[r->override_clip], r->override_time, r->override_mirror ? &tmp : &r->base);
        if (r->override_mirror) pose_mirror(&r->lib->skel, &tmp, &r->base);
    } else {
        ctrl_evaluate(&r->ctrl, &r->lib->skel, &r->lib->lookup, &r->base);
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
