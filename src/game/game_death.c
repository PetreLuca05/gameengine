// First-person death experience (docs/REVIVE.md "Death sequence"): drives the pure timeline in
// gameplay/revive/death_seq.c from the local player's state and the ragdoll impact, and feeds the
// post stack (desaturate / vignette / blur / exposure / black cut + TAA reset), the mixer (world
// low-pass + duck, ear ring, heartbeat) and the HUD (hidden during the fall and the black).
// Purely local and cosmetic: the host never waits for it, spectating starts on its own timer.
#include "game_internal.h"
#include "game_revive.h"
#include "gameplay/revive/death_seq.h"
#include "gameplay/health.h"
#include "gameplay/characters.h"
#include "player/spectator.h"
#include "render/post/post_settings.h"
#include "render/render.h"
#include "audio/audio.h"
#include "input/input.h"
#include "physics/physics.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define G (g_engine.game)

static struct {
    DeathSeqParams P;
    DeathSeq S;
    DeathFx fx;
    World *world;
    bool was_dead;
    bool attack_prev;
    int preview_stage;          // death_preview (0 = off)
    float preview_t;
    int die_frames;             // die <height>: frames until the kill (the ragdoll spawns in the air)
    double death_time;          // real time of the death (impact debug)
    u64 cut_frame;
    Cvar *impact_debug, *enable;
} D;

static void load_params(void) {
    death_params_defaults(&D.P);
    const char *files[2] = { "assets/vfx/death_sequence.json", "assets/audio/death_ring.json" };
    for (int k = 0; k < 2; k++) {
        size_t n = 0;
        char *text = platform_read_file(files[k], &n);
        char err[128] = { 0 };
        bool ok = text && (k == 0 ? death_params_parse_visual(&D.P, text, err, sizeof(err)) : death_params_parse_ring(&D.P, text, err, sizeof(err)));
        if (!ok) log_warn("death: %s %s, defaults", files[k], text ? err : "missing");
        free(text);
    }
}

static bool dev_or_skippable(void) {
    Cvar *cheats = cvar_find("sv_cheats");
    return revive_rules()->death_skippable || (cheats && cheats->i);
}

static void kill_local(void) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!w || !p || !w->authority) return;
    apply_damage_ex(w, p->ent, 100000.0f, DMG_GENERIC, p->ent, v3_add(p->sim.move.pos, v3(0, 1.0f, 0)), v3(0, -1, 0), -1, 0.0f, DMGF_KILL);
}

void game_death_frame(float dt) {
    World *w = G ? G->world : NULL;
    if (w != D.world) {
        D.world = w;
        memset(&D.S, 0, sizeof(D.S));
        D.was_dead = false;
    }
    Player *p = game_local_player();
    if (!w || !p) {
        D.fx = deathfx_neutral(&D.P);
        return;
    }
    if (D.die_frames > 0 && --D.die_frames == 0) kill_local();
    bool dead = p->sim.move.mode == MOVE_DEAD || (w->flags[ENT_INDEX(p->ent)] & EF_DEAD);
    bool downed = p->sim.down.downed && !dead;
    if (!D.enable->i) { dead = false; downed = false; }
    if (dead && !D.was_dead) {
        deathseq_start(&D.S);
        D.death_time = g_engine.real_time;
        if (D.impact_debug->i) log_info("death_impact: death at frame %llu", (unsigned long long)g_engine.frame_index);
    } else if (!dead && D.was_dead) {
        deathseq_reset(&D.S);   // respawn / revive: neutral at once
    }
    D.was_dead = dead;
    // Stage 2 trigger: the ragdoll's head or torso reaching the ground.
    if (D.S.stage == DS_FALL) {
        vec3 pos[2];
        float speed[2], clear[2];
        if (chars_ragdoll_probe(w, p->ent, pos, speed, clear)) {
            // The cut comes when the HEAD hits the ground (its clearance / its own velocity drop), not the chest.
            bool contact = clear[0] < D.P.impact_contact_dist;
            ImpactReason why = death_impact_detect(&D.S, &D.P, speed[0], speed[0], contact);
            if (why != IMPACT_NONE && D.impact_debug->i)
                log_info("death_impact: %s at %.3f s (head %.2f m/s clear %.3f m, chest %.2f m/s clear %.3f m)", why == IMPACT_CONTACT ? "contact" : "velocity drop",
                         D.S.t, speed[0], clear[0], speed[1], clear[1]);
        }
    }
    // Skippable (rule; always in dev): an attack press jumps to the fade-in.
    bool attack = input_down(ACT_ATTACK);
    if (dead && attack && !D.attack_prev && (D.S.stage == DS_FALL || D.S.stage == DS_BLACK) && dev_or_skippable()) deathseq_skip(&D.S);
    D.attack_prev = attack;
    u8 stage_before = D.S.stage;
    deathseq_step(&D.S, &D.P, dt, downed);
    if (stage_before == DS_FALL && D.S.stage == DS_BLACK && D.impact_debug->i)
        log_info("death_impact: timeout at %.3f s (no ground contact)", D.S.impact_t);
    D.fx = D.preview_stage ? deathseq_preview(&D.P, D.preview_stage, D.preview_t) : deathseq_eval(&D.S, &D.P);
    if (D.fx.camera_cut) {
        render_camera_cut();   // the cut (and the switch under the black): no TAA / exposure history
        D.cut_frame = g_engine.frame_index;
        if (D.impact_debug->i) log_info("death_cut: frame %llu stage %s black %.2f (%.3f s after death)", (unsigned long long)g_engine.frame_index,
                                        death_stage_name((DeathStage)D.S.stage), D.fx.black, g_engine.real_time - D.death_time);
    }
    deathseq_frame_done(&D.S);
    AudioDeathMix m = { D.fx.lpf_hz, D.fx.world_gain, D.fx.ring_gain, D.P.ring_freq, D.P.warble_hz, D.P.warble_depth, D.fx.heart_gain, D.fx.heart_bpm };
    audio_set_death_mix(&m);
}

