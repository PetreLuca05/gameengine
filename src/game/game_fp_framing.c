// First-person framing against a reference (docs/CAMERA_HOLDPOINT.md "Framing"): where the held gun's
// front / rear sight, bore, muzzle and hands land on screen through the arms layer, the reference's
// targets (assets/player/fp_framing.json), a fit of the arms camera's hip position to them (the
// camera moves, never turns: the arms, the item and the body stay where they are), the arms FOV sweep, and the
// reference overlay the arms tuner shows.
//   fp_framing [report]                 this frame's points against the targets (PASS / FAIL)
//   fp_framing_fit [fov]                fit the held item's hip arms camera to its targets (the rifle: the
//                                       full set; other classes: the hand + sight height, near the rifle's)
//   fp_framing_sweep <lo> <hi> <step>   the rifle fit at each arms FOV (horizontal at 16:9): the residual
//   fp_framing_overlay [0|1] [alpha]    the reference image + targets + measured points over the frame
//   fp_framing_track <frames>           log the sight / hand screen points every frame (hip <-> ADS checks)
//   fp_framing_capture <name>           screenshot + measurements into docs/fp_framing/ (tools/fp_framing_compare.py)
#include "game_internal.h"
#include "render/render.h"
#include "render/arms_layer.h"
#include "assets/item_mesh.h"
#include "gameplay/characters.h"
#include "player/hands.h"
#include "player/fov.h"
#include "ui/ui.h"
#include "ui/gui/gui.h"
#include "ui/gui/gui_draw.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/platform.h"
#include "cJSON.h"
#include "stb_image.h"
#include "nuklear_config.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define FF_JSON "assets/player/fp_framing.json"
#define FF_MAX_CLASSES 12

typedef struct FfClass {
    char id[32];
    int hand;                    // the hand that sets the class: 0 left, 1 right (the pistol)
    vec2 hand_at;                // where (screen fractions from the top left)
} FfClass;

typedef struct FfTargets {
    char image[160];             // the reference screenshot
    float pitch;                 // its camera pitch (deg; targets are screen-relative)
    char item[32];               // the item the full target set is for (the rifle)
    vec2 front, rear, left_hand; // screen fractions from the top left
    float exit_x;                // the bore axis at the bottom edge
    float angle;                 // the sight line (rear -> front), deg above horizontal
    float cant;                  // the gun's roll on screen (deg, + = clockwise; fitted loosely, reported)
    float tol, hand_tol, angle_tol, class_tol;   // points, the rifle's left hand, angle (deg), the classes' hand
    FfClass cls[FF_MAX_CLASSES];
    int ncls;
} FfTargets;

typedef struct FfMeasure {
    bool valid, sights;          // sights: the mesh's front / rear sight parts (else muzzle / ADS sight)
    vec2 front, rear, muzzle, sight, hand[2];
    float exit_x;                // NAN: the bore never reaches the bottom edge
    float angle, cant;           // deg; cant + = clockwise
    float length;                // 1 - front.y: the visible length below the front sight
    float min_depth;             // the nearest corner of the parts wholly in front of the arms camera (m)
    int cut;                     // triangles the near plane cuts on screen (near_cut)
    char cut_part[32];           // the first part with one
    float depth_ratio;           // front sight depth / rear sight depth (the perspective)
    bool muzzle_in;              // the muzzle on screen
    float fov_v;
} FfMeasure;

// What the measurement needs from the last frame.
typedef struct FfScene {
    ArmsLayerIn in;
    ArmsLayerOut out;
    CharsFpArms fa;
    const WeaponDef *wd;
    vec3 pos;
    quat rot;
    float k;
    ItemMesh *mesh;
    int front_part, rear_part;
    float aspect;
} FfScene;

static struct {
    FfTargets t;
    bool loaded;
    bool overlay, edit;
    float alpha;
    TextureId tex;
    int tex_w, tex_h;
    char tex_path[160];
    int drag;                    // the target marker being dragged + 1
    int track;                   // frames left to log
    FfMeasure live;
    char status[200];
} FF = { .alpha = 0.5f };

static FfTargets defaults(void) {
    FfTargets t;
    memset(&t, 0, sizeof(t));
    dg_strcpy(t.image, sizeof(t.image), "docs/fp_framing/reference_mwii_80fov.jpg");
    dg_strcpy(t.item, sizeof(t.item), "rifle");
    t.pitch = 15.0f;
    t.front = v2(0.563f, 0.566f);
    t.rear = v2(0.688f, 0.705f);
    t.left_hand = v2(0.525f, 0.79f);
    t.exit_x = 0.80f;
    t.angle = 32.1f;
    t.cant = 0.0f;
    t.tol = 0.02f;
    t.hand_tol = 0.07f;
    t.angle_tol = 3.0f;
    t.class_tol = 0.03f;
    return t;
}

static vec2 jv2(const cJSON *o, const char *k, vec2 d) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 2) return d;
    return v2((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble);
}
static float jnum(const cJSON *o, const char *k, float d) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : d;
}

static void load(void) {
    FF.t = defaults();
    FF.loaded = true;
    char path[600];
    size_t sz = 0;
    char *text = platform_read_file(platform_data_path(FF_JSON, path, sizeof(path)), &sz);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root) { log_warn("fp_framing: %s missing or bad (defaults)", FF_JSON); return; }
    FfTargets *t = &FF.t;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "image");
    if (cJSON_IsString(v)) dg_strcpy(t->image, sizeof(t->image), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(root, "item");
    if (cJSON_IsString(v)) dg_strcpy(t->item, sizeof(t->item), v->valuestring);
    t->pitch = jnum(root, "pitch", t->pitch);
    const cJSON *tg = cJSON_GetObjectItemCaseSensitive(root, "targets");
    t->front = jv2(tg, "front_sight", t->front);
    t->rear = jv2(tg, "rear_sight", t->rear);
    t->left_hand = jv2(tg, "left_hand", t->left_hand);
    t->exit_x = jnum(tg, "exit_x", t->exit_x);
    t->angle = jnum(tg, "angle", t->angle);
    t->cant = jnum(tg, "cant", t->cant);
    const cJSON *tol = cJSON_GetObjectItemCaseSensitive(root, "tolerance");
    t->tol = jnum(tol, "point", t->tol);
    t->hand_tol = jnum(tol, "left_hand", t->hand_tol);
    t->angle_tol = jnum(tol, "angle", t->angle_tol);
    t->class_tol = jnum(tol, "class_hand", t->class_tol);
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "classes")) {
        if (t->ncls >= FF_MAX_CLASSES || !c->string) break;
        FfClass *k = &t->cls[t->ncls++];
        dg_strcpy(k->id, sizeof(k->id), c->string);
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(c, "hand");
        k->hand = cJSON_IsString(h) && !strcmp(h->valuestring, "right") ? 1 : 0;
        k->hand_at = jv2(c, "at", t->left_hand);
    }
    cJSON_Delete(root);
}

