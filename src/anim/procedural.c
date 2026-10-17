// Procedural post-pass: parameters, hand frames and the fixed-order driver.
#include "procedural_internal.h"
#include "cJSON.h"
#include "core/log.h"
#include <math.h>
#include <stdio.h>

static const char *k_style_names[HOLD_COUNT] = { "none", "pistol", "rifle", "shotgun", "smg", "lmg", "melee_1h", "melee_2h", "item_small", "item_large",
                                                 "phone", "phone_landscape" };

static HoldStyle default_style(int id) {
    HoldStyle h;
    memset(&h, 0, sizeof(h));
    dg_strcpy(h.name, sizeof(h.name), k_style_names[id]);
    h.left_hand = true;
    h.elbow_r = v3(0.7f, -1.0f, 0.3f);
    h.elbow_l = v3(-0.6f, -1.0f, 0.0f);
    h.max_shift = 0.3f;    // upper bound; the hold point's reach_pull is the per-style limit
    // Fingers: assets/handposes/styles/<name>.json (docs/HANDPOSE.md).
    switch (id) {
    case HOLD_NONE: h.left_hand = false; break;
    case HOLD_PISTOL: h.elbow_l = v3(-0.8f, -1.0f, 0.2f); break;
    case HOLD_MELEE_1H: h.left_hand = false; break;
    case HOLD_ITEM_SMALL: h.left_hand = false; break;
    case HOLD_PHONE: h.left_hand = false; h.elbow_r = v3(0.6f, -1.0f, 0.1f); break;
    case HOLD_PHONE_LANDSCAPE:
        h.elbow_r = v3(0.7f, -1.0f, 0.1f);
        h.elbow_l = v3(-0.7f, -1.0f, 0.1f);
        break;
    default: break;
    }
    holdpoint_style_defaults(&h.hp, h.name);
    return h;
}

static vec3 jv(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static void jarr(const cJSON *o, const char *k, float *out, int n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v)) return;
    for (int i = 0; i < n && i < cJSON_GetArraySize(v); i++) out[i] = (float)cJSON_GetArrayItem(v, i)->valuedouble;
}

static DamagePreset preset(const char *name, float strength, float falloff, int depth, float k, float c, float drop, float push) {
    DamagePreset d;
    memset(&d, 0, sizeof(d));
    dg_strcpy(d.name, sizeof(d.name), name);
    d.strength = strength;
    d.falloff = falloff;
    d.chain_depth = depth;
    d.stiffness = k;
    d.damping = c;
    d.pelvis_drop = drop;
    d.translate = push;
    return d;
}

