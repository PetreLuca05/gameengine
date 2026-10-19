// First-person arms layer: the arms camera and its rigid placement (docs/CAMERA_HOLDPOINT.md
// "Rule 3"). Pure (no engine state): unit tested in tests/test_arms_layer.c.
#include "arms_layer.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ArmsPose pose_identity(void) {
    ArmsPose p;
    memset(&p, 0, sizeof(p));
    p.scale = 1.0f;
    return p;
}

void arms_layer_defaults(ArmsLayerDef *d) {
    memset(d, 0, sizeof(*d));
    for (int s = 0; s < HP_STATE_COUNT; s++) {
        d->state[s] = pose_identity();
        d->blend[s] = 0.2f;
    }
    d->equip_time = 0.3f;
    d->equip_pitch = 35.0f;
    d->equip_margin = 0.05f;
    d->seam = false;
    d->seam_r0 = 0.06f;
    d->seam_r1 = 0.16f;
    d->znear = 0.01f;
    d->weapon_fov[WEAPON_FOV_NARROW] = 60.0f;
    d->weapon_fov[WEAPON_FOV_DEFAULT] = 70.0f;
    d->weapon_fov[WEAPON_FOV_WIDE] = 85.0f;
    d->screen_scale = 1.125f;
    d->screen_anchor = v2(0.75f, 1.0f);
}

static const char *const k_weapon_fov[WEAPON_FOV_COUNT] = { "narrow", "default", "wide" };

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static vec3 jv3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = jget(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}

static ArmsPose parse_pose(const cJSON *o, ArmsPose p) {
    p.offset = jv3(o, "offset", p.offset);
    p.rotation = jv3(o, "rotation", p.rotation);
    p.scale = clampf(jf(o, "scale", p.scale), 0.9f, 1.1f);
    p.fov_add = clampf(jf(o, "fov_add", p.fov_add), -20.0f, 20.0f);
    const cJSON *c = jget(o, "camera");
    p.cam_pos = jv3(c, "position", p.cam_pos);
    return p;
}

void arms_layer_parse(ArmsLayerDef *d, const cJSON *root) {
    arms_layer_defaults(d);
    if (!root) return;
    float blend = jf(root, "blend", 0.2f);
    const cJSON *st = jget(root, "states");
    for (int s = 0; s < HP_STATE_COUNT; s++) {
        const cJSON *e = jget(st, holdpoint_state_name(s));
        d->state[s] = parse_pose(e, d->state[s]);
        d->blend[s] = fmaxf(0.0f, jf(e, "blend", blend));
    }
    const cJSON *items = jget(root, "items"), *it;
    cJSON_ArrayForEach(it, items) {
        if (d->item_count >= ARMS_MAX_ITEMS || !it->string) break;
        ArmsItem *a = &d->items[d->item_count++];
        memset(a, 0, sizeof(*a));
        snprintf(a->id, sizeof(a->id), "%s", it->string);
        for (int s = 0; s < HP_STATE_COUNT; s++) {
            const cJSON *e = jget(it, holdpoint_state_name(s));
            if (!e) continue;
            a->set[s] = true;
            a->state[s] = parse_pose(e, d->state[s]);
        }
    }
    const cJSON *eq = jget(root, "equip");
    d->equip_time = fmaxf(0.0f, jf(eq, "time", d->equip_time));
    d->equip_pitch = jf(eq, "pitch", d->equip_pitch);
    d->equip_margin = fmaxf(0.0f, jf(eq, "margin", d->equip_margin));
    const cJSON *seam = jget(root, "seam");
    if (cJSON_IsBool(jget(seam, "on"))) d->seam = cJSON_IsTrue(jget(seam, "on"));
    d->seam_r0 = fmaxf(0.0f, jf(seam, "r0", d->seam_r0));
    d->seam_r1 = fmaxf(d->seam_r0 + 0.01f, jf(seam, "r1", d->seam_r1));
    d->znear = clampf(jf(root, "znear", d->znear), 0.002f, 0.05f);
    const cJSON *wf = jget(root, "weapon_fov");
    for (int i = 0; i < WEAPON_FOV_COUNT; i++) d->weapon_fov[i] = clampf(jf(wf, k_weapon_fov[i], d->weapon_fov[i]), 30.0f, 130.0f);
    const cJSON *sc = jget(root, "screen");
    d->screen_scale = clampf(jf(sc, "scale", d->screen_scale), 0.25f, 2.0f);
    const cJSON *an = jget(sc, "anchor");
    if (cJSON_GetArraySize(an) == 2)
        d->screen_anchor = v2(clampf((float)cJSON_GetArrayItem(an, 0)->valuedouble, 0.0f, 1.0f), clampf((float)cJSON_GetArrayItem(an, 1)->valuedouble, 0.0f, 1.0f));
}

