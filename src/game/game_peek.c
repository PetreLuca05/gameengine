// Door peek (docs/UI_WHEELS_HUD.md "Peek"): the door wheel's Peek looks through the door viewer
// (visor) to the other side. Client only: the camera sits in the visor on the far face looking
// away from you, with a wide fisheye view (post lens distortion + vignette), a round mask and a
// limited look around the visor axis. It ends on use / Esc / moving / taking damage / the door
// moving or opening / walking away / dying.
#include "game_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "gameplay/entities.h"
#include "input/input.h"
#include "render/post/post_settings.h"
#include "ui/gui/gui_widgets.h"
#include "audio/audio.h"
#include <math.h>
#include <string.h>

#define G (g_engine.game)
#define TH (gui_theme())

static struct {
    bool active;
    EntityId door;
    float yaw_off, pitch_off;   // look around the visor axis (radians)
    float t;                    // fade in / out 0..1
    float door_progress;        // at the start: any change ends the peek
    Cvar *fov, *distortion, *yaw_limit, *pitch_limit;
} PK;

void peek_register(void) {
    PK.fov = cvar_reg_float("peek_fov", 100.0f, 60.0f, 140.0f, CVAR_ARCHIVE, "door peek: field of view through the visor (deg)");
    PK.distortion = cvar_reg_float("peek_distortion", 0.55f, 0.0f, 1.0f, CVAR_ARCHIVE, "door peek: fisheye lens distortion");
    PK.yaw_limit = cvar_reg_float("peek_yaw_limit", 40.0f, 0.0f, 80.0f, 0, "door peek: look left / right limit around the visor axis (deg)");
    PK.pitch_limit = cvar_reg_float("peek_pitch_limit", 28.0f, 0.0f, 80.0f, 0, "door peek: look up / down limit (deg)");
}

bool peek_active(void) { return PK.active; }
float peek_amount(void) { return PK.t; }

static bool door_closed(World *w, EntityId door) {
    if (!ent_valid(w, door)) return false;
    int i = (int)ENT_INDEX(door);
    return w->type[i] == ET_DOOR && !(w->flags[i] & EF_OPEN) && w->anim[i] < 0.02f;
}

// Why a peek can't start (NULL = it can).
const char *peek_block_reason(EntityId door) {
    World *w = G ? G->world : NULL;
    if (!w || !ent_valid(w, door) || w->type[ENT_INDEX(door)] != ET_DOOR) return "Not a door";
    if (!door_closed(w, door)) return "Door is open";
    return NULL;
}

bool peek_start(EntityId door) {
    if (peek_block_reason(door)) return false;
    PK.active = true;
    PK.door = door;
    PK.yaw_off = PK.pitch_off = 0;
    PK.door_progress = G->world->anim[ENT_INDEX(door)];
    audio_play_2d("click", 0.35f, SND_SFX);
    return true;
}

void peek_stop(void) {
    if (!PK.active) return;
    PK.active = false;
    audio_play_2d("click", 0.25f, SND_SFX);
}

// Per frame before the look is applied: true = the look belongs to the peek (consumed).
bool peek_frame(float dt, vec2 look, bool ui_blocking) {
    World *w = G ? G->world : NULL;
    PK.t = PK.active ? fminf(1.0f, PK.t + dt / 0.18f) : fmaxf(0.0f, PK.t - dt / 0.15f);
    if (!PK.active) return false;
    Player *p = game_local_player();
    bool end = !w || !p || p->sim.move.mode != MOVE_WALK || !door_closed(w, PK.door) ||
               fabsf(w->anim[ENT_INDEX(PK.door)] - PK.door_progress) > 0.01f;
    if (!end) {
        vec3 knob = ent_focus_point(w, PK.door);
        if (v3_dist(v3_flat(p->sim.move.pos), v3_flat(knob)) > 1.9f) end = true;
    }
    if (!ui_blocking) {
        InputFrame f = input_frame();
        if (v2_len(f.move) > 0.3f || input_down(ACT_JUMP) || input_peek_press(ACT_ATTACK)) end = true;
        if (input_consume_press(ACT_USE)) end = true;
    }
    if (end) { peek_stop(); return true; }
    if (ui_blocking) return true;
    float yl = PK.yaw_limit->f * DG_DEG2RAD, pl = PK.pitch_limit->f * DG_DEG2RAD;
    PK.yaw_off = clampf(PK.yaw_off + look.x, -yl, yl);
    PK.pitch_off = clampf(PK.pitch_off + look.y, -pl, pl);
    return true;
}