bool proc_params_load(ProcParams *p, const cJSON *root) {
    memset(p, 0, sizeof(*p));
    float sw[5] = { 0.18f, 0.22f, 0.25f, 0.15f, 0.20f };
    memcpy(p->spine_weights, sw, sizeof(sw));
    float swu[5] = { 0.0f, 0.02f, 0.05f, 0.38f, 0.55f };
    memcpy(p->spine_weights_up, swu, sizeof(swu));
    float swa[5] = { 0.40f, 0.35f, 0.25f, 0.0f, 0.0f };
    memcpy(p->spine_weights_ads_up, swa, sizeof(swa));
    p->yaw_limit = 100;
    p->pitch_limit = 80;
    p->pitch_limit_up = 89;
    p->turn_threshold = 65;
    p->turn_speed = 260;
    p->turn_mode = 0;
    p->foot_lock_release = 0.22f;
    p->foot_ray_up = 0.5f;
    p->foot_ray_down = 0.6f;
    p->pelvis_max_drop = 0.35f;
    p->foot_align = 0.8f;
    p->foot_smooth = 18.0f;
    p->foot_height = 0.08f;
    p->slope_lean = 0.35f;
    p->accel_lean = 0.012f;
    p->style_blend_time = 0.2f;
    for (int i = 0; i < HOLD_COUNT; i++) p->styles[i] = default_style(i);
    p->style_count = HOLD_COUNT;
    p->presets[0] = preset("head", 26, 0.45f, 3, 240, 28, 0, 0);
    p->presets[1] = preset("torso", 13, 0.6f, 3, 150, 22, 0, 0.04f);
    p->presets[2] = preset("arm", 32, 0.5f, 3, 200, 25, 0, 0);
    p->presets[3] = preset("leg", 16, 0.5f, 2, 170, 23, 0.09f, 0);
    p->preset_count = 4;
    // Body parts (gameplay/hitboxes.h order): head neck chest stomach pelvis, arms, hands, legs, feet.
    static const int map[17] = { 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3 };
    for (int i = 0; i < 17; i++) p->part_preset[i] = map[i];
    if (root) {
        const cJSON *pr = cJSON_GetObjectItemCaseSensitive(root, "procedural");
        if (pr) {
            const cJSON *sp = cJSON_GetObjectItemCaseSensitive(pr, "spine");
            if (sp) { jarr(sp, "weights", p->spine_weights, 5); jarr(sp, "weights_up", p->spine_weights_up, 5); jarr(sp, "weights_ads_up", p->spine_weights_ads_up, 5); p->yaw_limit = jf(sp, "yaw_limit", p->yaw_limit); p->pitch_limit = jf(sp, "pitch_limit", p->pitch_limit); p->pitch_limit_up = jf(sp, "pitch_limit_up", p->pitch_limit_up); }
            const cJSON *tu = cJSON_GetObjectItemCaseSensitive(pr, "turn");
            if (tu) {
                p->turn_threshold = jf(tu, "threshold", p->turn_threshold);
                p->turn_speed = jf(tu, "speed", p->turn_speed);
                const cJSON *m = cJSON_GetObjectItemCaseSensitive(tu, "mode");
                if (cJSON_IsString(m)) p->turn_mode = !strcmp(m->valuestring, "clips") ? 1 : 0;
                p->foot_lock_release = jf(tu, "foot_lock_release", p->foot_lock_release);
            }
            const cJSON *fo = cJSON_GetObjectItemCaseSensitive(pr, "foot_ik");
            if (fo) {
                p->foot_ray_up = jf(fo, "ray_up", p->foot_ray_up);
                p->foot_ray_down = jf(fo, "ray_down", p->foot_ray_down);
                p->pelvis_max_drop = jf(fo, "pelvis_max_drop", p->pelvis_max_drop);
                p->foot_align = jf(fo, "align", p->foot_align);
                p->foot_smooth = jf(fo, "smooth", p->foot_smooth);
                p->foot_height = jf(fo, "foot_height", p->foot_height);
            }
            const cJSON *le = cJSON_GetObjectItemCaseSensitive(pr, "lean");
            if (le) { p->slope_lean = jf(le, "slope", p->slope_lean); p->accel_lean = jf(le, "accel", p->accel_lean); }
        }
        const cJSON *hs = cJSON_GetObjectItemCaseSensitive(root, "hold_styles");
        const cJSON *e;
        if (hs) {
            p->style_blend_time = jf(hs, "blend_time", p->style_blend_time);
            cJSON_ArrayForEach(e, hs) {
                if (!cJSON_IsObject(e)) continue;
                int id = proc_hold_style_index(p, e->string);
                if (id < 0) {
                    if (p->style_count >= HOLD_STYLE_MAX) continue;
                    id = p->style_count++;
                    p->styles[id] = default_style(HOLD_RIFLE);
                    dg_strcpy(p->styles[id].name, 16, e->string);
                }
                HoldStyle *h = &p->styles[id];
                const cJSON *lh = cJSON_GetObjectItemCaseSensitive(e, "left_hand");
                if (cJSON_IsBool(lh)) h->left_hand = cJSON_IsTrue(lh);
                h->elbow_r = jv(e, "elbow_r", h->elbow_r);
                h->elbow_l = jv(e, "elbow_l", h->elbow_l);
                h->shoulder_r = jv(e, "shoulder_r", h->shoulder_r);
                h->shoulder_l = jv(e, "shoulder_l", h->shoulder_l);
                h->max_shift = jf(e, "max_shift", h->max_shift);
            }
        }
        const cJSON *dr = cJSON_GetObjectItemCaseSensitive(root, "damage_reactions");
        if (dr) {
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(dr, "hit_mode");
            if (cJSON_IsString(mode)) p->hit_mode = !strcmp(mode->valuestring, "clips") ? 1 : (!strcmp(mode->valuestring, "both") ? 2 : 0);
            cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(dr, "presets")) {
                if (!cJSON_IsObject(e)) continue;
                int id = -1;
                for (int i = 0; i < p->preset_count; i++) if (!strcmp(p->presets[i].name, e->string)) id = i;
                if (id < 0 && p->preset_count < DMG_PRESET_MAX) { id = p->preset_count++; p->presets[id] = preset(e->string, 15, 0.5f, 2, 180, 18, 0, 0); }
                if (id < 0) continue;
                DamagePreset *d = &p->presets[id];
                d->strength = jf(e, "strength", d->strength);
                d->falloff = jf(e, "falloff", d->falloff);
                d->chain_depth = (int)jf(e, "chain_depth", (float)d->chain_depth);
                d->stiffness = jf(e, "stiffness", d->stiffness);
                d->damping = jf(e, "damping", d->damping);
                d->pelvis_drop = jf(e, "pelvis_drop", d->pelvis_drop);
                d->translate = jf(e, "translate", d->translate);
            }
            const cJSON *parts = cJSON_GetObjectItemCaseSensitive(dr, "parts");
            static const char *part_names[17] = { "head", "neck", "chest", "stomach", "pelvis", "upper_arm_l", "upper_arm_r", "lower_arm_l",
                                                  "lower_arm_r", "hand_l", "hand_r", "upper_leg_l", "upper_leg_r", "lower_leg_l",
                                                  "lower_leg_r", "foot_l", "foot_r" };
            for (int i = 0; i < 17 && parts; i++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(parts, part_names[i]);
                if (!cJSON_IsString(v)) continue;
                for (int k = 0; k < p->preset_count; k++) if (!strcmp(p->presets[k].name, v->valuestring)) p->part_preset[i] = k;
            }
        }
    }
    proc_params_rehash(p);
    return true;
}