static bool save(void) {
    const FfTargets *t = &FF.t;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "_comment",
                            "First-person framing reference (docs/CAMERA_HOLDPOINT.md \"Framing\"): the reference screenshot (MWII, world Field of View 80, Weapon Field of View "
                            "Default, hip) and where its rifle sits on screen, fractions of the width / height from the top left. targets: the front sight post, the rear sight, "
                            "the left hand's centre, exit_x the bore axis at the bottom edge, angle the sight line (deg above horizontal), cant the front post's lean (deg, + = "
                            "clockwise). tolerance.left_hand is wider than the points': our rifle's support grip (an IK target, never moved) is 14 cm behind its front sight, the "
                            "reference gun's hand sits under its front sight, and the arms camera only moves (it never turns). classes: the hand that keeps its screen place for the other weapons (the rifle's fitted left hand; "
                            "the pistol's right hand), the sight height is the rifle's front sight. Edit with the arms tuner's reference overlay (fp_framing_*).");
    cJSON_AddStringToObject(o, "image", t->image);
    cJSON_AddNumberToObject(o, "pitch", t->pitch);
    cJSON_AddStringToObject(o, "item", t->item);
    cJSON *tg = cJSON_AddObjectToObject(o, "targets");
    float a[2];
    a[0] = t->front.x, a[1] = t->front.y;
    cJSON_AddItemToObject(tg, "front_sight", cJSON_CreateFloatArray(a, 2));
    a[0] = t->rear.x, a[1] = t->rear.y;
    cJSON_AddItemToObject(tg, "rear_sight", cJSON_CreateFloatArray(a, 2));
    a[0] = t->left_hand.x, a[1] = t->left_hand.y;
    cJSON_AddItemToObject(tg, "left_hand", cJSON_CreateFloatArray(a, 2));
    cJSON_AddNumberToObject(tg, "exit_x", t->exit_x);
    cJSON_AddNumberToObject(tg, "angle", t->angle);
    cJSON_AddNumberToObject(tg, "cant", t->cant);
    cJSON *tol = cJSON_AddObjectToObject(o, "tolerance");
    cJSON_AddNumberToObject(tol, "point", t->tol);
    cJSON_AddNumberToObject(tol, "left_hand", t->hand_tol);
    cJSON_AddNumberToObject(tol, "angle", t->angle_tol);
    cJSON_AddNumberToObject(tol, "class_hand", t->class_tol);
    cJSON *cl = cJSON_AddObjectToObject(o, "classes");
    for (int i = 0; i < t->ncls; i++) {
        cJSON *c = cJSON_AddObjectToObject(cl, t->cls[i].id);
        cJSON_AddStringToObject(c, "hand", t->cls[i].hand ? "right" : "left");
        a[0] = t->cls[i].hand_at.x, a[1] = t->cls[i].hand_at.y;
        cJSON_AddItemToObject(c, "at", cJSON_CreateFloatArray(a, 2));
    }
    char *text = cJSON_Print(o);
    cJSON_Delete(o);
    char path[600];
    bool ok = false;
#ifdef DG_SOURCE_DIR
    snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, FF_JSON);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fputc('\n', f); fclose(f); ok = true; }
#endif
    platform_data_path(FF_JSON, path, sizeof(path));
    FILE *g = fopen(path, "wb");
    if (g) { fputs(text, g); fputc('\n', g); fclose(g); ok = true; }
    free(text);
    return ok;
}

// ------------------------------------------------------------------ measuring

static bool scene(FfScene *s) {
    memset(s, 0, sizeof(*s));
    if (!FF.loaded) load();
    if (!game_arms_last(&s->in, &s->out) || !game_arms_last_fp(&s->fa) || !s->in.has_item) return false;
    if (!hands_fp_item(NULL, NULL, &s->wd) || !s->wd || s->wd->kind == WPN_MELEE) return false;
    // The drawn item as the arms layer took it this frame (pose eye x item): the hands' copy can be a
    // frame behind while the item moves.
    Xform item = xf_mul(s->in.pose_eye, s->in.item_cam);
    s->pos = item.t;
    s->rot = item.r;
    s->k = hands_fp_scale();
    s->mesh = item_mesh_get(s->wd->model.mesh);
    s->front_part = s->mesh ? item_mesh_find_part(s->mesh, "front_sight") : -1;
    s->rear_part = s->mesh ? item_mesh_find_part(s->mesh, "rear_sight") : -1;
    int w = render_width(), h = render_height();
    s->aspect = (w > 0 && h > 0) ? (float)w / (float)h / render_stretch_factor() : FOV_REF_ASPECT;
    return true;
}

static vec3 item_world(const FfScene *s, vec3 p) { return v3_add(s->pos, quat_rotate(s->rot, v3_scale(p, s->k))); }

// The top centre of a part's box (a sight post's tip, a rear sight's aperture), item space.
static vec3 part_top(const ItemMesh *m, int part) {
    ItemMeshPose pose = { 0 };
    pose.wobble = quat_identity();
    Xform px = item_mesh_part_xform(m, part, &pose);
    const ItemMeshPart *p = &m->parts[part];
    return xf_point(px, v3((p->bmin.x + p->bmax.x) * 0.5f, p->bmax.y, (p->bmin.z + p->bmax.z) * 0.5f));
}