// Damage ends it (EV_HURT).
void peek_on_hurt(void) { peek_stop(); }

// game_render: the visor camera.
bool peek_camera(World *w, RenderCamera *cam) {
    if (!PK.active) return false;
    Player *p = game_local_player();
    vec3 eye, dir;
    if (!p || !door_visor_view(w, PK.door, p->sim.move.pos, &eye, &dir)) return false;
    float base_yaw = atan2f(-dir.x, -dir.z);
    cam->pos = eye;
    cam->yaw = base_yaw + PK.yaw_off;
    cam->pitch = PK.pitch_off;
    cam->roll = 0;
    cam->fov_deg = PK.fov->f;
    return true;
}

// game_post: fisheye + vignette while peeking.
void peek_post(PostSettings *s) {
    if (PK.t <= 0.001f) return;
    s->distortion = lerpf(s->distortion, PK.distortion->f, PK.t);
    s->vignette = s->vignette + 0.9f * PK.t;
    s->chromatic = s->chromatic + 0.35f * PK.t;
}

// HUD: the round visor mask (soft edge) and the exit hint.
void peek_draw(void) {
    if (PK.t <= 0.001f) return;
    float a = PK.t;
    float cx = gui_w() * 0.5f, cy = gui_h() * 0.5f;
    float r = fminf(gui_w(), gui_h()) * 0.47f;
    float far = sqrtf(gui_w() * gui_w() + gui_h() * gui_h()) * 0.5f + 4.0f;
    gd_alpha_push(a);
    // Black outside the circle: the screen beyond its bounding square, then a thick ring (short
    // overlapping segments) from r out past the square's corners.
    u32 black = UI_RGBA(0, 0, 0, 255);
    gd_rect(gui_rect(0, 0, cx - r + 1.0f, gui_h()), black);
    gd_rect(gui_rect(cx + r - 1.0f, 0, gui_w() - (cx + r) + 1.0f, gui_h()), black);
    gd_rect(gui_rect(0, 0, gui_w(), cy - r + 1.0f), black);
    gd_rect(gui_rect(0, cy + r - 1.0f, gui_w(), gui_h() - (cy + r) + 1.0f), black);
    float th = r * 0.46f, rc = r + th * 0.5f;
    (void)far;
    int seg = 220;
    for (int k = 0; k < seg; k++) {
        float a0 = 2.0f * DG_PI * (float)k / (float)seg, a1 = 2.0f * DG_PI * ((float)k + 1.6f) / (float)seg;
        gd_line(cx + cosf(a0) * rc, cy + sinf(a0) * rc, cx + cosf(a1) * rc, cy + sinf(a1) * rc, th, black);
    }
    // Soft inner edge + the brass rim.
    for (int k = 0; k < 6; k++) gd_ring(cx, cy, r - (float)k * gui_s(5), gui_s(5), 1.0f, UI_RGBA(0, 0, 0, 150 - k * 25));
    gd_ring(cx, cy, r + gui_s(2), gui_s(3), 1.0f, UI_RGBA(120, 98, 52, 160));
    char key[32], msg[96];
    binding_name(input_get_binding(ACT_USE, 0), key, sizeof(key));
    snprintf(msg, sizeof(msg), "Peeking   %s  Stop", key);
    GuiText t = { gui_s(TH->t_small), GW_SEMIBOLD, TH->hud_text, GUI_CENTER, TH->tracking_caps * 1.3f, true, true, 0 };
    gd_text(&t, cx, cy - r + gui_s(84), msg);   // under the compass, clear of the hotbar
    gd_alpha_pop();
}
