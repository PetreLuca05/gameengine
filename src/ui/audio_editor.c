// Procedural audio tools (audio_editor, audio_env_preview; docs/AUDIO_PROCEDURAL.md "Recipe editor").
#include "audio_editor.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "audio/audio.h"
#include "audio/mixer.h"
#include "audio/synth/syn_material.h"
#include "audio/env/env_model.h"
#include "nuklear_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RENDER_FRAMES (48000 * 3 / 2)   // 1.5 s offline render
#define SPEC_COLS 128
#define SPEC_BINS 48
#define FFT_N 512

static struct {
    bool open, env_open;
    int recipe;
    float inputs[RCP_MAX_INPUTS];
    int inputs_for;                      // recipe the inputs belong to
    int seed;
    bool random_seed, flat;
    char sample[40];
    // live patch edits: the layer whose nodes are shown
    int layer;
    SynPatch work;
    int work_for;                        // library patch index of `work`
    char status[160];
    // offline render
    Mixer *mix;
    float *wave;
    int wave_n;
    float spec[SPEC_COLS][SPEC_BINS];
    bool have_render;
    // environment preview
    float dist, azim, facing, thick, detour, room[3], reflector, humidity, temp, rain, wind;
    int walls, wall_mat, room_mat, refl_mat;
    bool indoor;
    float out_gain_db, out_lp, out_band[3], out_delay, out_rt60, out_echo;
} E = { .recipe = -1, .inputs_for = -1, .seed = 1, .work_for = -1, .dist = 30, .thick = 0.2f, .room = { 8, 3, 6 }, .humidity = 60, .temp = 15,
        .wall_mat = SYN_MAT_CONCRETE, .room_mat = SYN_MAT_PLASTER, .refl_mat = SYN_MAT_CONCRETE };

static float S(float v) { return v * ui_scale(); }

bool audio_editor_open(void) { return E.open || E.env_open; }
static void cmd_editor(int argc, const char **argv) { (void)argc; (void)argv; E.open = !E.open; }
static void cmd_env(int argc, const char **argv) { (void)argc; (void)argv; E.env_open = !E.env_open; }

void audio_editor_init(void) {
    cmd_register("audio_editor", cmd_editor, "toggle the procedural audio recipe editor (inputs, trigger, live patch values, waveform / spectrogram, A/B, save)");
    cmd_register("audio_env_preview", cmd_env, "toggle the environment chain preview (distance, walls, room, reflector, weather sliders; plays a recipe through them)");
}

// ------------------------------------------------------------------ helpers

static const SynRecipe *cur_recipe(SynLibrary *lib) {
    if (E.recipe < 0 || E.recipe >= lib->nrecipes) {
        E.recipe = syn_lib_find_recipe(lib, "gunshot");
        if (E.recipe < 0) E.recipe = 0;
    }
    if (E.recipe >= lib->nrecipes) return NULL;
    const SynRecipe *r = lib->recipe[E.recipe];
    if (E.inputs_for != E.recipe) {
        for (int i = 0; i < r->ninputs; i++) E.inputs[i] = r->input_def[i];
        E.inputs_for = E.recipe;
        E.layer = 0;
        E.work_for = -1;
        E.have_render = false;
    }
    return r;
}

static void params_of(const SynRecipe *r, AudioParams *p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < r->ninputs && p->n < AUDIO_MAX_PARAMS; i++) audio_param(p, r->input_name[i], E.inputs[i]);
}

static u32 next_seed(void) {
    if (E.random_seed) E.seed = (int)(dg_hash_u32((u32)(platform_time() * 1000.0)) & 0xFFFFu);
    return (u32)E.seed;
}

static void play(const SynRecipe *r, const EnvParams *env, vec3 pos, float delay) {
    AudioParams p;
    params_of(r, &p);
    AudioEmit e;
    audio_emit_init(&e, r->name, pos);
    e.seed = next_seed();
    e.flat = E.flat && !env;
    e.sample = E.sample[0] ? E.sample : NULL;
    e.env = env;
    if (env) e.delay = delay;
    AudioVoice v = audio_emit(&e, &p);
    snprintf(E.status, sizeof(E.status), "%s: %s (seed %d)", r->name, v ? "playing" : "no voice", E.seed);
}