// A world point through the arms camera: screen fractions from the top left (false: behind the eye).
static bool project(mat4 view, float tv, float aspect, vec3 w, vec2 *uv, float *depth) {
    vec3 c = mat4_mul_point(view, w);
    float z = -c.z;
    if (depth) *depth = z;
    if (z < 1e-4f) return false;
    *uv = v2(0.5f + 0.5f * (c.x / z) / (tv * aspect), 0.5f - 0.5f * (c.y / z) / tv);
    return true;
}

static FfMeasure measure(const FfScene *s, mat4 view, float fov_v) {
    FfMeasure m;
    memset(&m, 0, sizeof(m));
    m.fov_v = fov_v;
    m.exit_x = NAN;
    float tv = tanf(fov_v * 0.5f * DG_DEG2RAD), A = s->aspect;
    vec3 fl, rl;
    m.sights = s->front_part >= 0 && s->rear_part >= 0;
    if (m.sights) {
        fl = part_top(s->mesh, s->front_part);
        rl = part_top(s->mesh, s->rear_part);
    } else {
        fl = s->wd->sockets.muzzle;
        rl = s->in.sight;
    }
    float zf = 0, zr = 0;
    bool ok = project(view, tv, A, item_world(s, fl), &m.front, &zf) && project(view, tv, A, item_world(s, rl), &m.rear, &zr);
    vec3 mz = s->wd->sockets.muzzle;
    ok = ok && project(view, tv, A, item_world(s, mz), &m.muzzle, NULL);
    ok = ok && project(view, tv, A, item_world(s, s->in.sight), &m.sight, NULL);
    for (int h = 0; h < 2; h++) ok = ok && project(view, tv, A, s->fa.hand_c[h], &m.hand[h], NULL);
    if (!ok) return m;
    m.valid = true;
    m.depth_ratio = zr > 1e-4f ? zf / zr : 0;
    m.muzzle_in = m.muzzle.x >= 0 && m.muzzle.x <= 1 && m.muzzle.y >= 0 && m.muzzle.y <= 1;
    // The bore axis (the muzzle back along +Z to the grip's plane) where it meets the bottom edge.
    vec2 b0, b1;
    if (project(view, tv, A, item_world(s, mz), &b0, NULL) && project(view, tv, A, item_world(s, v3(mz.x, mz.y, 0.0f)), &b1, NULL) && fabsf(b1.y - b0.y) > 1e-6f)
        m.exit_x = b0.x + (1.0f - b0.y) / (b1.y - b0.y) * (b1.x - b0.x);
    // The sight line (rear -> front) above horizontal, in pixels; the cant: the item's up axis at the
    // front sight (the post) against the screen's vertical (+ = leaning right, clockwise). A gun that
    // points straight ahead shows its sight line towards the screen centre, its post upright.
    float dx = (m.front.x - m.rear.x) * A, dy = m.rear.y - m.front.y;
    m.angle = atan2f(dy, -dx) * DG_RAD2DEG;
    vec2 up;
    if (project(view, tv, A, item_world(s, v3_add(fl, v3(0, 0.03f, 0))), &up, NULL)) m.cant = atan2f((up.x - m.front.x) * A, m.front.y - up.y) * DG_RAD2DEG;
    m.length = 1.0f - m.front.y;
    // The nearest part box wholly in front of the arms camera (the fit's cheap check; the triangles:
    // near_cut).
    m.min_depth = 1e9f;
    if (s->mesh) {
        ItemMeshPose pose = { 0 };
        pose.wobble = quat_identity();
        for (int p = 0; p < s->mesh->part_count; p++) {
            Xform px = item_mesh_part_xform(s->mesh, p, &pose);
            const ItemMeshPart *q = &s->mesh->parts[p];
            float lo = 1e9f;
            for (int c = 0; c < 8; c++) {
                vec3 b = v3(c & 1 ? q->bmax.x : q->bmin.x, c & 2 ? q->bmax.y : q->bmin.y, c & 4 ? q->bmax.z : q->bmin.z);
                lo = fminf(lo, -mat4_mul_point(view, item_world(s, xf_point(px, b))).z);
            }
            if (lo > 0.0f) m.min_depth = fminf(m.min_depth, lo);
        }
    }
    return m;
}

static FfMeasure measure_live(const FfScene *s) { return measure(s, s->out.view, s->in.fov_hands); }

// Near-plane clipping through the arms camera, triangle by triangle (as item_nearplane does for the
// world camera): a triangle in view nearer than the plane, or crossing it where the crossing is on
// screen. Sets m->cut / cut_part and the nearest on-screen vertex.
static void near_cut(const FfScene *s, mat4 view, FfMeasure *m) {
    m->cut = 0;
    m->cut_part[0] = 0;
    if (!s->mesh || !s->mesh->dparts || !s->mesh->verts || !s->mesh->indices) return;
    float zn = game_arms_def()->znear, ty = tanf(m->fov_v * 0.5f * DG_DEG2RAD), tx = ty * s->aspect;
    ItemMeshPose pose = { 0 };
    pose.wobble = quat_identity();
    for (int p = 0; p < s->mesh->part_count; p++) {
        const DgimPart *dp = &s->mesh->dparts[p];
        if (dp->lod_count == 0) continue;
        Xform px = item_mesh_part_xform(s->mesh, p, &pose);
        int part_cut = 0;
        for (u32 i = 0; i + 2 < dp->lod_in[0]; i += 3) {
            vec3 v[3];
            float d[3];
            for (int k = 0; k < 3; k++) {
                vec3 l = item_mesh_vertex_pos(dp, &s->mesh->verts[dp->lod_v0[0] + s->mesh->indices[dp->lod_i0[0] + i + k]]);
                v[k] = mat4_mul_point(view, item_world(s, xf_point(px, l)));
                d[k] = -v[k].z;
            }
            bool cut = false;
            for (int k = 0; k < 3 && !cut; k++) {
                vec3 a = v[k], b = v[(k + 1) % 3];
                float da = d[k], db = d[(k + 1) % 3];
                if (da > 0 && da < zn && fabsf(a.x) <= da * tx && fabsf(a.y) <= da * ty) cut = true;
                if ((da < zn) != (db < zn)) {
                    vec3 c = v3_lerp(a, b, (zn - da) / (db - da));
                    if (fabsf(c.x) <= zn * tx && fabsf(c.y) <= zn * ty) cut = true;
                }
            }
            part_cut += cut;
        }
        if (part_cut && m->cut == 0) dg_strcpy(m->cut_part, sizeof(m->cut_part), s->mesh->parts[p].name);
        m->cut += part_cut;
    }
}

