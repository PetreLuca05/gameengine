#include "anim.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

// ------------------------------------------------------------------ quaternion helpers

quat quat_from_to(vec3 a, vec3 b) {
    float d = v3_dot(a, b);
    if (d >= 1.0f - 1e-7f) return quat_identity();
    if (d <= -1.0f + 1e-7f) {
        vec3 axis = v3_cross(v3(1, 0, 0), a);
        if (v3_len2(axis) < 1e-6f) axis = v3_cross(v3(0, 1, 0), a);
        return quat_axis_angle(axis, DG_PI);
    }
    vec3 c = v3_cross(a, b);
    quat q = { c.x, c.y, c.z, 1.0f + d };
    return quat_norm(q);
}

vec3 quat_to_scaled_axis(quat q) {
    if (q.w < 0.0f) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    float s = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-7f) return v3(q.x * 2.0f, q.y * 2.0f, q.z * 2.0f);
    float angle = 2.0f * atan2f(s, q.w);
    return v3_scale(v3(q.x, q.y, q.z), angle / s);
}

quat quat_from_scaled_axis(vec3 v) {
    float angle = v3_len(v);
    if (angle < 1e-7f) { quat q = { v.x * 0.5f, v.y * 0.5f, v.z * 0.5f, 1.0f }; return quat_norm(q); }
    return quat_axis_angle(v3_scale(v, 1.0f / angle), angle);
}

float quat_angle_between(quat a, quat b) {
    // atan2 form: exact for tiny angles (acos of a dot product near 1 loses ~0.5 deg in float).
    quat d = quat_mul(a, quat_conj(b));
    float v = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0f * atan2f(v, fabsf(d.w));
}

quat quat_twist(quat q, vec3 axis) {
    vec3 r = v3(q.x, q.y, q.z);
    vec3 p = v3_scale(axis, v3_dot(r, axis));
    quat t = { p.x, p.y, p.z, q.w };
    float l = t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w;
    if (l < 1e-12f) return quat_identity();
    return quat_norm(t);
}

// ------------------------------------------------------------------ skeleton

static const char *base_name(const char *n) {
    const char *c = strrchr(n, ':');
    return c ? c + 1 : n;
}

// Exact name first, then ignoring the namespace prefix: Mixamo exports name the same rig
// "mixamorig:Hips", "mixamorig1:Hips", "mixamorig8:Hips" ... depending on the download.
int skel_find(const Skeleton *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->names[i], name)) return i;
    const char *b = base_name(name);
    for (int i = 0; i < s->count; i++)
        if (!strcmp(base_name(s->names[i]), b)) return i;
    return -1;
}

int skel_find_suffix(const Skeleton *s, const char *suffix) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(base_name(s->names[i]), suffix)) return i;
    return -1;
}

static const char *k_role_names[BR_COUNT][3] = {
    { "Hips", "pelvis", "Pelvis" }, { "Spine", "spine_01", NULL }, { "Spine1", "spine_02", NULL }, { "Spine2", "spine_03", NULL },
    { "Neck", "neck_01", NULL }, { "Head", "head", NULL }, { "Head_end", "HeadTop_End", NULL },
    { "LeftShoulder", "clavicle_l", NULL }, { "LeftArm", "upperarm_l", NULL }, { "LeftForeArm", "lowerarm_l", NULL }, { "LeftHand", "hand_l", NULL },
    { "RightShoulder", "clavicle_r", NULL }, { "RightArm", "upperarm_r", NULL }, { "RightForeArm", "lowerarm_r", NULL }, { "RightHand", "hand_r", NULL },
    { "LeftUpLeg", "thigh_l", NULL }, { "LeftLeg", "calf_l", NULL }, { "LeftFoot", "foot_l", NULL }, { "LeftToeBase", "ball_l", NULL },
    { "LeftToeBase_end", "LeftToe_End", NULL },
    { "RightUpLeg", "thigh_r", NULL }, { "RightLeg", "calf_r", NULL }, { "RightFoot", "foot_r", NULL }, { "RightToeBase", "ball_r", NULL },
    { "RightToeBase_end", "RightToe_End", NULL },
    { "LeftHandThumb1", NULL, NULL }, { "LeftHandIndex1", NULL, NULL }, { "LeftHandMiddle1", NULL, NULL }, { "LeftHandRing1", NULL, NULL },
    { "LeftHandPinky1", NULL, NULL },
    { "RightHandThumb1", NULL, NULL }, { "RightHandIndex1", NULL, NULL }, { "RightHandMiddle1", NULL, NULL }, { "RightHandRing1", NULL, NULL },
    { "RightHandPinky1", NULL, NULL },
};

