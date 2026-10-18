// Leaning out of an open window (docs/BUILDING_KIT.md "Windows"): use on a window's lean spot (a
// marker named "lean" in the opening, gameplay/entities.c) once the window is open moves the camera
// out through the opening and lets the player look along the facade and down at the street. Client
// only, like the door peek (game_peek.c): the body stays where it stands, the camera blends out and
// back, the view is limited to the outside half-space. It ends on use / moving / jumping / attacking /
// the window closing / walking away / taking damage / dying.
#include "game_internal.h"
#include "core/engine.h"
#include "gameplay/entities.h"
#include "input/input.h"
#include "ui/gui/gui_widgets.h"
#include <math.h>
#include <string.h>

#define G (g_engine.game)
#define TH (gui_theme())

static struct {
    bool active;
    EntityId spot;
    vec3 eye0;                  // the eye when the lean began (the body does not move meanwhile)
    vec3 out;                   // the lean point, outside the opening
    float out_yaw;              // looking straight out
    float t;                    // blend 0..1
    Cvar *dist, *drop, *yaw_limit, *pitch_down;
} LN;

void lean_register(void) {
    LN.dist = cvar_reg_float("lean_distance", 0.55f, 0.2f, 0.9f, CVAR_LOCKED, "window lean: how far the head goes out past the window (m)");
    LN.drop = cvar_reg_float("lean_drop", 0.12f, 0.0f, 0.4f, CVAR_LOCKED, "window lean: how much the head drops while leaning (m)");
    LN.yaw_limit = cvar_reg_float("lean_yaw_limit", 85.0f, 20.0f, 90.0f, CVAR_LOCKED, "window lean: look left / right limit from straight out (deg)");
    LN.pitch_down = cvar_reg_float("lean_pitch_down", 82.0f, 30.0f, 88.0f, CVAR_LOCKED, "window lean: how far down the view may look (deg)");
}

bool lean_active(void) { return LN.active; }
float lean_amount(void) { return LN.t; }

bool lean_start(EntityId spot) {
    World *w = G ? G->world : NULL;
    if (LN.active || !w || !lean_spot_open(w, spot)) return false;
    int i = (int)ENT_INDEX(spot);
    vec3 n = quat_rotate(w->xf[i].rot, v3(0, 0, -1));   // the opening faces out along its -Z
    LN.active = true;
    LN.spot = spot;
    LN.eye0 = G->view.eye;
    LN.out = v3_madd(w->xf[i].pos, n, LN.dist->f);
    LN.out.y = LN.eye0.y - LN.drop->f;
    LN.out_yaw = atan2f(-n.x, -n.z);
    return true;
}

void lean_stop(void) { LN.active = false; }

void lean_on_hurt(void) { lean_stop(); }

static float wrap_pi(float a) {
    while (a > DG_PI) a -= 2.0f * DG_PI;
    while (a < -DG_PI) a += 2.0f * DG_PI;
    return a;
}

// Per frame, after the look is applied: ends the lean, keeps the view outside.
void lean_frame(float dt, bool ui_blocking) {
    World *w = G ? G->world : NULL;
    LN.t = LN.active ? fminf(1.0f, LN.t + dt / 0.35f) : fmaxf(0.0f, LN.t - dt / 0.3f);
    if (!LN.active) return;
    Player *p = game_local_player();
    bool end = !w || !p || p->sim.move.mode != MOVE_WALK || !lean_spot_open(w, LN.spot);
    if (!end && v3_dist(v3_flat(p->sim.move.pos), v3_flat(w->xf[ENT_INDEX(LN.spot)].pos)) > 1.6f) end = true;
    if (!end && !ui_blocking) {
        InputFrame f = input_frame();
        if (v2_len(f.move) > 0.3f || input_down(ACT_JUMP) || input_peek_press(ACT_ATTACK)) end = true;
        if (input_consume_press(ACT_USE)) end = true;
    }
    if (end) { lean_stop(); return; }
    float yl = LN.yaw_limit->f * DG_DEG2RAD;
    G->view.yaw = LN.out_yaw + clampf(wrap_pi(G->view.yaw - LN.out_yaw), -yl, yl);
    G->view.pitch = clampf(G->view.pitch, -LN.pitch_down->f * DG_DEG2RAD, 50.0f * DG_DEG2RAD);
}

// game_render: the camera out of the window (blending in and back out).
bool lean_camera(World *w, RenderCamera *cam) {
    DG_UNUSED(w);
    if (LN.t <= 0.001f) return false;
    float s = LN.t * LN.t * (3.0f - 2.0f * LN.t);
    cam->pos = v3_lerp(LN.eye0, LN.out, s);
    return true;
}

// HUD: the exit hint (the peek's caps line).
void lean_draw(void) {
    if (LN.t <= 0.001f || !LN.active) return;
    char key[32], msg[96];
    binding_name(input_get_binding(ACT_USE, 0), key, sizeof(key));
    snprintf(msg, sizeof(msg), "Leaning out   %s  Back", key);
    GuiText t = { gui_s(TH->t_small), GW_SEMIBOLD, TH->hud_text, GUI_CENTER, TH->tracking_caps * 1.3f, true, true, 0 };
    gd_alpha_push(LN.t);
    gd_text(&t, gui_w() * 0.5f, gui_h() * 0.5f - fminf(gui_w(), gui_h()) * 0.47f + gui_s(84), msg);
    gd_alpha_pop();
}