void proc_params_rehash(ProcParams *p) {
    u32 h = ANIM_HASH_SEED;
    // The hash covers raw bytes: zero the padding of every hold point pose first (struct copies
    // may leave it undefined), so every machine computes the same asset manifest.
    for (int i = 0; i < p->style_count; i++)
        for (int k = 0; k < HP_STATE_COUNT; k++) {
            HpPose t = p->styles[i].hp.state[k];
            memset(&p->styles[i].hp.state[k], 0, sizeof(HpPose));
            p->styles[i].hp.state[k].pos = t.pos;
            p->styles[i].hp.state[k].rot = t.rot;
            p->styles[i].hp.state[k].set = t.set;
        }
    for (int i = 0; i < p->style_count; i++) {
        p->styles[i].hash = 0;
        p->styles[i].hash = anim_hash(&p->styles[i], sizeof(HoldStyle), ANIM_HASH_SEED);
        h = anim_hash(&p->styles[i].hash, sizeof(u32), h);
    }
    p->hash = h;
}

// assets/human/holdpoint.json: { "styles": { "<hold style>": { states, eye_relief, procedural,
// collision, ... } } } on top of the built-in defaults (holdpoint_style_defaults).
bool proc_holdpoint_load(ProcParams *p, const cJSON *root) {
    for (int i = 0; i < p->style_count; i++) holdpoint_style_defaults(&p->styles[i].hp, p->styles[i].name);
    if (root) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "styles");
        const cJSON *e;
        cJSON_ArrayForEach(e, st) {
            int id = proc_hold_style_index(p, e->string);
            if (id < 0) { log_warn("holdpoint: unknown hold style '%s'", e->string); continue; }
            holdpoint_style_parse(&p->styles[id].hp, e);
        }
    }
    proc_params_rehash(p);
    return root != NULL;
}

const HoldStyle *proc_hold_style(const ProcParams *p, int style) {
    if (style < 0 || style >= p->style_count) style = HOLD_NONE;
    return &p->styles[style];
}