const char *bone_role_name(BoneRole r) { return (r >= 0 && r < BR_COUNT) ? k_role_names[r][0] : "?"; }

bool skel_is_descendant(const Skeleton *s, int bone, int ancestor) {
    for (int b = bone; b >= 0; b = s->parent[b])
        if (b == ancestor) return true;
    return false;
}

static void mirror_name(const char *in, char *out, size_t n) {
    // Swap the first "Left"/"Right" (and "_l"/"_r" suffixes).
    const char *l = strstr(in, "Left"), *r = strstr(in, "Right");
    if (l && (!r || l < r)) snprintf(out, n, "%.*sRight%s", (int)(l - in), in, l + 4);
    else if (r) snprintf(out, n, "%.*sLeft%s", (int)(r - in), in, r + 5);
    else {
        size_t len = strlen(in);
        dg_strcpy(out, n, in);
        if (len > 2 && in[len - 2] == '_' && (in[len - 1] == 'l' || in[len - 1] == 'r') && len < n) out[len - 1] = in[len - 1] == 'l' ? 'r' : 'l';
    }
}

void skel_finalize(Skeleton *s) {
    for (int i = 0; i < s->count; i++) {
        int p = s->parent[i];
        s->bind_model[i] = p >= 0 ? xf_mul(s->bind_model[p], s->bind[i]) : s->bind[i];
    }
    for (int i = 0; i < s->count; i++) {
        s->length[i] = 0;
        for (int j = i + 1; j < s->count; j++)
            if (s->parent[j] == i) { s->length[i] = v3_len(s->bind[j].t); break; }
        if (s->length[i] <= 0) s->length[i] = v3_len(s->bind[i].t);
    }
    for (int r = 0; r < BR_COUNT; r++) {
        s->role[r] = -1;
        for (int k = 0; k < 3 && k_role_names[r][k]; k++) {
            int b = skel_find_suffix(s, k_role_names[r][k]);
            if (b >= 0) { s->role[r] = (i16)b; break; }
        }
    }
    for (int i = 0; i < s->count; i++) {
        char m[ANIM_NAME_MAX];
        mirror_name(s->names[i], m, sizeof(m));
        int j = skel_find(s, m);
        s->mirror[i] = (i16)(j >= 0 ? j : i);
    }
    // Reduced bone set for LOD2+: no fingers and no end bones.
    for (int i = 0; i < s->count; i++) {
        bool finger = false;
        int lh = s->role[BR_L_HAND], rh = s->role[BR_R_HAND];
        for (int b = s->parent[i]; b >= 0; b = s->parent[b])
            if (b == lh || b == rh) { finger = true; break; }
        bool is_end = strstr(s->names[i], "_end") || strstr(s->names[i], "_End");
        s->lod_keep[i] = (u8)(!finger && !is_end);
    }
    int he = s->role[BR_HEAD_END] >= 0 ? s->role[BR_HEAD_END] : s->role[BR_HEAD];
    s->height = he >= 0 ? s->bind_model[he].t.y : 1.8f;
    u32 h = ANIM_HASH_SEED;
    h = anim_hash(&s->count, sizeof(s->count), h);
    for (int i = 0; i < s->count; i++) {
        h = anim_hash_str(s->names[i], h);
        h = anim_hash(&s->parent[i], sizeof(s->parent[i]), h);
        // Quantize the bind (0.1 mm / 1e-4) so tiny float noise in re-exports keeps the id.
        int q[7] = { (int)lrintf(s->bind[i].t.x * 1e4f), (int)lrintf(s->bind[i].t.y * 1e4f), (int)lrintf(s->bind[i].t.z * 1e4f),
                     (int)lrintf(s->bind[i].r.x * 1e4f), (int)lrintf(s->bind[i].r.y * 1e4f), (int)lrintf(s->bind[i].r.z * 1e4f),
                     (int)lrintf(s->bind[i].r.w * 1e4f) };
        h = anim_hash(q, sizeof(q), h);
    }
    s->hash = h;
}