static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -6.2831853f / (float)len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = cosf(ang * (float)k), wi = sinf(ang * (float)k);
                float *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
                float vr = *br * wr - *bi * wi, vi = *br * wi + *bi * wr;
                *br = *ar - vr, *bi = *ai - vi;
                *ar += vr, *ai += vi;
            }
    }
}

// Offline render of the recipe as configured (2D, no environment): the waveform and its spectrogram.
static void render_offline(SynLibrary *lib, const SynRecipe *r) {
    if (!E.mix) E.mix = mixer_create(48000.0f, platform_time);
    if (!E.wave) E.wave = (float *)malloc(sizeof(float) * RENDER_FRAMES);
    if (!E.mix || !E.wave) return;
    SynRecipeOut o;
    syn_recipe_eval(lib, r, E.inputs, (u32)E.seed, E.sample[0] ? E.sample : NULL, &o);
    MixStart s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < o.nlayers && s.nlayers < RCP_MAX_LAYERS; i++) {
        if (!o.layer[i].on || !o.layer[i].patch) continue;
        MixLayerStart *l = &s.layer[s.nlayers++];
        l->patch = o.layer[i].patch;
        l->gain = o.layer[i].gain;
        memcpy(l->params, o.layer[i].params, sizeof(l->params));
    }
    s.seed = (u32)E.seed;
    s.volume = 1.0f;
    s.score = 1.0f;
    env_params_neutral(&s.env_params);
    MixHandle h = mixer_start(E.mix, &s);
    static float st[2 * 1024];
    for (int done = 0; done < RENDER_FRAMES;) {
        int n = RENDER_FRAMES - done < 1024 ? RENDER_FRAMES - done : 1024;
        mixer_render(E.mix, st, n);
        for (int i = 0; i < n; i++) E.wave[done + i] = st[i * 2];
        done += n;
    }
    mixer_stop(E.mix, h, 0.005f);
    mixer_render(E.mix, st, 1024);
    mixer_update(E.mix);
    E.wave_n = RENDER_FRAMES;
    // Spectrogram: SPEC_COLS frames of FFT_N, log-spaced bins 40 Hz .. 20 kHz, dB.
    static float re[FFT_N], im[FFT_N];
    for (int c = 0; c < SPEC_COLS; c++) {
        int start = c * (RENDER_FRAMES - FFT_N) / SPEC_COLS;
        for (int i = 0; i < FFT_N; i++) {
            re[i] = E.wave[start + i] * (0.5f - 0.5f * cosf(6.2831853f * (float)i / (float)(FFT_N - 1)));
            im[i] = 0;
        }
        fft(re, im, FFT_N);
        for (int b = 0; b < SPEC_BINS; b++) {
            float f0 = 40.0f * powf(500.0f, (float)b / SPEC_BINS), f1 = 40.0f * powf(500.0f, (float)(b + 1) / SPEC_BINS);
            int k0 = (int)(f0 / 48000.0f * FFT_N), k1 = (int)(f1 / 48000.0f * FFT_N);
            if (k1 <= k0) k1 = k0 + 1;
            float e = 0;
            for (int k = k0; k < k1 && k < FFT_N / 2; k++) e += re[k] * re[k] + im[k] * im[k];
            E.spec[c][b] = 10.0f * log10f(e / (float)(k1 - k0) + 1e-12f);
        }
    }
    E.have_render = true;
    float pk = 0, rms = 0;
    for (int i = 0; i < RENDER_FRAMES; i++) { pk = fmaxf(pk, fabsf(E.wave[i])); rms += E.wave[i] * E.wave[i]; }
    snprintf(E.status, sizeof(E.status), "rendered %s: peak %.3f, rms %.4f", r->name, pk, sqrtf(rms / RENDER_FRAMES));
}

