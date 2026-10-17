// Fire animation tuner (fire_tuner, docs/FIRE_ANIM.md "Tuning a new weapon"): pick a gun, edit its
// mass / springs / body / camera / mechanical values live, loop single / burst / mag dump in slow
// motion, read the graphs (gun pitch, kickback, hand lag, camera reaction, FOV punch, the shot =
// audio transient marked), A/B against the stored values, save the class file.
#include "fire_tuner.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "anim/fire_anim.h"
#include "gameplay/items.h"
#include "gameplay/weapons.h"
#include "game/game_internal.h"
#include "nuklear_config.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool open;
    int weapon;
    FireAnimDef stored;   // A/B: the other set
    bool have_stored, showing_b;
    char status[160];
} T = { .weapon = -1 };

#define G (g_engine.game)

static float S(float v) { return v * ui_scale(); }

bool fire_tuner_open(void) { return T.open; }
static void cmd_tuner(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    T.open = !T.open;
    if (!T.open) game_fire_loop(FIRE_LOOP_OFF, 1.0f);
}
void fire_tuner_init(void) { cmd_register("fire_tuner", cmd_tuner, "toggle the fire animation tuner (springs, curves, mass, camera; slow-motion loops, graphs, A/B, save)"); }

static bool frow(struct nk_context *ctx, const char *label, float *v, float lo, float hi, float step) {
    float o = *v;
    nk_layout_row_dynamic(ctx, S(20), 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_property_float(ctx, "#", lo, v, hi, step, step * 0.2f);
    return o != *v;
}

// One line graph of a recorded series (scaled to its own range), shots as vertical marks.
static void graph(struct nk_context *ctx, const char *label, const FireRecord *r, int n, int field, struct nk_color col) {
    nk_layout_row_dynamic(ctx, S(14), 1);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, S(70), 1);
    struct nk_rect b;
    if (!nk_widget(&b, ctx) || n < 2) return;
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    nk_fill_rect(cv, b, 0, nk_rgba(14, 16, 20, 255));
    float lo = 0, hi = 0;
    for (int i = 0; i < n; i++) {
        const float v[5] = { r[i].gun_pitch, r[i].gun_back * 100.0f, r[i].hand_pitch, r[i].cam_pitch, r[i].fov };
        float x = field == 2 ? fmaxf(v[0], v[2]) : v[field];
        float y = field == 2 ? fminf(v[0], v[2]) : v[field];
        hi = fmaxf(hi, x);
        lo = fminf(lo, y);
    }
    if (hi - lo < 1e-4f) hi = lo + 1e-4f;
    float y0 = b.y + b.h - (0 - lo) / (hi - lo) * b.h;
    nk_stroke_line(cv, b.x, y0, b.x + b.w, y0, 1, nk_rgba(60, 64, 72, 255));
    for (int i = 0; i < n; i++)
        if (r[i].shot) {
            float x = b.x + b.w * (float)i / (float)(n - 1);
            nk_stroke_line(cv, x, b.y, x, b.y + b.h, 1, nk_rgba(240, 200, 60, 160));   // the shot: audio transient + flash
        }
    for (int pass = 0; pass < (field == 2 ? 2 : 1); pass++) {
        int f = field == 2 ? (pass ? 2 : 0) : field;
        struct nk_color c = field == 2 && !pass ? nk_rgba(230, 140, 60, 200) : col;
        for (int i = 1; i < n; i++) {
            const float a[5] = { r[i - 1].gun_pitch, r[i - 1].gun_back * 100.0f, r[i - 1].hand_pitch, r[i - 1].cam_pitch, r[i - 1].fov };
            const float v[5] = { r[i].gun_pitch, r[i].gun_back * 100.0f, r[i].hand_pitch, r[i].cam_pitch, r[i].fov };
            float x0 = b.x + b.w * (float)(i - 1) / (float)(n - 1), x1 = b.x + b.w * (float)i / (float)(n - 1);
            float ya = b.y + b.h - (a[f] - lo) / (hi - lo) * b.h, yb = b.y + b.h - (v[f] - lo) / (hi - lo) * b.h;
            nk_stroke_line(cv, x0, ya, x1, yb, 1.5f, c);
        }
    }
    char rng[64];
    snprintf(rng, sizeof(rng), "%.2f .. %.2f", lo, hi);
    nk_draw_text(cv, nk_rect(b.x + 4, b.y + 2, b.w, 14), rng, (int)strlen(rng), ctx->style.font, nk_rgba(0, 0, 0, 0), nk_rgba(160, 165, 175, 255));
}

static const char *k_ch[FA_CH] = { "pitch (deg)", "yaw (deg)", "roll (deg)", "kickback (m)", "vertical (m)" };