int proc_hold_style_index(const ProcParams *p, const char *name) {
    for (int i = 0; i < p->style_count; i++) if (!strcmp(p->styles[i].name, name)) return i;
    return -1;
}

// ------------------------------------------------------------------ hand frames

static quat quat_from_axes(vec3 x, vec3 y, vec3 z) {
    // Columns x, y, z of a rotation matrix.
    float m00 = x.x, m11 = y.y, m22 = z.z;
    float tr = m00 + m11 + m22;
    quat q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q.w = 0.25f * s;
        q.x = (y.z - z.y) / s;
        q.y = (z.x - x.z) / s;
        q.z = (x.y - y.x) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2;
        q.w = (y.z - z.y) / s;
        q.x = 0.25f * s;
        q.y = (y.x + x.y) / s;
        q.z = (z.x + x.z) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2;
        q.w = (z.x - x.z) / s;
        q.x = (y.x + x.y) / s;
        q.y = 0.25f * s;
        q.z = (z.y + y.z) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2;
        q.w = (x.y - y.x) / s;
        q.x = (z.x + x.z) / s;
        q.y = (z.y + y.z) / s;
        q.z = 0.25f * s;
    }
    return quat_norm(q);
}

void proc_hand_frames(const Skeleton *s, HandFrames *out) {
    int hands[2] = { s->role[BR_L_HAND], s->role[BR_R_HAND] };
    int mids[2] = { s->role[BR_L_MIDDLE1], s->role[BR_R_MIDDLE1] };
    for (int h = 0; h < 2; h++) {
        out->palm[h] = xf_identity();
        out->curl_axis[h] = v3(-1, 0, 0);
        out->thumb_side[h] = 1.0f;
        if (hands[h] < 0) continue;
        Xform hb = s->bind_model[hands[h]];
        vec3 tip = mids[h] >= 0 ? s->bind_model[mids[h]].t : xf_point(hb, v3(0, 0.1f, 0));
        vec3 f = v3_sub(tip, hb.t);
        float len = v3_len(f);
        f = v3_scale(f, 1.0f / fmaxf(len, 1e-4f));
        vec3 up = v3(0, 1, 0);   // T-pose: palms face down, back of the hand up
        vec3 b = v3_norm(v3_sub(up, v3_scale(f, v3_dot(up, f))));
        vec3 z = v3_neg(f), y = b, x = v3_cross(y, z);
        Xform palm;
        palm.r = quat_from_axes(x, y, z);
        palm.t = v3_sub(v3_madd(hb.t, f, len * 0.45f), v3_scale(b, 0.012f));
        palm.s = v3(1, 1, 1);
        out->palm[h] = xf_mul(xf_inverse(hb), palm);
        int th = s->role[h ? BR_R_THUMB1 : BR_L_THUMB1];
        out->thumb_side[h] = 1.0f;
        if (th >= 0 && mids[h] >= 0) {
            vec3 d = quat_rotate(quat_conj(palm.r), v3_sub(s->bind_model[th].t, s->bind_model[mids[h]].t));
            out->thumb_side[h] = d.x >= 0 ? 1.0f : -1.0f;
        }
    }
}

// ------------------------------------------------------------------ driver

void proc_reset(ProcState *s) { memset(s, 0, sizeof(*s)); s->style = s->prev_style = -1; }

void proc_rotate_bone(const Skeleton *s, Pose *p, Xform *model, int b, quat q) {
    if (b < 0) return;
    Xform n = model[b];
    n.r = quat_norm(quat_mul(q, n.r));
    pose_set_model(s, p, model, b, n);
    pose_update_children(s, p, model, b);
}