void game_death_post(PostSettings *s) {
    const DeathFx *f = &D.fx;
    s->fb_desaturate = fmaxf(s->fb_desaturate, f->desat);
    s->contrast = fmaxf(0.3f, s->contrast + f->contrast);
    s->vignette += f->vignette;
    s->fb_blur = f->blur;
    s->exposure_comp += f->exposure;
    s->fb_black = f->black;
}

bool game_death_hud_hidden(void) { return D.fx.hud_hidden || D.fx.black > 0.5f; }

const DeathFx *game_death_fx(void) { return &D.fx; }
int game_death_stage(void) { return D.S.stage; }

// ------------------------------------------------------------------ commands

static void cmd_die(int argc, const char **argv) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!w || !p || !w->authority) { log_info("die: host only"); return; }
    float h = argc > 1 ? (float)atof(argv[1]) : 0.0f;
    if (h > 0.0f) {
        // Up in the air first; the kill follows once the character pose is there, so the ragdoll
        // falls from that height and the impact timing is visible.
        p->sim.move.pos.y += h;
        p->sim.move.vel = v3(0, 0, 0);
        if (p->character) { phys_character_set_pos(p->character, p->sim.move.pos); phys_character_set_vel(p->character, v3(0, 0, 0)); }
        D.die_frames = 2;
    } else {
        kill_local();
    }
}

static void cmd_death_preview(int argc, const char **argv) {
    D.preview_stage = argc > 1 ? atoi(argv[1]) : 0;
    D.preview_t = argc > 2 ? (float)atof(argv[2]) : 1.0f;
    if (D.preview_stage < 0 || D.preview_stage > 3) D.preview_stage = 0;
    log_info("death_preview: %s", D.preview_stage ? (D.preview_stage == 1 ? "stage 1 (fall)" : D.preview_stage == 2 ? "stage 2 (black / fade)" : "downed") : "off");
}

static void cmd_death_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    load_params();
    log_info("death: parameters reloaded");
}

static void cmd_death_dump(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const DeathFx *f = &D.fx;
    const AudioDeathMix *m = audio_death_mix();
    log_info("death_dump: frame %llu stage %s t %.3f impact %d desat %.3f black %.2f blur %.2f vignette %.2f exposure %.2f lpf %.0f world %.3f ring %.3f heart %.2f cut_frame %llu mix_lpf %.0f mix_ring %.3f",
             (unsigned long long)g_engine.frame_index, death_stage_name((DeathStage)D.S.stage), D.S.t, D.S.impact_reason, f->desat, f->black, f->blur, f->vignette,
             f->exposure, f->lpf_hz, f->world_gain, f->ring_gain, f->heart_gain, (unsigned long long)D.cut_frame, m ? m->lpf_hz : 0.0f, m ? m->ring_gain : 0.0f);
}

void game_death_register(void) {
    load_params();
    D.fx = deathfx_neutral(&D.P);
    D.impact_debug = cvar_reg_bool("death_impact_debug", false, CVAR_LOCKED, "log the ragdoll contact that triggers the death cut and the time since death");
    D.enable = cvar_reg_bool("death_sequence", true, CVAR_LOCKED, "first-person death sequence (desaturate, ear ring, black cut on impact)");
    cmd_register("die", cmd_die, "die [fall_height]: kill the local player with the death sequence (host; a height drops the ragdoll from there)");
    cmd_register("death_preview", cmd_death_preview, "death_preview <1 fall|2 black|3 downed> <0..1>: scrub the death sequence without dying (0 = off)");
    cmd_register("death_reload", cmd_death_reload, "reload assets/vfx/death_sequence.json and assets/audio/death_ring.json");
    cmd_register("death_dump", cmd_death_dump, "log the death sequence stage, post and mix values (tests)");
}