static float r4(float v) { return roundf(v * 10000.0f) / 10000.0f; }
static void put_v3(cJSON *o, const char *k, vec3 v) {
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.x)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.y)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.z)));
    cJSON_AddItemToObject(o, k, a);
}
static void put_pose(cJSON *o, ArmsPose p) {
    put_v3(o, "offset", p.offset);
    put_v3(o, "rotation", p.rotation);
    cJSON_AddNumberToObject(o, "scale", r4(p.scale));
    cJSON_AddNumberToObject(o, "fov_add", r4(p.fov_add));
    cJSON *c = cJSON_AddObjectToObject(o, "camera");
    put_v3(c, "position", p.cam_pos);
}

char *arms_layer_to_json(const ArmsLayerDef *d) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "_comment",
                            "First-person arms layer (docs/CAMERA_HOLDPOINT.md \"Rule 3\"): the arms + held item drawn with their own camera. Per hold point state "
                            "(and per item under \"items\"): offset (right, up, forward m), rotation (pitch, yaw, roll deg) and scale (0.9 .. 1.1) about the hold "
                            "point, fov_add (deg on the Weapon Field of View); in ADS only offset[2] (push), rotation[2] (roll) and scale (about the sight) apply. "
                            "camera: the arms camera's own position against the eye (right, up, forward m; it moves, never turns), the "
                            "hip framing (docs/CAMERA_HOLDPOINT.md \"Framing\"); aimed it is back on the eye. equip: the raise from below the view. seam (on: "
                            "arm vertices near the shoulder blend from the world camera (r0) to the arms camera (r1); off: the arms camera draws only the "
                            "arms + item). "
                            "weapon_fov: the Weapon Field of View presets (hip, horizontal deg at 16:9; the layer moves sideways with them so the sight keeps "
                            "its place on screen). screen: the arms + item picture's size on screen at the hip (scale 0.25 .. 2; 1 aimed) about anchor (screen "
                            "fractions: x from the left, y from the top, 1 = the bottom edge): bigger or smaller, the same place. Edit live with arms_tuner.");
    cJSON *st = cJSON_AddObjectToObject(o, "states");
    for (int s = 0; s < HP_STATE_COUNT; s++) {
        cJSON *e = cJSON_AddObjectToObject(st, holdpoint_state_name(s));
        put_pose(e, d->state[s]);
        cJSON_AddNumberToObject(e, "blend", r4(d->blend[s]));
    }
    cJSON *items = cJSON_AddObjectToObject(o, "items");
    for (int i = 0; i < d->item_count; i++) {
        cJSON *it = cJSON_AddObjectToObject(items, d->items[i].id);
        for (int s = 0; s < HP_STATE_COUNT; s++)
            if (d->items[i].set[s]) put_pose(cJSON_AddObjectToObject(it, holdpoint_state_name(s)), d->items[i].state[s]);
    }
    cJSON *eq = cJSON_AddObjectToObject(o, "equip");
    cJSON_AddNumberToObject(eq, "time", r4(d->equip_time));
    cJSON_AddNumberToObject(eq, "pitch", r4(d->equip_pitch));
    cJSON_AddNumberToObject(eq, "margin", r4(d->equip_margin));
    cJSON *seam = cJSON_AddObjectToObject(o, "seam");
    cJSON_AddBoolToObject(seam, "on", d->seam);
    cJSON_AddNumberToObject(seam, "r0", r4(d->seam_r0));
    cJSON_AddNumberToObject(seam, "r1", r4(d->seam_r1));
    cJSON_AddNumberToObject(o, "znear", r4(d->znear));
    cJSON *wf = cJSON_AddObjectToObject(o, "weapon_fov");
    for (int i = 0; i < WEAPON_FOV_COUNT; i++) cJSON_AddNumberToObject(wf, k_weapon_fov[i], r4(d->weapon_fov[i]));
    cJSON *sc = cJSON_AddObjectToObject(o, "screen");
    cJSON_AddNumberToObject(sc, "scale", r4(d->screen_scale));
    cJSON *an = cJSON_CreateArray();
    cJSON_AddItemToArray(an, cJSON_CreateNumber(r4(d->screen_anchor.x)));
    cJSON_AddItemToArray(an, cJSON_CreateNumber(r4(d->screen_anchor.y)));
    cJSON_AddItemToObject(sc, "anchor", an);
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    return s;
}