// ------------------------------------------------------------------ clips

void clip_free(Clip *c) {
    free(c->data);
    free(c->root_t);
    free(c->root_yaw);
    c->data = NULL;
    c->root_t = NULL;
    c->root_yaw = NULL;
}

float clip_time_from_ticks(const Clip *c, u32 ticks, float tick_dt, float speed) {
    return (float)ticks * tick_dt * speed * (c && c->speed > 0 ? c->speed : 1.0f);
}

// Frame position for time: looping clips wrap over (frames - 1) intervals (last == first).
static void frame_at(const Clip *c, float time, int *f0, int *f1, float *t) {
    if (c->frames <= 1) { *f0 = *f1 = 0; *t = 0; return; }
    float span = (float)(c->frames - 1);
    float f = time * c->rate;
    if (c->loop) {
        f = fmodf(f, span);
        if (f < 0) f += span;
    } else {
        f = clampf(f, 0.0f, span);
    }
    int a = (int)floorf(f);
    if (a >= c->frames - 1) { a = c->frames - 1; *f0 = *f1 = a; *t = 0; return; }
    *f0 = a;
    *f1 = a + 1;
    *t = f - (float)a;
}

void clip_sample(const Clip *c, float time, Pose *out) {
    int f0, f1;
    float t;
    frame_at(c, time, &f0, &f1, &t);
    out->count = c->bones;
    const Xform *a = c->data + (size_t)f0 * c->bones;
    const Xform *b = c->data + (size_t)f1 * c->bones;
    if (t == 0.0f) {
        memcpy(out->local, a, sizeof(Xform) * (size_t)c->bones);
        return;
    }
    for (int i = 0; i < c->bones; i++) out->local[i] = xf_lerp(a[i], b[i], t);
}

void clip_sample_phase(const Clip *c, float phase, Pose *out) {
    float span = c->frames > 1 ? (float)(c->frames - 1) / c->rate : 0.0f;
    clip_sample(c, phase * span, out);
}

static void root_at(const Clip *c, float time, vec3 *p, float *yaw) {
    int f0, f1;
    float t;
    frame_at(c, time, &f0, &f1, &t);
    *p = v3_lerp(c->root_t[f0], c->root_t[f1], t);
    *yaw = lerpf(c->root_yaw[f0], c->root_yaw[f1], t);
}

void clip_root_delta(const Clip *c, float t0, float t1, vec3 *dpos, float *dyaw) {
    *dpos = v3(0, 0, 0);
    *dyaw = 0;
    if (!c->root_t || c->frames < 2) return;
    float span = (float)(c->frames - 1) / c->rate;
    vec3 p0, p1;
    float y0, y1;
    root_at(c, t0, &p0, &y0);
    root_at(c, t1, &p1, &y1);
    if (c->loop && span > 0) {
        float w0 = floorf(t0 / span), w1 = floorf(t1 / span);
        float wraps = w1 - w0;
        vec3 full = c->root_t[c->frames - 1];
        float full_yaw = c->root_yaw[c->frames - 1];
        *dpos = v3_add(v3_sub(p1, p0), v3_scale(full, wraps));
        *dyaw = (y1 - y0) + full_yaw * wraps;
    } else {
        *dpos = v3_sub(p1, p0);
        *dyaw = y1 - y0;
    }
}

