// First-person arms layer (docs/CAMERA_HOLDPOINT.md "Rule 3"): assets/player/arms_layer.json, the
// per-frame evaluation (src/render/arms_layer.c) handed to the renderer, and the dev commands
// (arms_layer_reload / _save / _set / _info). The arms tuner panel (src/ui/arms_tuner.c) edits the
// same definition.
#include "game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/characters.h"
#include "gameplay/weapons.h"
#include "player/camera.h"
#include "render/arms_layer.h"
#include "render/render.h"
#include "player/spectator.h"
#include "ui/arms_tuner.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define ARMS_JSON "assets/player/arms_layer.json"

static struct {
    ArmsLayerDef def;
    ArmsLayerState st;
    ArmsLayerIn in;
    ArmsLayerOut out;
    CharsFpArms fa;           // the first-person arms it was built from
    bool have;                // the layer was set this frame
    Cvar *enable, *debug;
} AL;

ArmsLayerDef *game_arms_def(void) { return &AL.def; }
bool game_arms_last(ArmsLayerIn *in, ArmsLayerOut *out) {
    if (in) *in = AL.in;
    if (out) *out = AL.out;
    return AL.have;
}

bool game_arms_last_fp(CharsFpArms *fa) {
    if (fa) *fa = AL.fa;
    return AL.have;
}

bool game_arms_load(void) {
    char path[600];
    size_t size = 0;
    char *txt = platform_read_file(platform_data_path(ARMS_JSON, path, sizeof(path)), &size);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    arms_layer_parse(&AL.def, root);
    bool ok = root != NULL;
    if (!ok) log_warn("arms layer: cannot read %s (defaults)", path);
    cJSON_Delete(root);
    free(txt);
    arms_layer_reset(&AL.st);
    return ok;
}

bool game_arms_save(const char *path_override) {
    char *out = arms_layer_to_json(&AL.def);
    char path[600];
#ifdef DG_SOURCE_DIR
    if (!path_override) snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, ARMS_JSON);
#else
    if (!path_override) platform_data_path(ARMS_JSON, path, sizeof(path));
#endif
    if (path_override) snprintf(path, sizeof(path), "%s", path_override);
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) { fputs(out, f); fputc('\n', f); fclose(f); }
    // Runtime copy too (a reload keeps the edit when running from the build).
    char rt[600];
    snprintf(rt, sizeof(rt), "%s%s", platform_base_dir(), ARMS_JSON);
    if (ok && !path_override && strcmp(rt, path)) {
        FILE *g = fopen(rt, "wb");
        if (g) { fputs(out, g); fputc('\n', g); fclose(g); }
    }
    free(out);
    log_info("arms_layer_save: %s %s", ok ? "wrote" : "cannot write", path);
    return ok;
}

static void cmd_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("arms_layer_reload: %s", game_arms_load() ? "ok" : "defaults");
}
static void cmd_save(int argc, const char **argv) {
    if (cvar_release_locks()) { log_info("arms_layer_save: dev builds only"); return; }
    game_arms_save(argc > 1 ? argv[1] : NULL);
}

// arms_layer_set <state> <offset|rotation|scale|fov_add|camera|blend> <x> [y z] [item]
static void cmd_set(int argc, const char **argv) {
    if (argc < 4) { log_info("arms_layer_set <state> <offset|rotation|scale|fov_add|camera|blend> <x> [y z] [item]"); return; }
    if (cvar_release_locks()) { log_info("arms_layer_set: dev builds only"); return; }
    int s = holdpoint_state_from_name(argv[1]);
    if (s < 0) { log_info("arms_layer_set: unknown state '%s'", argv[1]); return; }
    bool vec = !strcmp(argv[2], "offset") || !strcmp(argv[2], "rotation") || !strcmp(argv[2], "camera");
    if (vec && argc < 6) { log_info("arms_layer_set: %s takes x y z", argv[2]); return; }
    const char *item = argc > (vec ? 6 : 4) ? argv[vec ? 6 : 4] : NULL;
    if (!strcmp(argv[2], "blend")) { AL.def.blend[s] = fmaxf(0.0f, (float)atof(argv[3])); return; }
    ArmsPose *p = arms_layer_edit(&AL.def, s, item);
    if (!strcmp(argv[2], "offset")) p->offset = v3(clampf((float)atof(argv[3]), -0.3f, 0.3f), clampf((float)atof(argv[4]), -0.3f, 0.3f), clampf((float)atof(argv[5]), -0.3f, 0.3f));
    else if (!strcmp(argv[2], "rotation")) p->rotation = v3((float)atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5]));
    else if (!strcmp(argv[2], "scale")) p->scale = clampf((float)atof(argv[3]), 0.9f, 1.1f);
    else if (!strcmp(argv[2], "fov_add")) p->fov_add = clampf((float)atof(argv[3]), -20.0f, 20.0f);
    else if (!strcmp(argv[2], "camera")) p->cam_pos = v3((float)atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5]));
    else { log_info("arms_layer_set: unknown field '%s'", argv[2]); return; }
    log_info("arms_layer_set: %s%s%s %s", holdpoint_state_name(s), item ? " / " : "", item ? item : "", argv[2]);
}