// The layer for a candidate hip placement of the held item, steady (no sway / kick / equip / aim).
static mat4 layer_view(const FfScene *s, ArmsPose p, float fov_v) {
    static ArmsLayerDef d;
    d = *game_arms_def();
    ArmsPose *e = arms_layer_edit(&d, HP_HIP, s->in.item_id);
    *e = p;
    ArmsLayerState st;
    arms_layer_reset(&st);
    ArmsLayerIn in = s->in;
    in.hp_state = HP_HIP;
    in.ads_w = 0.0f;
    in.equip_t = -1.0f;
    in.kick_pos = in.kick_rot = v3(0, 0, 0);
    in.sway_hip_rot = in.sway_hip_pos = in.sway_ads_rot = v3(0, 0, 0);
    in.screen_kick = 0.0f;
    in.aim_deg = v2(0, 0);
    // (in.lateral_scale as shown: fit at Weapon Field of View Default, where it is 1)
    in.fov_hands = fov_v;
    in.dt = 0.0f;
    ArmsLayerOut out;
    arms_layer_eval(&d, &st, &in, &out);
    return out.view;
}

// ------------------------------------------------------------------ fitting (Levenberg-Marquardt)

#define FF_NP 3                  // fitted parameters: the arms camera's position

typedef struct FfFit {
    const FfScene *s;
    float fov_v;
    const FfClass *cls;          // NULL: the full target set (the rifle)
    float x0[FF_NP];             // the class fit stays near this (the rifle's camera)
} FfFit;

// x: the arms camera's position (right, up, forward m); it never turns.
static ArmsPose pose_of(const float x[FF_NP], ArmsPose base) {
    base.cam_pos = v3(x[0], x[1], x[2]);
    return base;
}

static void x_of(ArmsPose p, float x[FF_NP]) { x[0] = p.cam_pos.x, x[1] = p.cam_pos.y, x[2] = p.cam_pos.z; }

static int residuals(const FfFit *f, const float x[FF_NP], float *r) {
    ArmsPose p = pose_of(x, arms_layer_target(game_arms_def(), HP_HIP, f->s->in.item_id));
    FfMeasure m = measure(f->s, layer_view(f->s, p, f->fov_v), f->fov_v);
    const FfTargets *t = &FF.t;
    float A = f->s->aspect;
    int n = 0;
    if (!m.valid) {
        for (int i = 0; i < 16; i++) r[i] = 1.0f;
        return f->cls ? 4 + FF_NP : 9;
    }
    if (!f->cls) {
        r[n++] = (m.front.x - t->front.x) * A, r[n++] = m.front.y - t->front.y;
        r[n++] = (m.rear.x - t->rear.x) * A, r[n++] = m.rear.y - t->rear.y;
        // The left hand weighs less: where it holds the gun is the gun's own grip (an IK target, never
        // moved), the reference gun's support hand sits further forward.
        r[n++] = (m.hand[0].x - t->left_hand.x) * A * 0.35f, r[n++] = (m.hand[0].y - t->left_hand.y) * 0.35f;
        r[n++] = isnan(m.exit_x) ? 1.0f : (m.exit_x - t->exit_x) * A * 0.5f;
        r[n++] = (m.angle - t->angle) / 200.0f;             // 1 deg weighs like 0.5 % of the screen
        r[n++] = (m.cant - t->cant) / 400.0f;               // loosely: the post upright, as the reference's
    } else {
        vec2 h = m.hand[f->cls->hand];
        r[n++] = (h.x - f->cls->hand_at.x) * A, r[n++] = h.y - f->cls->hand_at.y;
        r[n++] = m.front.y - t->front.y;                    // the rifle's sight height
        r[n++] = (m.angle - t->angle) / 60.0f;              // its sight line, loosely
        // Near the rifle's camera (the remaining freedom): 5 cm weighs like 1 % of the screen.
        for (int i = 0; i < FF_NP; i++) r[n++] = (x[i] - f->x0[i]) / 0.05f * 0.01f;
    }
    return n;
}

static float sumsq(const float *r, int n) {
    float s = 0;
    for (int i = 0; i < n; i++) s += r[i] * r[i];
    return s;
}

static bool solve(float A[FF_NP][FF_NP], float b[FF_NP], float x[FF_NP]) {
    float M[FF_NP][FF_NP + 1];
    for (int i = 0; i < FF_NP; i++) {
        for (int j = 0; j < FF_NP; j++) M[i][j] = A[i][j];
        M[i][FF_NP] = b[i];
    }
    for (int c = 0; c < FF_NP; c++) {
        int piv = c;
        for (int r = c + 1; r < FF_NP; r++)
            if (fabsf(M[r][c]) > fabsf(M[piv][c])) piv = r;
        if (fabsf(M[piv][c]) < 1e-12f) return false;
        if (piv != c)
            for (int j = 0; j <= FF_NP; j++) { float t = M[c][j]; M[c][j] = M[piv][j]; M[piv][j] = t; }
        for (int r = c + 1; r < FF_NP; r++) {
            float k = M[r][c] / M[c][c];
            for (int j = c; j <= FF_NP; j++) M[r][j] -= k * M[c][j];
        }
    }
    for (int i = FF_NP - 1; i >= 0; i--) {
        float v = M[i][FF_NP];
        for (int j = i + 1; j < FF_NP; j++) v -= M[i][j] * x[j];
        x[i] = v / M[i][i];
    }
    return true;
}

static void clamp_x(float x[FF_NP]) {
    for (int i = 0; i < FF_NP; i++) x[i] = clampf(x[i], -0.8f, 0.8f);
}