static void draw_render(struct nk_context *ctx) {
    if (!E.have_render) return;
    struct nk_rect b;
    nk_layout_row_dynamic(ctx, S(80), 1);
    if (nk_widget(&b, ctx)) {
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        nk_fill_rect(cv, b, 0, nk_rgba(14, 16, 20, 255));
        int cols = (int)b.w;
        float mid = b.y + b.h * 0.5f;
        for (int x = 0; x < cols; x++) {
            int a = x * E.wave_n / cols, z = (x + 1) * E.wave_n / cols;
            float lo = 0, hi = 0;
            for (int i = a; i < z; i++) { lo = fminf(lo, E.wave[i]); hi = fmaxf(hi, E.wave[i]); }
            nk_stroke_line(cv, b.x + (float)x, mid - hi * b.h * 0.5f, b.x + (float)x, mid - lo * b.h * 0.5f, 1, nk_rgba(120, 200, 255, 255));
        }
    }
    nk_layout_row_dynamic(ctx, S(110), 1);
    if (nk_widget(&b, ctx)) {
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        float cw = b.w / SPEC_COLS, ch = b.h / SPEC_BINS;
        for (int c = 0; c < SPEC_COLS; c++)
            for (int k = 0; k < SPEC_BINS; k++) {
                float t = (E.spec[c][k] + 90.0f) / 70.0f;
                t = t < 0 ? 0 : (t > 1 ? 1 : t);
                struct nk_color col = nk_rgba((nk_byte)(255 * t), (nk_byte)(255 * t * t * 0.8f), (nk_byte)(80 + 120 * (1 - t) * t), 255);
                nk_fill_rect(cv, nk_rect(b.x + (float)c * cw, b.y + b.h - (float)(k + 1) * ch, cw + 0.5f, ch + 0.5f), 0, col);
            }
    }
}

static bool save_patch(const SynPatch *p) {
    char rel[160], path[600];
    snprintf(rel, sizeof(rel), "assets/audio/patches/%s.json", p->name);
#ifdef DG_SOURCE_DIR
    snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, rel);
#else
    platform_data_path(rel, path, sizeof(path));
#endif
    char *txt = syn_patch_write(p, NULL, 0);
    if (!txt) return false;
    bool ok = platform_write_file(path, txt, strlen(txt));
    // Runtime copy too (a reload keeps the edit when running from the build).
    char rt[600];
    snprintf(rt, sizeof(rt), "%s%s", platform_base_dir(), rel);
    if (ok && strcmp(rt, path)) platform_write_file(rt, txt, strlen(txt));
    free(txt);
    snprintf(E.status, sizeof(E.status), "%s %s", ok ? "saved" : "cannot write", path);
    log_info("audio_editor: %s", E.status);
    return ok;
}