void clip_detect_markers(Clip *c, const Skeleton *s) {
    int feet[2] = { s->role[BR_L_FOOT], s->role[BR_R_FOOT] };
    if (feet[0] < 0 || feet[1] < 0 || c->frames < 4) return;
    Pose p;
    Xform model[ANIM_MAX_BONES];
    float *h[2], *spd[2];
    for (int k = 0; k < 2; k++) {
        h[k] = (float *)malloc(sizeof(float) * (size_t)c->frames);
        spd[k] = (float *)malloc(sizeof(float) * (size_t)c->frames);
    }
    vec3 prev[2] = { { 0 }, { 0 } };
    for (int f = 0; f < c->frames; f++) {
        memcpy(p.local, c->data + (size_t)f * c->bones, sizeof(Xform) * (size_t)c->bones);
        p.count = c->bones;
        pose_to_model(s, &p, model);
        for (int k = 0; k < 2; k++) {
            vec3 w = v3_add(model[feet[k]].t, c->root_t ? c->root_t[f] : v3(0, 0, 0));
            h[k][f] = model[feet[k]].t.y;
            spd[k][f] = f > 0 ? v3_len(v3_sub(w, prev[k])) * c->rate : 1e9f;
            prev[k] = w;
        }
    }
    bool ok = true;
    for (int k = 0; k < 2; k++) {
        float mn = 1e9f, mx = -1e9f;
        for (int f = 0; f < c->frames; f++) { mn = fminf(mn, h[k][f]); mx = fmaxf(mx, h[k][f]); }
        float limit = mn + (mx - mn) * 0.3f;
        int best = -1;
        float best_s = 1e9f;
        for (int f = 1; f < c->frames; f++)
            if (h[k][f] <= limit && spd[k][f] < best_s) { best_s = spd[k][f]; best = f; }
        if (best < 0 || mx - mn < 0.03f) ok = false;   // feet barely move: not locomotion
        c->plant[k] = best >= 0 ? (float)best / (float)(c->frames - 1) : 0.0f;
        free(h[k]);
        free(spd[k]);
    }
    c->has_markers = ok;
}

// ------------------------------------------------------------------ poses

void pose_bind(const Skeleton *s, Pose *out) {
    out->count = s->count;
    memcpy(out->local, s->bind, sizeof(Xform) * (size_t)s->count);
}

void pose_copy(Pose *dst, const Pose *src) {
    dst->count = src->count;
    memcpy(dst->local, src->local, sizeof(Xform) * (size_t)src->count);
}

void pose_blend(const Pose *a, const Pose *b, float t, Pose *out) {
    int n = a->count;
    out->count = n;
    if (t <= 0.0f) { if (out != a) memcpy(out->local, a->local, sizeof(Xform) * (size_t)n); return; }
    if (t >= 1.0f) { if (out != b) memcpy(out->local, b->local, sizeof(Xform) * (size_t)n); return; }
    for (int i = 0; i < n; i++) {
        if (!memcmp(&a->local[i], &b->local[i], sizeof(Xform))) { out->local[i] = a->local[i]; continue; }
        out->local[i] = xf_lerp(a->local[i], b->local[i], t);
    }
}

void pose_blend_masked(const Pose *a, const Pose *b, const float *weights, float t, Pose *out) {
    int n = a->count;
    out->count = n;
    for (int i = 0; i < n; i++) {
        float w = (weights ? weights[i] : 1.0f) * t;
        if (w <= 0.0f) out->local[i] = a->local[i];
        else if (w >= 1.0f) out->local[i] = b->local[i];
        else out->local[i] = xf_lerp(a->local[i], b->local[i], w);
    }
}

void pose_additive(const Pose *base, const Pose *add, const Pose *ref, float w, const float *mask, Pose *out) {
    int n = base->count;
    out->count = n;
    if (w == 0.0f) { if (out != base) memcpy(out->local, base->local, sizeof(Xform) * (size_t)n); return; }
    for (int i = 0; i < n; i++) {
        float bw = w * (mask ? mask[i] : 1.0f);
        Xform b = base->local[i];
        if (bw == 0.0f) { out->local[i] = b; continue; }
        quat d = quat_mul(quat_conj(ref->local[i].r), add->local[i].r);
        quat dw = quat_nlerp(quat_identity(), d, bw);
        out->local[i].r = quat_norm(quat_mul(b.r, dw));
        out->local[i].t = v3_madd(b.t, v3_sub(add->local[i].t, ref->local[i].t), bw);
        out->local[i].s = b.s;
    }
}