// Fits x (the camera's position, m) in place; returns the RMS residual (screen heights).
static float fit(const FfFit *f, float x[FF_NP]) {
    float r[32], rn[32], J[32][FF_NP];
    int n = residuals(f, x, r);
    float cost = sumsq(r, n), lambda = 1e-3f;
    static const float h = 1e-4f;   // m
    for (int it = 0; it < 120; it++) {
        for (int j = 0; j < FF_NP; j++) {
            float xp[FF_NP];
            memcpy(xp, x, sizeof(xp));
            xp[j] += h;
            residuals(f, xp, rn);
            for (int i = 0; i < n; i++) J[i][j] = (rn[i] - r[i]) / h;
        }
        float JtJ[FF_NP][FF_NP], g[FF_NP];
        for (int a = 0; a < FF_NP; a++) {
            g[a] = 0;
            for (int i = 0; i < n; i++) g[a] += J[i][a] * r[i];
            for (int b = 0; b < FF_NP; b++) {
                JtJ[a][b] = 0;
                for (int i = 0; i < n; i++) JtJ[a][b] += J[i][a] * J[i][b];
            }
        }
        bool stepped = false;
        for (int tries = 0; tries < 8 && !stepped; tries++) {
            float M[FF_NP][FF_NP], b[FF_NP], d[FF_NP];
            for (int a = 0; a < FF_NP; a++) {
                for (int c = 0; c < FF_NP; c++) M[a][c] = JtJ[a][c];
                M[a][a] += lambda * (JtJ[a][a] + 1e-9f);
                b[a] = -g[a];
            }
            if (!solve(M, b, d)) { lambda *= 10.0f; continue; }
            float xn[FF_NP];
            for (int a = 0; a < FF_NP; a++) xn[a] = x[a] + d[a];
            clamp_x(xn);
            int nn = residuals(f, xn, rn);
            float cn = sumsq(rn, nn);
            if (cn < cost) {
                memcpy(x, xn, sizeof(xn));
                memcpy(r, rn, sizeof(float) * (size_t)nn);
                bool small = fabsf(cost - cn) < 1e-12f;
                cost = cn;
                lambda = fmaxf(lambda * 0.3f, 1e-7f);
                stepped = true;
                if (small) it = 1000;
            } else {
                lambda *= 8.0f;
            }
        }
        if (!stepped) break;
    }
    return sqrtf(cost / (float)n);
}

static const FfClass *class_of(const char *id) {
    for (int i = 0; i < FF.t.ncls; i++)
        if (id && !strcmp(FF.t.cls[i].id, id)) return &FF.t.cls[i];
    return NULL;
}

// ------------------------------------------------------------------ report

static bool near2(vec2 a, vec2 b, float tol) { return fabsf(a.x - b.x) <= tol && fabsf(a.y - b.y) <= tol; }

static void report(const char *tag) {
    FfScene s;
    if (!scene(&s)) { log_info("fp_framing %s: no first-person gun this frame", tag); return; }
    FfMeasure m = measure_live(&s);
    if (!m.valid) { log_info("fp_framing %s: a point is behind the arms camera", tag); return; }
    near_cut(&s, s.out.view, &m);
    const FfTargets *t = &FF.t;
    float world = G->view.fov;
    log_info("fp_framing %s: item %s, arms FOV %.2f (vertical; %.1f horizontal at 16:9), world FOV %.2f, %s", tag, s.in.item_id, m.fov_v, fov_v_to_h16(m.fov_v), world,
             m.sights ? "front / rear sight parts" : "muzzle / ADS sight");
    log_info("fp_framing %s: front %.3f %.3f  rear %.3f %.3f  muzzle %.3f %.3f  sight %.3f %.3f  left hand %.3f %.3f  right hand %.3f %.3f", tag, m.front.x, m.front.y,
             m.rear.x, m.rear.y, m.muzzle.x, m.muzzle.y, m.sight.x, m.sight.y, m.hand[0].x, m.hand[0].y, m.hand[1].x, m.hand[1].y);
    log_info("fp_framing %s: bore exit %.3f  angle %.1f deg  cant %.1f deg cw  visible length %.1f %%  depth front / rear %.3f  nearest %.3f m (near plane %.3f, %d triangles cut on screen%s%s)  muzzle %s  [%s]",
             tag, m.exit_x, m.angle, m.cant, m.length * 100.0f, m.depth_ratio, m.min_depth, game_arms_def()->znear, m.cut, m.cut ? ": " : "", m.cut_part, m.muzzle_in ? "on screen" : "OFF SCREEN",
             chars_holdpoint_text(G->world, G->local_slot));
    bool near_ok = m.cut == 0 && m.muzzle_in;
    if (s.in.item_id && !strcmp(s.in.item_id, t->item)) {
        bool f = near2(m.front, t->front, t->tol), r = near2(m.rear, t->rear, t->tol), l = near2(m.hand[0], t->left_hand, t->hand_tol);
        bool e = !isnan(m.exit_x) && fabsf(m.exit_x - t->exit_x) <= t->tol, a = fabsf(m.angle - t->angle) <= t->angle_tol;
        log_info("fp_framing %s: vs reference: front %+.3f %+.3f %s, rear %+.3f %+.3f %s, left hand %+.3f %+.3f %s, bore exit %+.3f %s, angle %+.1f %s, near plane / muzzle %s -> %s",
                 tag, m.front.x - t->front.x, m.front.y - t->front.y, f ? "ok" : "FAIL", m.rear.x - t->rear.x, m.rear.y - t->rear.y, r ? "ok" : "FAIL",
                 m.hand[0].x - t->left_hand.x, m.hand[0].y - t->left_hand.y, l ? "ok" : "FAIL", m.exit_x - t->exit_x, e ? "ok" : "FAIL", m.angle - t->angle,
                 a ? "ok" : "FAIL", near_ok ? "ok" : "FAIL", f && r && l && e && a && near_ok ? "PASS" : "FAIL");
    } else {
        const FfClass *c = class_of(s.in.item_id);
        vec2 want = c ? c->hand_at : t->left_hand;
        int hand = c ? c->hand : 0;
        bool ok = near2(m.hand[hand], want, t->class_tol);
        log_info("fp_framing %s: class %s: %s hand %+.3f %+.3f (%.0f %% tolerance) %s, sight height %+.3f, near plane / muzzle %s -> %s", tag, s.in.item_id, hand ? "right" : "left",
                 m.hand[hand].x - want.x, m.hand[hand].y - want.y, t->class_tol * 100.0f, ok ? "ok" : "FAIL", m.front.y - t->front.y, near_ok ? "ok" : "FAIL",
                 ok && near_ok ? "PASS" : "FAIL");
    }
}