static void cmd_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!AL.have) { log_info("arms_layer_info: no arms layer this frame"); return; }
    const ArmsLayerIn *in = &AL.in;
    vec3 t = v3(AL.out.layer.m[12], AL.out.layer.m[13], AL.out.layer.m[14]);
    log_info("arms_layer_info: state %s item %s ads %.2f raise %.2f fov hands %.1f world %.1f | layer move %.1f %.1f %.1f mm | present %.1f %.1f %.1f mm",
             holdpoint_state_name(in->hp_state), in->item_id ? in->item_id : "-", in->ads_w, AL.out.raise, in->fov_hands, in->fov_world, t.x * 1000, t.y * 1000,
             t.z * 1000, in->present.t.x * 1000, in->present.t.y * 1000, in->present.t.z * 1000);
}

void game_arms_init(void) {
    AL.enable = cvar_reg_bool("arms_layer", true, CVAR_LOCKED,
                              "first-person arms layer: the arms + held item drawn with their own camera / FOV and presented ADS (0 = the world camera, no ADS presentation)");
    AL.debug = cvar_reg_int("arms_layer_debug", 0, 0, 1, CVAR_LOCKED, "1: log the arms layer every 30 frames");
    cmd_register("arms_layer_reload", cmd_reload, "reload assets/player/arms_layer.json (the first-person arms layer)");
    cmd_register("arms_layer_save", cmd_save, "write the arms layer definition [path] (the source tree's assets/player/arms_layer.json)");
    cmd_register("arms_layer_set", cmd_set, "arms_layer_set <state> <offset|rotation|scale|fov_add|camera|blend> <x> [y z] [item]: edit the arms layer live");
    cmd_register("arms_layer_info", cmd_info, "print this frame's arms layer (state, raise, FOVs, the layer move)");
    arms_tuner_init();
    game_arms_load();
}

float game_arms_hip_fov(void) { return camera_hands_fov(AL.def.weapon_fov); }

// The sprint's widening of the world FOV (camera.c: the hip FOV's tangent x (1 + cl_sprint_fov),
// smoothed; cam->vm_fov_deg is that hip FOV) as a tangent ratio against Field of View, 1 otherwise.
static float sprint_widen(const RenderCamera *cam) {
    float base = camera_base_fov();
    if (cam->vm_fov_deg <= 1.0f || base <= 1.0f) return 1.0f;
    return fmaxf(1.0f, tanf(cam->vm_fov_deg * 0.5f * DG_DEG2RAD) / tanf(base * 0.5f * DG_DEG2RAD));
}

// The arms FOV of this frame: Weapon Field of View (Hor+ like the world FOV, never the world's) plus
// the state's fov_add, widened like the world while sprinting (the gun and hands zoom out with the
// world instead of holding still against it); aimed, the sight's own FOV (render_vm_fov_for), the
// same for every preset.
static float hands_fov(const RenderCamera *cam, float fov_add) {
    RenderCamera hc = *cam;
    float v = clampf(game_arms_hip_fov() + fov_add, 20.0f, 120.0f);
    hc.vm_fov_deg = 2.0f * atanf(tanf(v * 0.5f * DG_DEG2RAD) * sprint_widen(cam)) * DG_RAD2DEG;
    return render_vm_fov_for(&hc, 0.0f);
}