void pose_mirror(const Skeleton *s, const Pose *in, Pose *out) {
    Xform model[ANIM_MAX_BONES], mirrored[ANIM_MAX_BONES];
    pose_to_model(s, in, model);
    for (int i = 0; i < s->count; i++) {
        int j = s->mirror[i];
        // Reflect bone j's transform across the YZ plane, then correct for the bind frame difference.
        Xform m = model[j];
        Xform r;
        r.t = v3(-m.t.x, m.t.y, m.t.z);
        quat q = { m.r.x, -m.r.y, -m.r.z, m.r.w };
        Xform bj = s->bind_model[j];
        quat bq = { bj.r.x, -bj.r.y, -bj.r.z, bj.r.w };
        // correction C = inv(reflect(bind_j)) * bind_i
        quat corr = quat_mul(quat_conj(bq), s->bind_model[i].r);
        r.r = quat_norm(quat_mul(q, corr));
        r.s = m.s;
        mirrored[i] = r;
    }
    out->count = s->count;
    for (int i = 0; i < s->count; i++) {
        int p = s->parent[i];
        out->local[i] = p >= 0 ? xf_mul(xf_inverse(mirrored[p]), mirrored[i]) : mirrored[i];
    }
}

void pose_to_model(const Skeleton *s, const Pose *p, Xform *model) {
    for (int i = 0; i < s->count; i++) {
        int par = s->parent[i];
        model[i] = par >= 0 ? xf_mul(model[par], p->local[i]) : p->local[i];
    }
}

void pose_set_model(const Skeleton *s, Pose *p, Xform *model, int bone, Xform desired) {
    int par = s->parent[bone];
    p->local[bone] = par >= 0 ? xf_mul(xf_inverse(model[par]), desired) : desired;
    p->local[bone].s = v3(1, 1, 1);
    model[bone] = par >= 0 ? xf_mul(model[par], p->local[bone]) : p->local[bone];
}

void pose_update_children(const Skeleton *s, const Pose *p, Xform *model, int bone) {
    u8 dirty[ANIM_MAX_BONES];
    memset(dirty, 0, sizeof(dirty));
    dirty[bone] = 1;
    for (int j = bone + 1; j < s->count; j++) {
        int par = s->parent[j];
        if (par >= 0 && dirty[par]) {
            model[j] = xf_mul(model[par], p->local[j]);
            dirty[j] = 1;
        }
    }
}

void mask_fill(BoneMask *m, float v) { for (int i = 0; i < ANIM_MAX_BONES; i++) m->w[i] = v; }

void mask_set_chain(const Skeleton *s, BoneMask *m, int bone, float v) {
    if (bone < 0) return;
    for (int i = 0; i < s->count; i++)
        if (skel_is_descendant(s, i, bone)) m->w[i] = v;
}

// ------------------------------------------------------------------ IK

bool anim_ik_two_bone(vec3 root, vec3 target, vec3 pole, float a, float b, vec3 *mid, vec3 *end) {
    vec3 d = v3_sub(target, root);
    float dist = v3_len(d);
    vec3 dir = dist > 1e-5f ? v3_scale(d, 1.0f / dist) : v3(0, 0, -1);
    float max_reach = a + b - 1e-4f;
    float min_reach = fabsf(a - b) + 1e-4f;
    bool reached = dist <= max_reach;
    dist = clampf(dist, min_reach, max_reach);
    float cos_a = clampf((a * a + dist * dist - b * b) / (2.0f * a * dist), -1.0f, 1.0f);
    float sin_a = sqrtf(fmaxf(0.0f, 1.0f - cos_a * cos_a));
    vec3 p = v3_sub(pole, v3_scale(dir, v3_dot(pole, dir)));
    if (v3_len2(p) < 1e-8f) p = v3_cross(dir, v3(1, 0, 0));
    if (v3_len2(p) < 1e-8f) p = v3_cross(dir, v3(0, 0, 1));
    p = v3_norm(p);
    *mid = v3_add(root, v3_add(v3_scale(dir, a * cos_a), v3_scale(p, a * sin_a)));
    *end = v3_madd(root, dir, dist);
    return reached;
}

float g_anim_forearm_twist = 0.6f;
bool g_anim_ik_shoulders = false;

// The twist of q about the unit axis n (swing-twist decomposition), as an angle (radians).
static float twist_angle(quat q, vec3 n) {
    float proj = q.x * n.x + q.y * n.y + q.z * n.z;
    return 2.0f * atan2f(proj, q.w);
}