static void cmd_report(int argc, const char **argv) { report(argc > 1 ? argv[1] : "now"); }

// fp_framing_fit [fov h16]: fit the held item's hip placement at the arms FOV in use (or the given one).
static void cmd_fit(int argc, const char **argv) {
    FfScene s;
    if (!scene(&s)) { log_info("fp_framing_fit: no first-person gun this frame"); return; }
    float fov_v = argc > 1 ? camera_fov_v((float)atof(argv[1])) : s.in.fov_hands;
    ArmsLayerDef *d = game_arms_def();
    ArmsPose cur = arms_layer_target(d, HP_HIP, s.in.item_id);
    FfFit f = { &s, fov_v, NULL, { 0 } };
    bool rifle = s.in.item_id && !strcmp(s.in.item_id, FF.t.item);
    if (!rifle) {
        // Other classes start from (and stay near) the rifle's camera.
        f.cls = class_of(s.in.item_id);
        static FfClass dflt;
        if (!f.cls) {
            memset(&dflt, 0, sizeof(dflt));
            dg_strcpy(dflt.id, sizeof(dflt.id), s.in.item_id ? s.in.item_id : "-");
            dflt.hand_at = FF.t.left_hand;
            f.cls = &dflt;
        }
        x_of(arms_layer_target(d, HP_HIP, FF.t.item), f.x0);
    }
    float x[FF_NP];
    x_of(cur, x);
    if (!rifle) memcpy(x, f.x0, sizeof(x));
    if (fabsf(s.in.lateral_scale - 1.0f) > 1e-3f)
        log_warn("fp_framing_fit: Weapon Field of View is not Default (or cl_hands_fov overrides it): the fit holds for this preset only");
    float rms = fit(&f, x);
    // The hip and its variants (crouch / prone / sprint frame their own hold with the same camera).
    static const int states[] = { HP_HIP, HP_CROUCH, HP_PRONE, HP_SPRINT };
    for (int i = 0; i < 4; i++) {
        ArmsPose *e = arms_layer_edit(d, states[i], s.in.item_id);
        *e = pose_of(x, *e);
    }
    snprintf(FF.status, sizeof(FF.status), "fit %s at %.1f (h16): rms %.4f, camera %.3f %.3f %.3f m", s.in.item_id, fov_v_to_h16(fov_v), rms, x[0], x[1], x[2]);
    log_info("fp_framing_fit: %s (the arms camera of %s at hip / crouch / prone / sprint; arms_layer_save writes it)", FF.status, s.in.item_id);
}

// fp_framing_sweep <lo> <hi> <step>: the rifle fit at each arms FOV (horizontal at 16:9).
static void cmd_sweep(int argc, const char **argv) {
    FfScene s;
    if (!scene(&s) || !s.in.item_id || strcmp(s.in.item_id, FF.t.item)) { log_info("fp_framing_sweep: hold the %s in first person", FF.t.item); return; }
    float lo = argc > 1 ? (float)atof(argv[1]) : 50.0f, hi = argc > 2 ? (float)atof(argv[2]) : 100.0f, step = argc > 3 ? (float)atof(argv[3]) : 2.5f;
    ArmsPose cur = arms_layer_target(game_arms_def(), HP_HIP, s.in.item_id);
    float x[FF_NP];
    x_of(cur, x);
    float best = 1e9f, best_h = lo;
    for (float h = lo; h <= hi + 1e-3f; h += fmaxf(step, 0.1f)) {
        FfFit f = { &s, camera_fov_v(h), NULL, { 0 } };
        float rms = fit(&f, x);
        mat4 view = layer_view(&s, pose_of(x, cur), f.fov_v);
        FfMeasure m = measure(&s, view, f.fov_v);
        near_cut(&s, view, &m);
        float worst = fmaxf(fmaxf(v2_len(v2_sub(m.front, FF.t.front)), v2_len(v2_sub(m.rear, FF.t.rear))), v2_len(v2_sub(m.hand[0], FF.t.left_hand)));
        log_info("fp_framing_sweep: arms FOV %.1f (h16, %.2f vertical): rms %.4f, worst point %.3f, exit %.3f, angle %.1f, depth front / rear %.3f, nearest %.3f m, %d triangles cut, camera %.3f %.3f %.3f m, "
                 "left hand %+.3f %+.3f",
                 h, f.fov_v, rms, worst, m.exit_x, m.angle, m.depth_ratio, m.min_depth, m.cut, x[0], x[1], x[2], m.hand[0].x - FF.t.left_hand.x, m.hand[0].y - FF.t.left_hand.y);
        if (rms < best) best = rms, best_h = h;
    }
    log_info("fp_framing_sweep: best arms FOV %.1f (rms %.4f)", best_h, best);
}

// ------------------------------------------------------------------ overlay, tracking, capture

static void load_image(void) {
    if (FF.tex && !strcmp(FF.tex_path, FF.t.image)) return;
    char path[600];
    int w, h, comp;
    u8 *px = stbi_load(platform_data_path(FF.t.image, path, sizeof(path)), &w, &h, &comp, 4);
    if (!px) { snprintf(FF.status, sizeof(FF.status), "reference %s missing", FF.t.image); return; }
    if (FF.tex) render_texture_destroy(FF.tex);
    FF.tex = render_texture_create(w, h, px, true, false);
    stbi_image_free(px);
    FF.tex_w = w, FF.tex_h = h;
    dg_strcpy(FF.tex_path, sizeof(FF.tex_path), FF.t.image);
}