// The selected layer's patch: every node's constant values (and the patch parameter defaults),
// applied live (new voices use the edited copy).
static void patch_rows(struct nk_context *ctx, SynLibrary *lib, const SynRecipe *r) {
    if (E.layer >= r->nlayers) E.layer = 0;
    const SynLayer *ly = &r->layer[E.layer];
    if (ly->dyn || ly->patch < 0) { nk_layout_row_dynamic(ctx, S(18), 1); nk_label(ctx, "(sample layer: the event's sample)", NK_TEXT_LEFT); return; }
    if (E.work_for != ly->patch) { E.work = *lib->patch[ly->patch]; E.work_for = ly->patch; }
    SynPatch *p = &E.work;
    if (!strncmp(p->name, "sample:", 7)) { nk_layout_row_dynamic(ctx, S(18), 1); nk_label(ctx, p->name, NK_TEXT_LEFT); return; }
    bool changed = false;
    char lbl[64];
    for (int i = 0; i < p->nparams; i++) {
        nk_layout_row_dynamic(ctx, S(20), 2);
        snprintf(lbl, sizeof(lbl), "$%s", p->param_name[i]);
        nk_label(ctx, lbl, NK_TEXT_LEFT);
        float o = p->param_def[i], lo = p->param_lo[i] > -1e8f ? p->param_lo[i] : -1e5f, hi = p->param_hi[i] < 1e8f ? p->param_hi[i] : 1e5f;
        nk_property_float(ctx, "#", lo, &p->param_def[i], hi, fmaxf(fabsf(o) * 0.02f, 0.001f), fmaxf(fabsf(o) * 0.005f, 0.0005f));
        changed |= o != p->param_def[i];
    }
    for (int n = 0; n < p->nnodes; n++) {
        SynNodeDef *d = &p->nodes[n];
        const SynNodeInfo *info = syn_node_info(d->type);
        nk_layout_row_dynamic(ctx, S(16), 1);
        snprintf(lbl, sizeof(lbl), "%s (%s)", d->id, info->name);
        nk_label(ctx, lbl, NK_TEXT_LEFT);
        for (int k = 0; k < SYN_NODE_PARAMS; k++) {
            if (!info->param[k] || d->p[k].kind != SYN_SRC_CONST) continue;
            nk_layout_row_dynamic(ctx, S(20), 2);
            snprintf(lbl, sizeof(lbl), "   %s", info->param[k]);
            nk_label(ctx, lbl, NK_TEXT_LEFT);
            float o = d->p[k].value;
            nk_property_float(ctx, "#", -1e5f, &d->p[k].value, 1e5f, fmaxf(fabsf(o) * 0.02f, 0.001f), fmaxf(fabsf(o) * 0.005f, 0.0005f));
            changed |= o != d->p[k].value;
        }
    }
    if (changed) {
        char err[160];
        if (syn_lib_put_patch(lib, p, err, sizeof(err)) < 0) snprintf(E.status, sizeof(E.status), "%s", err);
        else snprintf(E.status, sizeof(E.status), "%s: edited (not saved)", p->name);
    }
    nk_layout_row_dynamic(ctx, S(24), 2);
    if (nk_button_label(ctx, "Save patch")) save_patch(p);
    if (nk_button_label(ctx, "Revert (reload)")) { audio_reload(); E.work_for = -1; }
}

static void recipe_editor(struct nk_context *ctx, SynLibrary *lib) {
    const SynRecipe *r = cur_recipe(lib);
    if (!r) return;
    static const char *names[SYN_LIB_RECIPES];
    int n = 0;
    for (int i = 0; i < lib->nrecipes; i++) names[n++] = lib->recipe[i]->name;
    nk_layout_row_dynamic(ctx, S(24), 1);
    E.recipe = nk_combo(ctx, names, n, E.recipe, S(20), nk_vec2(S(260), S(360)));
    r = cur_recipe(lib);
    nk_layout_row_dynamic(ctx, S(16), 1);
    nk_label(ctx, "event inputs", NK_TEXT_LEFT);
    for (int i = 0; i < r->ninputs; i++) {
        if (!strcmp(r->input_name[i], "seed01") || !strcmp(r->input_name[i], "distance")) continue;   // filled at emit
        nk_layout_row_dynamic(ctx, S(20), 2);
        nk_label(ctx, r->input_name[i], NK_TEXT_LEFT);
        float def = r->input_def[i], hi = fmaxf(1.0f, fabsf(def) * 4.0f);
        if (!strcmp(r->input_name[i], "material")) hi = (float)(SYN_MAT_BUILTIN_COUNT - 1);
        if (!strcmp(r->input_name[i], "distance")) hi = 2000.0f;
        nk_property_float(ctx, "#", 0.0f, &E.inputs[i], hi, hi / 100.0f, hi / 400.0f);
    }
    nk_layout_row_dynamic(ctx, S(22), 3);
    nk_property_int(ctx, "seed", 0, &E.seed, 65535, 1, 1);
    nk_checkbox_label(ctx, "random", &E.random_seed);
    nk_checkbox_label(ctx, "2D", &E.flat);
    nk_layout_row_dynamic(ctx, S(22), 2);
    nk_label(ctx, "sample (@ layers / A-B)", NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, E.sample, sizeof(E.sample), nk_filter_default);
    nk_layout_row_dynamic(ctx, S(26), 4);
    if (nk_button_label(ctx, "Play")) {
        vec3 fwd = g_engine.game ? audio_listener_pos() : v3(0, 0, 0);
        play(r, NULL, v3_add(fwd, v3(0, 0, -4)), -1);
    }
    if (nk_button_label(ctx, "Render")) render_offline(lib, r);
    if (nk_button_label(ctx, "A: sample") && E.sample[0]) audio_play_2d(E.sample, 1.0f, SND_SFX);
    if (nk_button_label(ctx, "B: recipe")) { bool f = E.flat; E.flat = true; play(r, NULL, audio_listener_pos(), -1); E.flat = f; }
    draw_render(ctx);
    nk_layout_row_dynamic(ctx, S(18), 1);
    nk_label(ctx, E.status, NK_TEXT_LEFT);
    // Layers.
    static const char *ln[RCP_MAX_LAYERS];
    static char lnb[RCP_MAX_LAYERS][48];
    for (int i = 0; i < r->nlayers; i++) {
        const SynLayer *l = &r->layer[i];
        snprintf(lnb[i], sizeof(lnb[i]), "%d: %s%s", i, l->dyn ? "@sample" : (l->patch >= 0 ? lib->patch[l->patch]->name : "?"), l->optional ? " (optional)" : "");
        ln[i] = lnb[i];
    }
    nk_layout_row_dynamic(ctx, S(24), 1);
    int sel = nk_combo(ctx, ln, r->nlayers, E.layer, S(20), nk_vec2(S(260), S(200)));
    if (sel != E.layer) { E.layer = sel; E.work_for = -1; }
    patch_rows(ctx, lib, r);
}