static const ArmsItem *find_item(const ArmsLayerDef *d, const char *id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < d->item_count; i++)
        if (!strcmp(d->items[i].id, id)) return &d->items[i];
    return NULL;
}

ArmsPose arms_layer_target(const ArmsLayerDef *d, int state, const char *item_id) {
    state = DG_CLAMP(state, 0, HP_STATE_COUNT - 1);
    const ArmsItem *it = find_item(d, item_id);
    return it && it->set[state] ? it->state[state] : d->state[state];
}

ArmsPose *arms_layer_edit(ArmsLayerDef *d, int state, const char *item_id) {
    state = DG_CLAMP(state, 0, HP_STATE_COUNT - 1);
    if (!item_id || !item_id[0]) return &d->state[state];
    ArmsItem *it = (ArmsItem *)find_item(d, item_id);
    if (!it) {
        if (d->item_count >= ARMS_MAX_ITEMS) return &d->state[state];
        it = &d->items[d->item_count++];
        memset(it, 0, sizeof(*it));
        snprintf(it->id, sizeof(it->id), "%s", item_id);
    }
    if (!it->set[state]) {
        it->set[state] = true;
        it->state[state] = d->state[state];
    }
    return &it->state[state];
}

void arms_layer_reset(ArmsLayerState *st) { memset(st, 0, sizeof(*st)); }

static ArmsPose pose_lerp(ArmsPose a, ArmsPose b, float t) {
    ArmsPose r;
    r.offset = v3_lerp(a.offset, b.offset, t);
    r.rotation = v3_lerp(a.rotation, b.rotation, t);
    r.scale = lerpf(a.scale, b.scale, t);
    r.fov_add = lerpf(a.fov_add, b.fov_add, t);
    r.cam_pos = v3_lerp(a.cam_pos, b.cam_pos, t);
    return r;
}

static u32 str_hash(const char *s) {
    u32 h = 2166136261u;
    while (s && *s) { h ^= (u8)*s++; h *= 16777619u; }
    return h;
}

// Rotation r and uniform scale s about the point p, then translation t: x -> t + p + s R (x - p).
static mat4 about(vec3 p, quat r, float s, vec3 t) {
    mat4 m = mat4_mul(mat4_translate(v3_add(t, p)), mat4_mul(mat4_trs(v3(0, 0, 0), r, v3(s, s, s)), mat4_translate(v3_neg(p))));
    return m;
}

// NDC' = s NDC + (1 - s) anchor: x' = s x + (1 - s) ax w (the anchor's NDC, Vulkan: y down).
mat4 arms_layer_screen_clip(float s, vec2 anchor) {
    mat4 m = mat4_identity();
    m.m[0] = m.m[5] = s;
    m.m[12] = (1.0f - s) * (2.0f * anchor.x - 1.0f);
    m.m[13] = (1.0f - s) * (2.0f * anchor.y - 1.0f);
    return m;
}

// The same pixel through the world projection at the same depth: x' = k s x - (1 - s) ax tw aspect z,
// y' = k s y + (1 - s) ay tw z (view space looks down -z; NDC y down = view y up).
mat4 arms_layer_screen_view(float s, vec2 anchor, float k, float tw, float aspect) {
    mat4 m = mat4_identity();
    m.m[0] = m.m[5] = k * s;
    m.m[8] = -(1.0f - s) * (2.0f * anchor.x - 1.0f) * tw * aspect;
    m.m[9] = (1.0f - s) * (2.0f * anchor.y - 1.0f) * tw;
    return m;
}

quat arms_layer_aim_rot(vec2 aim_deg, float view_pitch, float fov_world, float fov_hands) {
    // The shot is dir(yaw - a, pitch + b) = R(yaw, pitch) * q * forward with
    // q = X(-pitch) * Y(-a) * X(pitch + b) in camera space (exact, not small-angle).
    float a = aim_deg.x * DG_DEG2RAD, b = aim_deg.y * DG_DEG2RAD;
    if (fabsf(a) < 1e-7f && fabsf(b) < 1e-7f) return quat_identity();
    quat q = quat_mul(quat_axis_angle(v3(1, 0, 0), -view_pitch), quat_mul(quat_axis_angle(v3(0, 1, 0), -a), quat_axis_angle(v3(1, 0, 0), view_pitch + b)));
    vec3 d = quat_rotate(quat_norm(q), v3(0, 0, -1));
    // The arms layer is drawn with its own FOV: aim at the arms-space direction that lands on the
    // same pixel as the shot does with the world FOV (x / y scaled by the tan ratio).
    float s = (fov_world > 1.0f && fov_hands > 1.0f) ? tanf(fov_hands * 0.5f * DG_DEG2RAD) / tanf(fov_world * 0.5f * DG_DEG2RAD) : 1.0f;
    vec3 dv = v3_norm(v3(d.x * s, d.y * s, d.z));
    return quat_norm(quat_from_to(v3(0, 0, -1), dv));
}