static void cmd_overlay(int argc, const char **argv) {
    if (!FF.loaded) load();
    FF.overlay = argc > 1 ? atoi(argv[1]) != 0 : !FF.overlay;
    if (argc > 2) FF.alpha = clampf((float)atof(argv[2]), 0.0f, 1.0f);
    if (FF.overlay) load_image();
}

static void cmd_track(int argc, const char **argv) { FF.track = argc > 1 ? atoi(argv[1]) : 60; }

static void cmd_capture(int argc, const char **argv) {
    const char *name = argc > 1 ? argv[1] : "capture";
    FfScene s;
    char dir[600], cmd[700];
#ifdef DG_SOURCE_DIR
    snprintf(dir, sizeof(dir), "%s/docs/fp_framing", DG_SOURCE_DIR);
#else
    snprintf(dir, sizeof(dir), "docs/fp_framing");
#endif
    SDL_CreateDirectory(dir);
    if (scene(&s)) {
        FfMeasure m = measure_live(&s);
        char path[700];
        snprintf(path, sizeof(path), "%s/%s.json", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f,
                    "{\"item\": \"%s\", \"arms_fov_v\": %.3f, \"world_fov_v\": %.3f, \"front\": [%.4f, %.4f], \"rear\": [%.4f, %.4f], \"muzzle\": [%.4f, %.4f], "
                    "\"left_hand\": [%.4f, %.4f], \"right_hand\": [%.4f, %.4f], \"exit_x\": %.4f, \"angle\": %.2f, \"cant\": %.2f, \"targets\": {\"front\": [%.4f, %.4f], "
                    "\"rear\": [%.4f, %.4f], \"left_hand\": [%.4f, %.4f], \"exit_x\": %.4f, \"angle\": %.2f}, \"image\": \"%s\"}\n",
                    s.in.item_id, m.fov_v, G->view.fov, m.front.x, m.front.y, m.rear.x, m.rear.y, m.muzzle.x, m.muzzle.y, m.hand[0].x, m.hand[0].y, m.hand[1].x, m.hand[1].y,
                    isnan(m.exit_x) ? -1.0f : m.exit_x, m.angle, m.cant, FF.t.front.x, FF.t.front.y, FF.t.rear.x, FF.t.rear.y, FF.t.left_hand.x, FF.t.left_hand.y, FF.t.exit_x,
                    FF.t.angle, FF.t.image);
            fclose(f);
        }
    }
    snprintf(cmd, sizeof(cmd), "%s/%s.png", dir, name);
    render_screenshot(cmd);   // directly: a queued one would run only after the script's next wait
    snprintf(FF.status, sizeof(FF.status), "captured docs/fp_framing/%s (py tools/fp_framing_compare.py %s)", name, name);
    log_info("fp_framing_capture: %s", FF.status);
}

static void marker(vec2 p, float r, u32 c, bool cross) {
    float W = ui_width(), H = ui_height(), x = p.x * W, y = p.y * H;
    if (cross) {
        ui_line(x - r, y, x + r, y, 2.0f, c);
        ui_line(x, y - r, x, y + r, 2.0f, c);
    } else {
        ui_rect_outline(x - r, y - r, 2 * r, 2 * r, 2.0f, c);
    }
}

void fp_framing_draw(void) {
    FfScene s;
    bool have = (FF.overlay || FF.track > 0) && scene(&s);
    FfMeasure m;
    memset(&m, 0, sizeof(m));
    if (have) FF.live = m = measure_live(&s);
    if (FF.track > 0) {
        FF.track--;
        if (have && m.valid)
            log_info("fp_framing_track: frame %u ads %.3f front %.4f %.4f rear %.4f %.4f sight %.4f %.4f left %.4f %.4f arms FOV %.2f camera %.3f sight_eye %.4f %.4f %.4f",
                     (unsigned)g_engine.frame_index, s.in.ads_w, m.front.x, m.front.y, m.rear.x, m.rear.y, m.sight.x, m.sight.y, m.hand[0].x, m.hand[0].y, m.fov_v, s.out.cam_w,
                     s.out.sight.x, s.out.sight.y, s.out.sight.z);
    }
    if (!FF.overlay) return;
    float W = ui_width(), H = ui_height();
    if (FF.tex) gd_image((GuiRect){ 0, 0, W, H }, FF.tex, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, (u32)lrintf(clampf(FF.alpha, 0.0f, 1.0f) * 255.0f)));
    // Targets (yellow crosses; draggable while editing) and this frame's points (cyan boxes).
    vec2 *tg[3] = { &FF.t.front, &FF.t.rear, &FF.t.left_hand };
    static const char *const names[3] = { "front sight", "rear sight", "left hand" };
    if (FF.edit) {
        vec2 mp = gui_mouse();
        vec2 mf = v2(mp.x / W, mp.y / H);
        if (gui_mouse_pressed(0) && !ui_mouse_over_window()) {
            FF.drag = 0;
            for (int i = 0; i < 3; i++)
                if (v2_len(v2_sub(v2(tg[i]->x * W, tg[i]->y * H), mp)) < 14.0f) FF.drag = i + 1;
            if (!FF.drag && fabsf(mf.y - 1.0f) < 0.03f && fabsf(mf.x - FF.t.exit_x) < 0.02f) FF.drag = 4;
        }
        if (FF.drag && gui_mouse_down(0)) {
            if (FF.drag <= 3) *tg[FF.drag - 1] = v2(clampf(mf.x, 0, 1), clampf(mf.y, 0, 1.2f));
            else FF.t.exit_x = clampf(mf.x, 0, 1);
        }
        if (gui_mouse_released(0)) FF.drag = 0;
    }
    for (int i = 0; i < 3; i++) {
        marker(*tg[i], 10.0f, UI_RGBA(255, 220, 40, 230), true);
        ui_text_shadow(tg[i]->x * W + 12, tg[i]->y * H - 8, FONT_SMALL, UI_RGBA(255, 220, 40, 230), UI_ALIGN_LEFT, names[i]);
    }
    ui_line(FF.t.exit_x * W, H - 14, FF.t.exit_x * W, H, 3.0f, UI_RGBA(255, 220, 40, 230));
    if (have && m.valid) {
        vec2 pts[3] = { m.front, m.rear, m.hand[0] };
        for (int i = 0; i < 3; i++) {
            marker(pts[i], 6.0f, UI_RGBA(60, 230, 255, 240), false);
            ui_line(pts[i].x * W, pts[i].y * H, tg[i]->x * W, tg[i]->y * H, 1.0f, UI_RGBA(60, 230, 255, 160));
        }
        ui_line(m.rear.x * W, m.rear.y * H, m.front.x * W, m.front.y * H, 1.0f, UI_RGBA(60, 230, 255, 200));
        if (!isnan(m.exit_x)) ui_line(m.exit_x * W, H - 14, m.exit_x * W, H, 2.0f, UI_RGBA(60, 230, 255, 240));
        char line[220];
        snprintf(line, sizeof(line), "front %+.1f %+.1f  rear %+.1f %+.1f  left hand %+.1f %+.1f  exit %+.1f (%%)  angle %+.1f deg  arms FOV %.1f", (m.front.x - FF.t.front.x) * 100,
                 (m.front.y - FF.t.front.y) * 100, (m.rear.x - FF.t.rear.x) * 100, (m.rear.y - FF.t.rear.y) * 100, (m.hand[0].x - FF.t.left_hand.x) * 100,
                 (m.hand[0].y - FF.t.left_hand.y) * 100, isnan(m.exit_x) ? 0.0f : (m.exit_x - FF.t.exit_x) * 100, m.angle - FF.t.angle, fov_v_to_h16(m.fov_v));
        ui_text_shadow(W * 0.5f, 70, FONT_SMALL, UI_RGBA(60, 230, 255, 240), UI_ALIGN_CENTER, line);
    }
}