// ------------------------------------------------------------------ environment preview

static void env_preview(struct nk_context *ctx, SynLibrary *lib) {
    const SynRecipe *r = cur_recipe(lib);
    if (!r) return;
    static const char *names[SYN_LIB_RECIPES];
    for (int i = 0; i < lib->nrecipes; i++) names[i] = lib->recipe[i]->name;
    nk_layout_row_dynamic(ctx, S(24), 1);
    E.recipe = nk_combo(ctx, names, lib->nrecipes, E.recipe, S(20), nk_vec2(S(260), S(360)));
    r = cur_recipe(lib);
    static const char *mats[SYN_MAT_MAX];
    const SynMaterialTable *mt = syn_material_table();
    for (int i = 0; i < mt->count; i++) mats[i] = mt->m[i].name;
    char vl[64];
#define ROW(label, var, lo, hi, step) do { nk_layout_row_dynamic(ctx, S(20), 2); snprintf(vl, sizeof(vl), "%s  %.1f", label, (double)(var)); nk_label(ctx, vl, NK_TEXT_LEFT); nk_slider_float(ctx, lo, &(var), hi, step); } while (0)
    ROW("distance m", E.dist, 1.0f, 2000.0f, 1.0f);
    ROW("azimuth deg", E.azim, -180.0f, 180.0f, 1.0f);
    ROW("source facing deg (0 = at you)", E.facing, 0.0f, 180.0f, 1.0f);
    nk_layout_row_dynamic(ctx, S(22), 3);
    nk_property_int(ctx, "walls", 0, &E.walls, 3, 1, 1);
    E.wall_mat = nk_combo(ctx, mats, mt->count, E.wall_mat, S(20), nk_vec2(S(200), S(300)));
    nk_property_float(ctx, "thick", 0.005f, &E.thick, 1.0f, 0.01f, 0.005f);
    ROW("diffraction detour m (0 none)", E.detour, 0.0f, 6.0f, 0.05f);
    nk_layout_row_dynamic(ctx, S(22), 2);
    nk_checkbox_label(ctx, "indoor room", &E.indoor);
    E.room_mat = nk_combo(ctx, mats, mt->count, E.room_mat, S(20), nk_vec2(S(200), S(300)));
    nk_layout_row_dynamic(ctx, S(22), 3);
    nk_property_float(ctx, "W", 1.5f, &E.room[0], 60.0f, 0.5f, 0.1f);
    nk_property_float(ctx, "H", 2.0f, &E.room[1], 20.0f, 0.5f, 0.1f);
    nk_property_float(ctx, "L", 1.5f, &E.room[2], 80.0f, 0.5f, 0.1f);
    nk_layout_row_dynamic(ctx, S(22), 2);
    nk_property_float(ctx, "reflector m behind you (0 none)", 0.0f, &E.reflector, 600.0f, 5.0f, 1.0f);
    E.refl_mat = nk_combo(ctx, mats, mt->count, E.refl_mat, S(20), nk_vec2(S(200), S(300)));
    ROW("humidity %", E.humidity, 5.0f, 100.0f, 1.0f);
    ROW("temperature C", E.temp, -20.0f, 40.0f, 1.0f);
    ROW("rain", E.rain, 0.0f, 1.0f, 0.01f);
    ROW("wind m/s", E.wind, 0.0f, 25.0f, 0.5f);
#undef ROW
    // The chain for these values (listener at the origin facing -Z), as the world would set it.
    float az = E.azim * 0.0174533f, d = E.dist;
    vec3 L = v3(0, 0, 0), right = v3(1, 0, 0), fwd = v3(0, 0, -1);
    vec3 Sp = v3(sinf(az) * d, 0, -cosf(az) * d);
    EnvParams ep;
    env_params_neutral(&ep);
    ep.spread = 0;
    float c = env_sound_speed(E.temp);
    float g = env_distance_gain(d, r->min_dist, r->max_dist, r->rolloff);
    float band[ENV_BANDS], lp;
    env_air(d, E.temp, E.humidity, band, &lp);
    float ddb, dlp;
    env_directivity(cosf(E.facing * 0.0174533f), r->front_db, r->rear_db, r->rear_lp, &ddb, &dlp);
    g *= syn_db(ddb);
    lp = fminf(lp, dlp);
    if (E.walls > 0) {
        EnvWall w[3] = { { E.wall_mat, E.thick }, { E.wall_mat, E.thick }, { E.wall_mat, E.thick } };
        float tl[ENV_BANDS];
        env_transmission(w, E.walls, tl);
        if (E.detour > 0) {
            float df[ENV_BANDS];
            env_diffraction(E.detour, df);
            for (int b = 0; b < ENV_BANDS; b++) tl[b] = 10.0f * log10f(powf(10.0f, tl[b] / 10.0f) + powf(10.0f, df[b] / 10.0f));
        }
        for (int b = 0; b < ENV_BANDS; b++) band[b] += tl[b];
    }
    EnvWeather wx = { E.temp, E.humidity, E.rain, E.wind, 0, 1 };
    g *= env_masking_gain(g * syn_db(band[1]), env_noise_floor(&wx));
    ep.gain = g;
    ep.lp_hz = lp;
    for (int b = 0; b < ENV_BANDS; b++) ep.band_db[b] = band[b];
    env_pan(L, right, fwd, Sp, &ep.pan, &ep.front);
    EnvReverbParams rv;
    memset(&rv, 0, sizeof(rv));
    E.out_rt60 = 0;
    if (E.indoor) {
        // A box room around the listener; the source inside it along the azimuth (clamped).
        EnvRoom room;
        memset(&room, 0, sizeof(room));
        room.indoor = true;
        room.enclosed = 1;
        room.mn = v3(-E.room[0] * 0.5f, -1.6f, -E.room[2] * 0.5f);
        room.mx = v3(E.room[0] * 0.5f, E.room[1] - 1.6f, E.room[2] * 0.5f);
        room.dims = v3(E.room[0], E.room[1], E.room[2]);
        room.volume = E.room[0] * E.room[1] * E.room[2];
        room.surface = 2.0f * (E.room[0] * E.room[1] + E.room[0] * E.room[2] + E.room[1] * E.room[2]);
        const SynMaterial *m = syn_material(E.room_mat);
        for (int i = 0; i < 6; i++) room.wall_material[i] = E.room_mat;
        for (int b = 0; b < ENV_BANDS; b++) {
            float a = fminf(m->absorb[b] + 0.05f, 0.99f);
            room.absorb[b] = m->absorb[b];
            room.rt60[b] = fminf(0.161f * room.volume / (-room.surface * logf(1.0f - a)), 4.0f);
        }
        vec3 Si = v3_max(v3_min(Sp, v3_sub(room.mx, v3s(0.3f))), v3_add(room.mn, v3s(0.3f)));
        ep.ntaps = env_room_taps(&room, Si, L, right, c, ep.tap, ENV_MAX_TAPS);
        env_room_reverb(&room, &rv);
        ep.send[0] = r->reverb * 0.3f * fminf(1.0f / fmaxf(g, 1e-3f), 6.0f);
        audio_preview_reverb(0, &rv, 6.0f);
        E.out_rt60 = room.rt60[1];
    } else {
        env_outdoor_reverb(0, 0, 0, &rv);
        ep.send[2] = r->reverb * 0.24f * fminf(1.0f / fmaxf(g, 1e-3f), 6.0f);
    }
    E.out_gain_db = syn_todb(g);
    E.out_lp = lp;
    for (int b = 0; b < ENV_BANDS; b++) E.out_band[b] = band[b];
    E.out_delay = d / c;
    // An outdoor reflector behind the listener: one discrete echo along S -> P -> L.
    E.out_echo = 0;
    vec3 P = v3(-sinf(az) * E.reflector, 0, cosf(az) * E.reflector);
    float sp = v3_dist(Sp, P), path = sp + E.reflector;
    if (E.reflector > 0 && !E.indoor) E.out_echo = (path - d) / c;
    nk_layout_row_dynamic(ctx, S(18), 1);
    char line[200];
    snprintf(line, sizeof(line), "gain %+.1f dB  lp %.0f Hz  bands %.1f / %.1f / %.1f dB  delay %.0f ms", E.out_gain_db, E.out_lp, E.out_band[0], E.out_band[1], E.out_band[2], E.out_delay * 1000.0f);
    nk_label(ctx, line, NK_TEXT_LEFT);
    snprintf(line, sizeof(line), "%s  taps %d  RT60 %.2f s  echo %s%.0f ms", E.indoor ? "indoor" : "outdoor", ep.ntaps, E.out_rt60, E.out_echo > 0 ? "+" : "", E.out_echo * 1000.0f);
    nk_label(ctx, line, NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, S(26), 1);
    if (nk_button_label(ctx, "Play through this environment")) {
        // Placed for real around the listener (the chain's parameters are fixed: the world does not move them).
        vec3 lp3 = audio_listener_pos();
        play(r, &ep, v3_add(lp3, Sp), E.out_delay);
        if (E.out_echo > 0) {
            EnvParams ee;
            env_params_neutral(&ee);
            ee.spread = 0.25f;
            float eb[ENV_BANDS], elp;
            env_air(path, E.temp, E.humidity, eb, &elp);
            ee.gain = env_distance_gain(path, r->min_dist, r->max_dist, r->rolloff) * syn_material(E.refl_mat)->reflect;
            ee.lp_hz = elp;
            for (int b = 0; b < ENV_BANDS; b++) ee.band_db[b] = eb[b];
            env_pan(L, right, fwd, P, &ee.pan, &ee.front);
            play(r, &ee, v3_add(lp3, P), E.out_delay + E.out_echo);
        }
    }
    nk_layout_row_dynamic(ctx, S(18), 1);
    nk_label(ctx, E.status, NK_TEXT_LEFT);
}

void audio_editor_draw(void) {
    if (!E.open && !E.env_open) return;
    SynLibrary *lib = audio_library();
    if (!lib || !lib->nrecipes) return;
    struct nk_context *ctx = ui_nk();
    if (E.open) {
        if (nk_begin(ctx, "AUDIO RECIPE EDITOR", nk_rect(S(20), S(60), S(520), S(860)),
                     NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            recipe_editor(ctx, lib);
        else E.open = false;
        nk_end(ctx);
    }
    if (E.env_open) {
        if (nk_begin(ctx, "AUDIO ENV PREVIEW", nk_rect(S(560), S(60), S(480), S(640)),
                     NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            env_preview(ctx, lib);
        else E.env_open = false;
        nk_end(ctx);
    }
}