static float smoother(float x) {
    x = saturate(x);
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

void arms_layer_eval(const ArmsLayerDef *d, ArmsLayerState *st, const ArmsLayerIn *in, ArmsLayerOut *out) {
    int state = DG_CLAMP(in->hp_state, 0, HP_STATE_COUNT - 1);
    u32 ih = str_hash(in->item_id);
    ArmsPose target = arms_layer_target(d, state, in->item_id);
    // Aimed the camera is the hip's, faded by the item's own ADS progress below (a second, state blend
    // of it would lag the item and swing it past its place on screen).
    if (state == HP_ADS) target.cam_pos = arms_layer_target(d, HP_HIP, in->item_id).cam_pos;
    // State / item changes ease from wherever the layer is (smootherstep over the state's blend).
    if (!st->init) {
        st->init = true;
        st->state = state;
        st->item_hash = ih;
        st->cur = target;
        st->t = st->dur = 0;
    } else if (state != st->state || ih != st->item_hash) {
        st->from = st->cur;
        st->state = state;
        st->item_hash = ih;
        st->t = 0;
        st->dur = d->blend[state];
    }
    st->t += fmaxf(in->dt, 0.0f);
    st->cur = st->dur > 1e-4f && st->t < st->dur ? pose_lerp(st->from, target, smoother(st->t / st->dur)) : target;

    float ads = saturate(in->ads_w);
    ArmsPose p = st->cur;
    // ADS: the sight stays on the view axis - no lateral offset, no pitch / yaw; push, roll and scale
    // act about the sight (the pivot slides there with the ADS progress).
    p.offset.x *= 1.0f - ads;
    p.offset.y *= 1.0f - ads;
    p.rotation.x *= 1.0f - ads;
    p.rotation.y *= 1.0f - ads;
    p.fov_add *= 1.0f - ads;

    // Present: the drawn arms + item rigidly onto the presented item, at the hip as aimed (the sight on
    // the view axis): the presented item is camera-locked (no soft pitch limit), so the item and the
    // hands keep their place on screen at any view pitch whatever the body's arms reach.
    Xform pr = in->present;
    pr.s = v3(1, 1, 1);
    mat4 P = mat4_trs(pr.t, pr.r, v3(1, 1, 1));
    Xform item = xf_mul(pr, in->item_cam);
    vec3 pivot = in->has_item ? v3_lerp(xf_point(item, in->pivot), xf_point(item, in->sight), ads) : v3(0.12f, -0.18f, -0.35f);
    // Tuned placement about the hold point.
    mat4 T = about(pivot, hp_euler_deg(p.rotation), p.scale, hp_cam_pos(p.offset));
    vec3 sight_t = in->has_item ? mat4_mul_point(T, xf_point(item, in->sight)) : v3(0, 0, -0.3f);
    pivot = mat4_mul_point(T, pivot);
    // The arms camera's own position (the hip framing; it never turns): pose-eye space -> arms camera
    // space. Aimed it is back on the eye, so the sight lands on the screen centre. Its weight follows
    // the item itself: how far the presented sight has come from its aimed place to its hip place
    // (3D: item and camera then move along one line in camera space, a straight path on screen; a
    // separate ease would let the item arrive first and swing past its place). Until the item has been
    // aimed once, the sight's distance from the view axis against its hip place.
    float cw = 1.0f - ads;
    if (in->has_item) {
        vec3 sc = xf_point(item, in->sight);
        if (st->sight_item != ih) {
            st->hip_sight_ok = st->ads_sight_ok = false;
            st->sight_item = ih;
        }
        if (ads <= 0.0f) {
            st->hip_sight = sc;
            st->hip_sight_ok = true;
        } else if (ads >= 1.0f) {
            st->ads_sight = sc;
            st->ads_sight_ok = true;
            cw = 0.0f;
        } else if (st->hip_sight_ok) {
            vec3 a = st->ads_sight_ok ? st->ads_sight : v3(0, 0, sc.z), d = v3_sub(st->hip_sight, a);
            if (!st->ads_sight_ok) d.z = 0.0f;
            float d2 = v3_dot(d, d);
            if (d2 > 1e-8f) cw = clampf(v3_dot(v3_sub(sc, a), d) / d2, 0.0f, 1.0f);
        }
    }
    mat4 C = mat4_translate(v3_scale(hp_cam_pos(p.cam_pos), -cw));
    out->cam_w = cw;
    // The screen size follows the same weight: the picture is its own size aimed (the sight centred).
    out->screen_scale = 1.0f + (clampf(d->screen_scale, 0.25f, 2.0f) - 1.0f) * cw;
    out->sight = in->has_item ? xf_point(item, in->sight) : v3(0, 0, 0);
    // Cosmetic sway (Weapon sway < 1): the pose's sway shown smaller - about the pivot at the hip, about
    // the sight aimed (the hold point turns it there).
    mat4 Sw = mat4_identity();
    {
        float k = 1.0f - clampf(in->sway_scale, 0.0f, 1.0f);
        if (k > 0.0f) {
            quat rh = quat_conj(hp_euler_deg(v3_scale(in->sway_hip_rot, k * (1.0f - ads))));
            quat ra = quat_conj(hp_euler_deg(v3_scale(in->sway_ads_rot, k * ads)));
            Sw = about(pivot, quat_norm(quat_mul(ra, rh)), 1.0f, v3_scale(in->sway_hip_pos, -k * (1.0f - ads)));
        }
    }
    // Cosmetic kick: the pose's kick shown smaller (Weapon kick < 1), plus the screen kick towards the eye.
    // Aimed, only the push: the hold point turns the gun about the sight there (and clamps irons), so
    // countering the raw rotation would move the reticle.
    mat4 K = mat4_identity();
    {
        float k = 1.0f - clampf(in->kick_scale, 0.0f, 1.0f);
        vec3 kp = in->kick_pos;
        kp.x *= 1.0f - ads;
        kp.y *= 1.0f - ads;
        vec3 kd = v3_scale(v3(in->kick_rot.x, -in->kick_rot.y, in->kick_rot.z), 1.0f - ads);
        quat r = quat_conj(hp_euler_deg(v3_scale(kd, k)));
        vec3 t = v3_add(v3_scale(kp, -k), v3(0, 0, in->screen_kick * clampf(in->kick_scale, 0.0f, 1.0f)));
        K = about(pivot, r, 1.0f, t);
    }
    // Equip: a newly selected item comes up from below the arms frustum (muzzle down) to its hold.
    mat4 E = mat4_identity();
    float raise = 0.0f;
    if (in->equip_t >= 0.0f && d->equip_time > 1e-3f && in->equip_t < d->equip_time) {
        raise = 1.0f - smoother(in->equip_t / d->equip_time);
        vec3 pc = mat4_mul_point(C, pivot);   // seen from the arms camera
        float depth = fmaxf(-pc.z, 0.05f);
        float th = tanf(fmaxf(in->fov_hands, 10.0f) * 0.5f * DG_DEG2RAD);
        float reach = fmaxf(in->length, 0.35f) + d->equip_margin;   // the item and the forearms around the hold point
        float drop = pc.y + depth * th + reach;
        E = about(pivot, quat_axis_angle(v3(1, 0, 0), -d->equip_pitch * DG_DEG2RAD * raise), 1.0f, v3(0, -drop * raise, 0));
    }
    // Aim: where the world camera shows the shot (sway, the ADS counter of the camera kick, the FOVs).
    quat a = arms_layer_aim_rot(in->aim_deg, in->aim_pitch, in->fov_world, in->fov_hands);
    mat4 A = mat4_from_quat(a);
    // Weapon Field of View: sideways at the same depth (arms camera space) by the preset's tangent ratio,
    // so the sight keeps its pixel at the hip (aimed it is on the view axis already).
    mat4 L = mat4_identity();
    if (in->has_item && in->lateral_scale > 0.0f && fabsf(in->lateral_scale - 1.0f) > 1e-6f) {
        vec3 s = mat4_mul_point(C, sight_t);
        float k = (in->lateral_scale - 1.0f) * cw;
        L = mat4_translate(v3(s.x * k, s.y * k, 0.0f));
    }
    out->layer = mat4_mul(L, mat4_mul(C, mat4_mul(A, mat4_mul(E, mat4_mul(K, mat4_mul(Sw, mat4_mul(T, P)))))));
    Xform eye = in->pose_eye;
    eye.s = v3(1, 1, 1);
    mat4 inv_eye = mat4_inverse_rigid(mat4_trs(eye.t, eye.r, v3(1, 1, 1)));
    out->view = mat4_mul(out->layer, inv_eye);
    out->fov_add = p.fov_add;
    out->raise = raise;
}