void fire_tuner_draw(void) {
    if (!T.open || !G) return;
    ItemDB *db = &G->items;
    struct nk_context *ctx = ui_nk();
    if (nk_begin(ctx, "FIRE ANIM TUNER", nk_rect(S(20), S(60), S(460), S(820)),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        static const char *names[MAX_WEAPON_DEFS];
        static int map[MAX_WEAPON_DEFS];
        int n = 0, cur = 0;
        for (int i = 0; i < db->weapon_count; i++) {
            if (db->weapons[i].kind == WPN_MELEE) continue;
            if (i == T.weapon) cur = n;
            map[n] = i;
            names[n++] = db->weapons[i].id;
        }
        if (!n) { nk_end(ctx); return; }
        if (T.weapon < 0) {
            // Start on the equipped gun.
            T.weapon = map[0];
            const WeaponDef *eq = weapon_equipped_def(G->world, game_view_slot());
            for (int i = 0; i < n && eq; i++) if (!strcmp(db->weapons[map[i]].id, eq->id)) { T.weapon = map[i]; cur = i; }
        }
        nk_layout_row_dynamic(ctx, S(24), 2);
        int sel = nk_combo(ctx, names, n, cur, S(20), nk_vec2(S(200), S(260)));
        if (map[sel] != T.weapon) { T.weapon = map[sel]; T.have_stored = false; }
        WeaponDef *wd = &db->weapons[T.weapon];
        FireAnimDef *d = &wd->fire;
        if (nk_button_label(ctx, "Equip")) {
            char buf[96];
            snprintf(buf, sizeof(buf), "give %s\nequip %s\ninfinite_ammo 1\n", wd->id, wd->id);
            cbuf_add(buf);
        }
        // Loops in slow motion.
        static float slow = 0.25f;
        nk_layout_row_dynamic(ctx, S(20), 2);
        nk_label(ctx, "slow motion", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.05f, &slow, 1.0f, 0.05f);
        nk_layout_row_dynamic(ctx, S(24), 4);
        if (nk_button_label(ctx, "Single")) game_fire_loop(FIRE_LOOP_SINGLE, slow);
        if (nk_button_label(ctx, "Burst")) game_fire_loop(FIRE_LOOP_BURST, slow);
        if (nk_button_label(ctx, "Mag dump")) game_fire_loop(FIRE_LOOP_MAGDUMP, slow);
        if (nk_button_label(ctx, "Stop")) game_fire_loop(FIRE_LOOP_OFF, 1.0f);
        // A/B and save.
        nk_layout_row_dynamic(ctx, S(24), 3);
        if (nk_button_label(ctx, T.have_stored ? (T.showing_b ? "A/B: B" : "A/B: A") : "Store A")) {
            if (!T.have_stored) { T.stored = *d; T.have_stored = true; T.showing_b = false; }
            else { FireAnimDef t = *d; *d = T.stored; T.stored = t; T.showing_b = !T.showing_b; }
        }
        if (nk_button_label(ctx, "Save class")) snprintf(T.status, sizeof(T.status), game_fire_save_class(d) ? "saved fire_poses/%s.json" : "save failed (%s)", d->cls_name);
        if (nk_button_label(ctx, "Reload")) { cmd_execute_line("fire_reload"); snprintf(T.status, sizeof(T.status), "reloaded"); }
        nk_layout_row_dynamic(ctx, S(18), 1);
        char info[160];
        snprintf(info, sizeof(info), "class %s  mass %.2f kg  pitch inertia %.3f  (weapon mass / moment: data/weapons)", d->cls_name, d->mass, fire_anim_inertia(d, FA_PITCH));
        nk_label(ctx, info, NK_TEXT_LEFT);
        if (T.status[0]) nk_label(ctx, T.status, NK_TEXT_LEFT);
        // Graphs of the last 4 s.
        static FireRecord rec[240];
        int rn = game_fire_record(rec, 240);
        graph(ctx, "gun pitch (orange) / hand pitch (green): the hands trail the kick", rec, rn, 2, nk_rgba(90, 220, 120, 255));
        graph(ctx, "gun kickback (cm)", rec, rn, 1, nk_rgba(230, 140, 60, 255));
        graph(ctx, "camera pitch (deg): after the gun", rec, rn, 3, nk_rgba(100, 160, 255, 255));
        graph(ctx, "FOV punch (deg)", rec, rn, 4, nk_rgba(220, 220, 220, 255));
        bool ch = false;
        if (nk_tree_push(ctx, NK_TREE_TAB, "Mass", NK_MAXIMIZED)) {
            ch |= frow(ctx, "mass (kg)", &d->mass, 0.2f, 20, 0.05f);
            ch |= frow(ctx, "moment (front-heavy 0..1)", &d->moment, 0, 1, 0.01f);
            ch |= frow(ctx, "first shot x", &d->first_mult, 0.5f, 2, 0.01f);
            ch |= frow(ctx, "hip scale", &d->hip_scale, 0, 3, 0.01f);
            ch |= frow(ctx, "ADS scale", &d->ads_scale, 0, 3, 0.01f);
            nk_tree_pop(ctx);
        }
        for (int c = 0; c < FA_CH; c++) {
            if (nk_tree_push_id(ctx, NK_TREE_TAB, k_ch[c], NK_MINIMIZED, c)) {
                FaChannel *k = &d->ch[c];
                bool m = c >= FA_BACK;
                ch |= frow(ctx, "peak", &k->peak, 0, m ? 0.2f : 30.0f, m ? 0.001f : 0.05f);
                ch |= frow(ctx, "random min", &k->rnd_min, 0, 2, 0.01f);
                ch |= frow(ctx, "random max", &k->rnd_max, 0, 2, 0.01f);
                ch |= frow(ctx, "omega (rad/s)", &k->omega, 1, 60, 0.1f);
                ch |= frow(ctx, "damping ratio", &k->zeta, 0.05f, 1.5f, 0.01f);
                ch |= frow(ctx, "ADS x", &k->ads, 0, 2, 0.01f);
                ch |= frow(ctx, "momentum (follow)", &k->follow, 0, 1, 0.01f);
                ch |= frow(ctx, "random sign", &k->sign_rnd, 0, 1, 0.01f);
                ch |= frow(ctx, "ride per shot", &k->ride, 0, 1.5f, 0.01f);
                ch |= frow(ctx, "ride max", &k->ride_max, 0, 6, 0.05f);
                nk_tree_pop(ctx);
            }
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Curves", NK_MINIMIZED)) {
            for (int c = 0; c < FA_CH; c++) ch |= frow(ctx, k_ch[c], &d->curve_amp[c], 0, 3, 0.01f);
            ch |= frow(ctx, "string gap (s)", &d->settle_time, 0.05f, 2, 0.01f);
            ch |= frow(ctx, "ride release (s)", &d->ride_release, 0.02f, 1, 0.01f);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Hands / body", NK_MINIMIZED)) {
            ch |= frow(ctx, "hand omega", &d->hand_omega, 5, 150, 0.5f);
            ch |= frow(ctx, "hand damping", &d->hand_zeta, 0.1f, 2, 0.01f);
            ch |= frow(ctx, "max glue error (m)", &d->hand_max_err, 0, 0.03f, 0.0005f);
            ch |= frow(ctx, "shoulder push", &d->shoulder_push, 0, 2, 0.01f);
            ch |= frow(ctx, "shoulder omega", &d->shoulder_omega, 2, 80, 0.5f);
            ch |= frow(ctx, "trigger pull", &d->trigger_pull, 0, 1, 0.01f);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Camera / punch", NK_MINIMIZED)) {
            for (int c = 0; c < FA_CH; c++) ch |= frow(ctx, k_ch[c], &d->cam_gain[c], 0, 2, 0.01f);
            float dl = (float)d->cam_delay;
            ch |= frow(ctx, "delay (ticks)", &dl, 0, FA_CAM_DELAY_MAX, 1);
            d->cam_delay = (int)(dl + 0.5f);
            ch |= frow(ctx, "camera omega", &d->cam_omega, 2, 60, 0.5f);
            ch |= frow(ctx, "camera damping", &d->cam_zeta, 0.05f, 1.5f, 0.01f);
            ch |= frow(ctx, "FOV punch (deg)", &d->fov_punch, 0, 8, 0.05f);
            ch |= frow(ctx, "FOV release (s)", &d->fov_release, 0.02f, 0.5f, 0.005f);
            ch |= frow(ctx, "roll twitch (deg)", &d->roll_twitch, 0, 5, 0.05f);
            ch |= frow(ctx, "screen kick (m)", &d->screen_kick, 0, 0.05f, 0.0005f);
            ch |= frow(ctx, "blur ticks", &d->blur_ticks, 0, 4, 1);
            ch |= frow(ctx, "chroma tick", &d->chroma, 0, 1, 0.01f);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Mechanical", NK_MINIMIZED)) {
            ch |= frow(ctx, "slide travel (m)", &d->slide_travel, 0, 0.15f, 0.001f);
            ch |= frow(ctx, "slide time (s)", &d->slide_time, 0.01f, 1, 0.005f);
            ch |= frow(ctx, "bolt / pump delay (s)", &d->bolt_delay, 0, 1, 0.01f);
            ch |= frow(ctx, "wobble omega", &d->wobble_omega, 5, 80, 0.5f);
            ch |= frow(ctx, "wobble gain", &d->wobble_gain, 0, 2, 0.01f);
            ch |= frow(ctx, "heat per shot", &d->heat_per_shot, 0, 0.5f, 0.005f);
            nk_tree_pop(ctx);
        }
        if (ch) fire_anim_resolve(d);
    }
    nk_end(ctx);
}