static bool two_bone_pose(const Skeleton *s, Pose *p, Xform *model, int ia, int ib, int ic, vec3 target_pos,
                          const quat *target_rot, vec3 pole_dir, float weight, float forearm_twist) {
    if (ia < 0 || ib < 0 || ic < 0 || weight <= 0.0f) return true;
    Xform orig_a = p->local[ia], orig_b = p->local[ib], orig_c = p->local[ic];
    vec3 pa = model[ia].t, pb = model[ib].t, pc = model[ic].t;
    float la = v3_dist(pa, pb), lb = v3_dist(pb, pc);
    vec3 mid, end;
    bool reached = anim_ik_two_bone(pa, target_pos, pole_dir, la, lb, &mid, &end);
    // Upper bone: swing so the joint lands on mid.
    vec3 d0 = v3_norm(v3_sub(pb, pa)), d1 = v3_norm(v3_sub(mid, pa));
    Xform na = model[ia];
    na.r = quat_norm(quat_mul(quat_from_to(d0, d1), na.r));
    pose_set_model(s, p, model, ia, na);
    model[ib] = xf_mul(model[ia], p->local[ib]);
    model[ic] = xf_mul(model[ib], p->local[ic]);
    // Lower bone: swing so the end lands on target.
    vec3 e0 = v3_norm(v3_sub(model[ic].t, model[ib].t)), e1 = v3_norm(v3_sub(end, model[ib].t));
    Xform nb = model[ib];
    nb.r = quat_norm(quat_mul(quat_from_to(e0, e1), nb.r));
    pose_set_model(s, p, model, ib, nb);
    model[ic] = xf_mul(model[ib], p->local[ic]);
    if (target_rot && forearm_twist > 0.0f) {
        // Roll the lower bone about its own axis (the end joint stays put) by a share of the end's
        // twist relative to it: the twist spreads along the forearm instead of all at the wrist.
        vec3 ax = v3_sub(model[ic].t, model[ib].t);
        if (v3_len2(ax) > 1e-10f) {
            ax = v3_norm(ax);
            // World-space turn still needed at the end (current -> wanted); its twist about the axis.
            float tw = wrap_angle(twist_angle(quat_mul(target_rot[0], quat_conj(model[ic].r)), ax));
            Xform nb2 = model[ib];
            nb2.r = quat_norm(quat_mul(quat_axis_angle(ax, tw * clampf(forearm_twist, 0.0f, 1.0f)), nb2.r));
            pose_set_model(s, p, model, ib, nb2);
            model[ic] = xf_mul(model[ib], p->local[ic]);
        }
    }
    if (target_rot) {
        Xform nc = model[ic];
        nc.r = *target_rot;
        pose_set_model(s, p, model, ic, nc);
    }
    if (weight < 1.0f) {
        p->local[ia] = xf_lerp(orig_a, p->local[ia], weight);
        p->local[ib] = xf_lerp(orig_b, p->local[ib], weight);
        p->local[ic] = xf_lerp(orig_c, p->local[ic], weight);
        model[ia] = s->parent[ia] >= 0 ? xf_mul(model[s->parent[ia]], p->local[ia]) : p->local[ia];
    }
    pose_update_children(s, p, model, ia);
    return reached;
}

bool anim_ik_two_bone_pose(const Skeleton *s, Pose *p, Xform *model, int ia, int ib, int ic, vec3 target_pos,
                           const quat *target_rot, vec3 pole_dir, float weight) {
    return two_bone_pose(s, p, model, ia, ib, ic, target_pos, target_rot, pole_dir, weight, 0.0f);
}

bool anim_ik_arm_pose(const Skeleton *s, Pose *p, Xform *model, int ia, int ib, int ic, vec3 target_pos,
                      const quat *target_rot, vec3 pole_dir, float weight) {
    return two_bone_pose(s, p, model, ia, ib, ic, target_pos, target_rot, pole_dir, weight, g_anim_forearm_twist);
}

void anim_aim_bone(const Skeleton *s, Pose *p, Xform *model, int bone, vec3 local_axis, vec3 target_dir, float weight) {
    if (bone < 0 || weight <= 0.0f) return;
    vec3 cur = v3_norm(quat_rotate(model[bone].r, local_axis));
    quat d = quat_from_to(cur, v3_norm(target_dir));
    if (weight < 1.0f) d = quat_nlerp(quat_identity(), d, weight);
    Xform n = model[bone];
    n.r = quat_norm(quat_mul(d, n.r));
    pose_set_model(s, p, model, bone, n);
    pose_update_children(s, p, model, bone);
}