// Brings the arm root (shoulder joint `arm`) within `reach - margin` of `target` by rotating the
// clavicle `clav` towards it (at most max_deg): shoulder protraction / elevation instead of moving
// the held item. Returns the remaining shortfall (0 = in reach).
float proc_clavicle_reach(const Skeleton *s, Pose *p, Xform *model, int clav, int arm, vec3 target, float reach, float margin, float max_deg) {
    if (clav < 0 || arm < 0) return 0;
    float need = v3_dist(model[arm].t, target) - (reach - margin);
    if (need <= 0) return 0;
    vec3 c = model[clav].t;
    vec3 v = v3_sub(model[arm].t, c), u = v3_sub(target, c);
    vec3 axis = v3_cross(v, u);
    if (v3_len2(axis) < 1e-10f) return need;
    axis = v3_norm(axis);
    // Distance to the target falls monotonically while rotating v towards u: bisect the angle.
    float lo = 0, hi = fminf(max_deg * DG_DEG2RAD, acosf(clampf(v3_dot(v3_norm(v), v3_norm(u)), -1.0f, 1.0f)));
    for (int i = 0; i < 20; i++) {
        float mid = 0.5f * (lo + hi);
        vec3 a = v3_add(c, quat_rotate(quat_axis_angle(axis, mid), v));
        if (v3_dist(a, target) > reach - margin) lo = mid; else hi = mid;
    }
    proc_rotate_bone(s, p, model, clav, quat_axis_angle(axis, hi));
    return fmaxf(0.0f, v3_dist(model[arm].t, target) - (reach - margin));
}

// Seated pose without a clip: pelvis onto the seat, thighs flexed forward, knees bent, feet IK'd
// to the pedals (knees forward and a little out).
void proc_seated(ProcState *st, const ProcInput *in, Pose *pose, Xform *model) {
    (void)st;
    const Skeleton *s = in->skel;
    Xform inv = xf_inverse(in->root);
    int hips = s->role[BR_HIPS];
    if (hips < 0) return;
    Xform n = model[hips];
    n.t = xf_point(inv, in->seat_hip);
    pose_set_model(s, pose, model, hips, n);
    pose_update_children(s, pose, model, hips);
    static const BoneRole leg[2][3] = { { BR_L_UPLEG, BR_L_LEG, BR_L_FOOT }, { BR_R_UPLEG, BR_R_LEG, BR_R_FOOT } };
    for (int k = 0; k < 2; k++) {
        int a = s->role[leg[k][0]], b = s->role[leg[k][1]], c = s->role[leg[k][2]];
        if (a < 0 || b < 0 || c < 0) continue;
        // Hip flexion first (the IK then only bends the knee towards the pedal).
        proc_rotate_bone(s, pose, model, a, quat_axis_angle(v3(1, 0, 0), 80.0f * DG_DEG2RAD));
        vec3 target = xf_point(inv, in->seat_feet[k]);
        quat foot = model[c].r;
        anim_ik_two_bone_pose(s, pose, model, a, b, c, target, &foot, v3(k ? 0.25f : -0.25f, 0.4f, -1.0f), 1.0f);
    }
}

void proc_run(ProcState *st, const ProcInput *in0, Pose *pose, Xform *model) {
    ProcInput in = *in0;
    if (!st->init) {
        st->init = true;
        st->render_body_yaw = in.body_yaw;
        st->prev_vel = in.velocity;
        st->style = st->prev_style = in.hold_style;
        st->style_t = 1;
    }
    if (in.seated) {
        // The body faces the vehicle (no turn-in-place lag), no ground feet, no lean.
        st->render_body_yaw = in.body_yaw;
        in.foot_ik = false;
        in.lean = false;
    } else proc_turn(st, &in);
    in.root.r = quat_yaw(st->render_body_yaw);
    if (in.seated) proc_seated(st, &in, pose, model);
    float foot_off[2] = { 0, 0 };
    vec3 anim_foot[2] = { { 0 }, { 0 } };
    if (in.foot_ik) proc_foot_pelvis(st, &in, pose, model, foot_off, anim_foot);
    if (in.lean) proc_lean(st, &in, pose, model);
    if (in.spine) proc_spine(st, &in, pose, model);
    if (in.hold && in.item) proc_hold(st, &in, pose, model);
    else st->has_item = false;
    if (in.hand_anchor_w > 0) proc_hand_anchors(st, &in, pose, model);
    if (in.foot_ik) proc_foot_legs(st, &in, pose, model, foot_off, anim_foot);
    st->prev_vel = in.velocity;
}