// The arms tuner's section (src/ui/arms_tuner.c).
void fp_framing_tuner(struct nk_context *ctx) {
    float s = ui_scale();
    if (!FF.loaded) load();
    if (!nk_tree_push(ctx, NK_TREE_TAB, "Reference framing (fp_framing)", NK_MINIMIZED)) return;
    nk_layout_row_dynamic(ctx, 20 * s, 2);
    nk_bool on = FF.overlay, ed = FF.edit;
    if (nk_checkbox_label(ctx, "reference overlay", &on)) {
        FF.overlay = on;
        if (on) load_image();
    }
    if (nk_checkbox_label(ctx, "drag targets", &ed)) FF.edit = ed;
    nk_layout_row_dynamic(ctx, 20 * s, 2);
    nk_label(ctx, "opacity", NK_TEXT_LEFT);
    nk_slider_float(ctx, 0.0f, &FF.alpha, 1.0f, 0.05f);
    nk_layout_row_dynamic(ctx, 24 * s, 3);
    if (nk_button_label(ctx, "Camera level")) G->view.pitch = 0.0f;
    char b[48];
    snprintf(b, sizeof(b), "Pitch %+.0f (ref)", FF.t.pitch);
    if (nk_button_label(ctx, b)) G->view.pitch = FF.t.pitch * DG_DEG2RAD;
    if (nk_button_label(ctx, "Report")) report("tuner");
    nk_layout_row_dynamic(ctx, 24 * s, 3);
    if (nk_button_label(ctx, "Fit this item")) cmd_fit(0, NULL);
    if (nk_button_label(ctx, "Capture")) {
        const char *argv[2] = { "fp_framing_capture", "tuner" };
        cmd_capture(2, argv);
    }
    if (nk_button_label(ctx, "Save targets")) snprintf(FF.status, sizeof(FF.status), save() ? "saved %s" : "save failed (%s)", FF_JSON);
    if (FF.live.valid) {
        char line[200];
        nk_layout_row_dynamic(ctx, 16 * s, 1);
        snprintf(line, sizeof(line), "front %.3f %.3f (target %.3f %.3f)", FF.live.front.x, FF.live.front.y, FF.t.front.x, FF.t.front.y);
        nk_label(ctx, line, NK_TEXT_LEFT);
        snprintf(line, sizeof(line), "rear %.3f %.3f (target %.3f %.3f)", FF.live.rear.x, FF.live.rear.y, FF.t.rear.x, FF.t.rear.y);
        nk_label(ctx, line, NK_TEXT_LEFT);
        snprintf(line, sizeof(line), "left hand %.3f %.3f (target %.3f %.3f)", FF.live.hand[0].x, FF.live.hand[0].y, FF.t.left_hand.x, FF.t.left_hand.y);
        nk_label(ctx, line, NK_TEXT_LEFT);
        snprintf(line, sizeof(line), "bore exit %.3f (target %.3f), angle %.1f (target %.1f)", FF.live.exit_x, FF.t.exit_x, FF.live.angle, FF.t.angle);
        nk_label(ctx, line, NK_TEXT_LEFT);
    }
    if (FF.status[0]) {
        nk_layout_row_dynamic(ctx, 16 * s, 1);
        nk_label(ctx, FF.status, NK_TEXT_LEFT);
    }
    nk_tree_pop(ctx);
}

void fp_framing_register(void) {
    cmd_register("fp_framing", cmd_report, "fp_framing [tag]: the held gun's sights / bore / hands on screen through the arms layer against the reference targets (PASS / FAIL)");
    cmd_register("fp_framing_fit", cmd_fit, "fp_framing_fit [arms FOV h16]: fit the held item's hip arms camera to the reference (rifle) or its class targets");
    cmd_register("fp_framing_sweep", cmd_sweep, "fp_framing_sweep <lo> <hi> <step>: the rifle's reference fit at each arms FOV (horizontal deg at 16:9), the residuals");
    cmd_register("fp_framing_overlay", cmd_overlay, "fp_framing_overlay [0|1] [alpha]: the reference screenshot, its targets and this frame's points over the view");
    cmd_register("fp_framing_track", cmd_track, "fp_framing_track [frames]: log the gun's sights / left hand on screen every frame (hip <-> ADS checks)");
    cmd_register("fp_framing_capture", cmd_capture, "fp_framing_capture <name>: screenshot + measurements into docs/fp_framing/ (tools/fp_framing_compare.py)");
}