void game_arms_frame(const RenderCamera *cam, vec2 aim_deg, float aim_pitch, float screen_kick) {
    AL.have = false;
    CharsFpArms fa;
    // No layer under a camera that isn't the player's eye (flythroughs, a spectator switching targets):
    // the arms then stay where the world camera sees them.
    if (!AL.enable || !AL.enable->i || render_camera_overridden() || spectator_blending() || !chars_fp_arms(&fa)) {
        render_set_arms_layer(NULL);
        return;
    }
    Player *p = game_view_player();
    AL.fa = fa;
    ArmsLayerIn *in = &AL.in;
    memset(in, 0, sizeof(*in));
    // A weapon's own run clip carrying the gun (controller "carry"): the arms camera glides from the
    // head's eye to the world camera, so the run's motion shows on screen (the presentation lets go of
    // the gun as well: the clip owns the arms, proc_presented_move).
    in->pose_eye = fa.pose_eye;
    if (fa.clip_w > 0.0f) in->pose_eye.t = v3_lerp(fa.pose_eye.t, cam->pos, saturate(fa.clip_w));
    in->present = fa.present;
    in->has_item = fa.has_item;
    in->item_cam = fa.item_cam;
    in->pivot = fa.pivot;
    in->sight = fa.sight;
    in->length = fa.length;
    in->item_id = fa.item_id;
    in->hp_state = fa.hp_state;
    in->ads_w = fa.ads_w;   // the presentation hold point's own ADS weight (the aim claims use the same)
    // The raise follows this frame, not the 60 Hz tick.
    in->equip_t = p && fa.has_item ? fminf(p->sim.wpn.equip_t + G->alpha * SIM_DT, AL.def.equip_time + 1.0f) : -1.0f;
    in->aim_deg = aim_deg;
    in->aim_pitch = aim_pitch;
    in->fov_world = cam->fov_deg;
    in->fov_hands = hands_fov(cam, AL.st.cur.fov_add * (1.0f - in->ads_w));   // last frame's fov_add (the aim needs the FOV first)
    in->kick_pos = fa.kick_pos;
    in->kick_rot = fa.kick_rot;
    const WeaponCvars *wc = weapons_cvars();
    in->kick_scale = wc && wc->visual_scale ? wc->visual_scale->f : 1.0f;
    in->sway_hip_rot = fa.sway_hip_rot;
    in->sway_hip_pos = fa.sway_hip_pos;
    in->sway_ads_rot = fa.sway_ads_rot;
    in->sway_scale = wc && wc->sway_cosmetic ? wc->sway_cosmetic->f : 1.0f;
    in->screen_kick = screen_kick;
    // Weapon Field of View: sideways with the preset so the sight keeps its place (against Default).
    in->lateral_scale = fov_lateral_scale(game_arms_hip_fov(), camera_fov_v(AL.def.weapon_fov[WEAPON_FOV_DEFAULT]));
    in->dt = (float)g_engine.frame_dt;
    arms_layer_eval(&AL.def, &AL.st, in, &AL.out);
    RenderArmsLayer rl;
    memset(&rl, 0, sizeof(rl));
    rl.valid = true;
    rl.view = AL.out.view;
    rl.fov_deg = hands_fov(cam, AL.out.fov_add);
    rl.znear = AL.def.znear;
    rl.arm_cut = AL.def.arm_cut;
    rl.shoulder[0] = fa.shoulder[0];
    rl.shoulder[1] = fa.shoulder[1];
    rl.seam_r0 = AL.def.seam_r0;   // (seam_r1 0: off, the arms camera draws only the arms + item)
    rl.seam_r1 = AL.def.seam ? AL.def.seam_r1 : 0.0f;
    rl.screen_scale = AL.out.screen_scale;   // smaller on screen about the anchor at the hip, 1 aimed
    rl.screen_anchor = AL.def.screen_anchor;
    in->fov_hands = rl.fov_deg;
    render_set_arms_layer(&rl);
    AL.have = true;
    if (AL.debug && AL.debug->i && (g_engine.frame_index % 30) == 0) cmd_info(0, NULL);
}
