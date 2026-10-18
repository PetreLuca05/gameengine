// Game side of the post-processing stack (src/render/post): every frame the final PostSettings
// are blended from
//   1. the Graphics settings (r_* cvars, quality presets Low / Medium / High / Ultra),
//   2. the level's post volumes (data/levels/<map>.json: "post" = global grade, "post_volumes" =
//      boxes with a blend distance and a priority; higher priorities blend over lower ones),
//   3. gameplay feedback (damage, low health, flash, hit markers, sprint, underwater, night
//      vision, death), scaled by r_post_feedback,
//   4. photo mode overrides,
// and handed to the renderer with render_post_settings. Also: the debug / profiling commands
// (post_bench, post_volumes, screenshot_hdr) and the post profiler overlay (r_post_profile).
#include "game_internal.h"
#include "player/spectator.h"
#include "game_revive.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "render/fidelity/fidelity.h"
#include "ui/ui.h"
#include "ui/crosshair.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define MAX_POST_VOLUMES 32

// Which fields a volume overrides (unset fields keep the value from below).
enum {
    PV_EXPOSURE = 1 << 0, PV_SATURATION = 1 << 1, PV_CONTRAST = 1 << 2, PV_TEMP = 1 << 3, PV_TINT = 1 << 4,
    PV_VIGNETTE = 1 << 5, PV_BLOOM = 1 << 6, PV_FOG = 1 << 7, PV_FOG_ALBEDO = 1 << 8, PV_GAIN = 1 << 9,
    PV_LIFT = 1 << 10, PV_GAMMA = 1 << 11, PV_CHROMATIC = 1 << 12, PV_GRAIN = 1 << 13, PV_FOG_HEIGHT = 1 << 14,
    PV_SHAFTS = 1 << 15, PV_LUT_WEIGHT = 1 << 16, PV_AO = 1 << 17,
};

typedef struct PostVolume {
    char name[32];
    bool global;
    vec3 mn, mx;
    float blend;             // meters outside the box over which the weight fades to 0
    int priority;
    u32 mask;
    float exposure, saturation, contrast, temp, tint, vignette, bloom, fog, fog_height, chromatic, grain, shafts, lut_weight, ao;
    vec3 fog_albedo, gain, lift, gamma;
} PostVolume;

static struct {
    Cvar *preset, *aa, *upscaler, *upscale, *sharpen, *ao, *contact, *ssr, *ssgi, *vol, *mb, *mb_strength, *dof, *bloom, *bloom_q,
        *bloom_int, *flare, *chromatic, *vignette, *grain, *tonemap, *exp_comp, *auto_exp, *brightness, *gamma, *colorblind, *lut,
        *debug, *compare, *paper, *maxnits, *feedback, *profile, *volumes_draw, *distortion, *underwater, *lens_dirt, *fog_scale,
        *dof_quality, *vol_lights;
    PostVolume vols[MAX_POST_VOLUMES];
    int vol_count;
    char vol_level[64];
    float lut_loaded_mod;
    // feedback state
    float sprint, lowhealth, desat, flash, nightvision_w;
    bool nightvision;
    // photo mode
    bool photo;
    float photo_focus, photo_fstop, photo_focal, photo_exposure, photo_vignette, photo_grain, photo_temp, photo_sat;
    int photo_tm, photo_dof;
    // post_bench
    int bench_frames, bench_left;
    float bench_ms[64];
    char bench_names[64][32];
    int bench_count;
    float bench_total_post, bench_total_all;
    char bench_label[32];
    PostSettings last;
} GP;

// ------------------------------------------------------------------ presets

typedef struct PresetDef { int aa, ao, contact, ssr, ssgi, vol, mb, dof, bloom_q, flare; } PresetDef;
static const PresetDef k_presets[4] = {
    // aa: 0 off, 1 FXAA, 2 SMAA, 3 TAA
    { 1, 0, 0, 0, 0, 0, 0, 0, 1, 0 },   // Low
    { 3, 1, 1, 1, 0, 1, 1, 1, 1, 0 },   // Medium
    { 3, 2, 2, 2, 0, 2, 1, 1, 2, 0 },   // High
    { 3, 2, 2, 2, 1, 3, 2, 1, 2, 1 },   // Ultra
};
static const char *k_preset_names[5] = { "Low", "Medium", "High", "Ultra", "Custom" };

static void apply_preset(int p) {
    if (p < 0 || p > 3) return;
    const PresetDef *d = &k_presets[p];
    CvarSource src = cvar_source_push(CVAR_SRC_FIDELITY);   // tier cvars (docs/SETTINGS_AUDIT.md)
    cvar_set_int(GP.aa, d->aa);
    cvar_set_int(GP.ao, d->ao);
    cvar_set_int(GP.contact, d->contact);
    cvar_set_int(GP.ssr, d->ssr);
    cvar_set_int(GP.ssgi, d->ssgi);
    cvar_set_int(GP.vol, d->vol);
    cvar_set_int(GP.mb, d->mb);
    cvar_set_int(GP.dof, d->dof);
    cvar_set_int(GP.bloom_q, d->bloom_q);
    // (d->flare: the lens flare strength is the Look's, locked for every preset.)
    cvar_set_int(GP.preset, p);
    cvar_source_pop(src);
}

// Presets are applied through the Engine Fidelity bounds (docs/ENGINE_FIDELITY.md); one only reads
// back when the level allows all of it (game_post_preset_allowed), otherwise Custom.
static int pv(int f, int v) { return (int)lroundf(fid_clamp_current(f, (float)v)); }
int game_post_detect_preset(void) {
    for (int p = 0; p < 4; p++) {
        const PresetDef *d = &k_presets[p];
        if (!game_post_preset_allowed(p, NULL)) continue;
        if (GP.aa->i == pv(FF_AA, d->aa) && GP.ao->i == pv(FF_AO, d->ao) && GP.contact->i == pv(FF_CONTACT_SHADOWS, d->contact) &&
            GP.ssr->i == pv(FF_SSR, d->ssr) && GP.ssgi->i == pv(FF_SSGI, d->ssgi) && GP.vol->i == pv(FF_VOLUMETRICS, d->vol) &&
            GP.mb->i == pv(FF_MOTION_BLUR, d->mb) && GP.dof->i == pv(FF_DOF, d->dof) && GP.bloom_q->i == pv(FF_BLOOM_QUALITY, d->bloom_q) &&
            ((GP.flare->f > 0) == (d->flare != 0)))
            return p;
    }
    return 4;
}

// Whether the current Engine Fidelity level (and era) allows every value of a preset; else the
// lowest level that does (*req, -1 = none in this era).
bool game_post_preset_allowed(int p, int *req) {
    if (p < 0 || p > 3) return false;
    const PresetDef *d = &k_presets[p];
    const int f[9] = { FF_AA, FF_AO, FF_CONTACT_SHADOWS, FF_SSR, FF_SSGI, FF_VOLUMETRICS, FF_MOTION_BLUR, FF_DOF, FF_BLOOM_QUALITY };
    const float v[9] = { (float)d->aa, (float)d->ao, (float)d->contact, (float)d->ssr, (float)d->ssgi, (float)d->vol, (float)d->mb, (float)d->dof, (float)d->bloom_q };
    int need = 0, era = fid_era();
    for (int i = 0; i < 9; i++) {
        int l = fid_min_level_for(era, f[i], v[i]);
        if (l < 0) { need = -1; break; }
        need = DG_MAX(need, l);
    }
    if (req) *req = need;
    return need >= 0 && need <= fid_level();
}
const char *game_post_preset_name(int p) { return k_preset_names[DG_CLAMP(p, 0, 4)]; }
void game_post_apply_preset(int p) { apply_preset(p); }

static void on_preset(Cvar *c) {
    if (c->i >= 0 && c->i <= 3 && game_post_detect_preset() != c->i) apply_preset(c->i);
}

// ------------------------------------------------------------------ post volumes

static float jfv(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static bool jvec(const cJSON *o, const char *k, vec3 *out) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 3) return false;
    out->x = (float)cJSON_GetArrayItem(a, 0)->valuedouble;
    out->y = (float)cJSON_GetArrayItem(a, 1)->valuedouble;
    out->z = (float)cJSON_GetArrayItem(a, 2)->valuedouble;
    return true;
}

static void parse_overrides(PostVolume *v, const cJSON *o) {
#define F(key, field, bit) do { const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, key); if (cJSON_IsNumber(x)) { v->field = (float)x->valuedouble; v->mask |= bit; } } while (0)
    F("exposure", exposure, PV_EXPOSURE);
    F("saturation", saturation, PV_SATURATION);
    F("contrast", contrast, PV_CONTRAST);
    F("temperature", temp, PV_TEMP);
    F("tint", tint, PV_TINT);
    F("vignette", vignette, PV_VIGNETTE);
    F("bloom", bloom, PV_BLOOM);
    F("fog_density", fog, PV_FOG);
    F("fog_height", fog_height, PV_FOG_HEIGHT);
    F("chromatic", chromatic, PV_CHROMATIC);
    F("grain", grain, PV_GRAIN);
    F("sun_shafts", shafts, PV_SHAFTS);
    F("lut_weight", lut_weight, PV_LUT_WEIGHT);
    F("ao", ao, PV_AO);
#undef F
    if (jvec(o, "fog_albedo", &v->fog_albedo)) v->mask |= PV_FOG_ALBEDO;
    if (jvec(o, "gain", &v->gain)) v->mask |= PV_GAIN;
    if (jvec(o, "lift", &v->lift)) v->mask |= PV_LIFT;
    if (jvec(o, "gamma", &v->gamma)) v->mask |= PV_GAMMA;
}

static void load_volumes(const char *map) {
    GP.vol_count = 0;
    dg_strcpy(GP.vol_level, sizeof(GP.vol_level), map);
    char rel[128], buf[512];
    snprintf(rel, sizeof(rel), "data/levels/%s.json", map);
    char *text = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
    if (!text) return;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return;
    const cJSON *glob = cJSON_GetObjectItemCaseSensitive(root, "post");
    if (cJSON_IsObject(glob)) {
        PostVolume *v = &GP.vols[GP.vol_count++];
        memset(v, 0, sizeof(*v));
        dg_strcpy(v->name, sizeof(v->name), "global");
        v->global = true;
        v->priority = -1000;
        parse_overrides(v, glob);
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "post_volumes")) {
        if (GP.vol_count >= MAX_POST_VOLUMES) break;
        PostVolume *v = &GP.vols[GP.vol_count];
        memset(v, 0, sizeof(*v));
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
        dg_strcpy(v->name, sizeof(v->name), cJSON_IsString(nm) ? nm->valuestring : "volume");
        vec3 a, b;
        if (!jvec(e, "min", &a) || !jvec(e, "max", &b)) { log_warn("post: volume '%s' needs min / max", v->name); continue; }
        v->mn = v3(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z));
        v->mx = v3(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z));
        v->blend = jfv(e, "blend", 2.0f);
        v->priority = (int)jfv(e, "priority", 0);
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(e, "settings");
        parse_overrides(v, s ? s : e);
        GP.vol_count++;
    }
    cJSON_Delete(root);
    // Low priority first (blended over in order).
    for (int i = 1; i < GP.vol_count; i++)
        for (int k = i; k > 0 && GP.vols[k].priority < GP.vols[k - 1].priority; k--) {
            PostVolume t = GP.vols[k];
            GP.vols[k] = GP.vols[k - 1];
            GP.vols[k - 1] = t;
        }
    log_info("post: %d post volume(s) for '%s'", GP.vol_count, map);
}

static float volume_weight(const PostVolume *v, vec3 p) {
    if (v->global) return 1.0f;
    vec3 d = v3(fmaxf(fmaxf(v->mn.x - p.x, p.x - v->mx.x), 0.0f), fmaxf(fmaxf(v->mn.y - p.y, p.y - v->mx.y), 0.0f),
                fmaxf(fmaxf(v->mn.z - p.z, p.z - v->mx.z), 0.0f));
    float dist = v3_len(d);
    if (dist <= 0.0f) return 1.0f;
    if (v->blend <= 0.0f) return 0.0f;
    return 1.0f - smoothstepf(0.0f, v->blend, dist);
}

static void apply_volume(PostSettings *s, const PostVolume *v, float w) {
    if (w <= 0.0f) return;
    u32 m = v->mask;
    if (m & PV_EXPOSURE) s->exposure_comp += v->exposure * w;
    if (m & PV_SATURATION) s->saturation *= lerpf(1.0f, v->saturation, w);
    if (m & PV_CONTRAST) s->contrast *= lerpf(1.0f, v->contrast, w);
    if (m & PV_TEMP) s->wb_temp = lerpf(s->wb_temp, v->temp, w);
    if (m & PV_TINT) s->wb_tint = lerpf(s->wb_tint, v->tint, w);
    if (m & PV_VIGNETTE) s->vignette = lerpf(s->vignette, v->vignette, w);
    if (m & PV_BLOOM) s->bloom_intensity *= lerpf(1.0f, v->bloom, w);
    if (m & PV_FOG) s->fog_density = lerpf(s->fog_density, v->fog, w);
    if (m & PV_FOG_HEIGHT) s->fog_height = lerpf(s->fog_height, v->fog_height, w);
    if (m & PV_FOG_ALBEDO) s->fog_albedo = v3_lerp(s->fog_albedo, v->fog_albedo, w);
    if (m & PV_SHAFTS) s->sun_shafts = lerpf(s->sun_shafts, v->shafts, w);
    if (m & PV_CHROMATIC) s->chromatic = lerpf(s->chromatic, v->chromatic, w);
    if (m & PV_GRAIN) s->grain = lerpf(s->grain, v->grain, w);
    if (m & PV_LUT_WEIGHT) s->lut_weight = lerpf(s->lut_weight, v->lut_weight, w);
    if (m & PV_AO) s->ao_intensity = lerpf(s->ao_intensity, v->ao, w);
    if (m & PV_GAIN) s->gain = v3_mul(s->gain, v3_lerp(v3(1, 1, 1), v->gain, w));
    if (m & PV_LIFT) s->lift = v3_add(s->lift, v3_scale(v->lift, w));
    if (m & PV_GAMMA) s->gamma = v3_mul(s->gamma, v3_lerp(v3(1, 1, 1), v->gamma, w));
}

// ------------------------------------------------------------------ commands

static void cmd_volumes(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Player *p = game_local_player();
    vec3 pos = p ? p->sim.move.pos : v3(0, 0, 0);
    log_info("post volumes (%s): %d", GP.vol_level, GP.vol_count);
    for (int i = 0; i < GP.vol_count; i++) {
        const PostVolume *v = &GP.vols[i];
        log_info("  %-16s prio %4d blend %.1f mask %05x weight %.2f", v->name, v->priority, v->blend, v->mask, volume_weight(v, pos));
    }
}

static void cmd_reload_volumes(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (G && G->g_map) load_volumes(G->g_map->str);
}

static void cmd_flash(int argc, const char **argv) { game_post_flash(argc > 1 ? (float)atof(argv[1]) : 1.0f); }
static void cmd_nightvision(int argc, const char **argv) {
    GP.nightvision = argc > 1 ? atoi(argv[1]) != 0 : !GP.nightvision;
    log_info("night vision %s", GP.nightvision ? "on" : "off");
}

// post_bench [frames] [label]: average GPU time per frame graph pass over N frames.
static void cmd_bench(int argc, const char **argv) {
    GP.bench_frames = GP.bench_left = argc > 1 ? DG_MAX(1, atoi(argv[1])) : 120;
    dg_strcpy(GP.bench_label, sizeof(GP.bench_label), argc > 2 ? argv[2] : "");
    GP.bench_count = 0;
    memset(GP.bench_ms, 0, sizeof(GP.bench_ms));
}

// perf_stop [frames=300] [label]: whole-frame timings over N frames (tools/tests/perf_compare.cfg,
// docs/NO_NANITE.md; run with r_maxfps 0 / r_vsync 0): avg / worst CPU frame (frame_dt), avg / worst
// GPU frame (command buffer timestamps), draw calls and triangles per frame.
static struct {
    int left, frames;
    char label[48];
    double cpu_sum, cpu_max, gpu_sum, gpu_max, draws, tris;
} PS;

static void cmd_perf_stop(int argc, const char **argv) {
    memset(&PS, 0, sizeof(PS));
    PS.frames = PS.left = argc > 1 ? DG_MAX(1, atoi(argv[1])) : 300;
    dg_strcpy(PS.label, sizeof(PS.label), argc > 2 ? argv[2] : "");
}

static void perf_stop_frame(void) {
    if (PS.left <= 0) return;
    double cpu = g_engine.frame_dt * 1000.0, gpu = render_gpu_frame_ms();
    RenderStats st = render_stats();
    PS.cpu_sum += cpu;
    PS.gpu_sum += gpu;
    PS.cpu_max = fmax(PS.cpu_max, cpu);
    PS.gpu_max = fmax(PS.gpu_max, gpu);
    PS.draws += st.draw_calls;
    PS.tris += st.triangles;
    if (--PS.left > 0) return;
    double n = (double)PS.frames;
    log_info("perf_stop %s: %d frames, render %ux%u: frame avg %.3f ms (%.1f fps), worst %.3f ms; GPU avg %.3f ms, worst %.3f ms; %.0f draws, %.2fM tris",
             PS.label, PS.frames, (unsigned)render_width(), (unsigned)render_height(), PS.cpu_sum / n, 1000.0 * n / fmax(PS.cpu_sum, 1e-6), PS.cpu_max, PS.gpu_sum / n,
             PS.gpu_max, PS.draws / n, PS.tris / n / 1e6);
}

static bool is_scene_pass(const char *n) {
    static const char *scene[] = { "prepass", "opaque", "decals", "opaque_dynamic", "scene_copy", "transparent", "vm_prepass", "viewmodel" };
    for (int i = 0; i < (int)ARRAY_COUNT(scene); i++) if (!strcmp(n, scene[i])) return true;
    return false;
}

static void bench_frame(void) {
    if (GP.bench_left <= 0) return;
    int n = render_pass_count();
    for (int i = 0; i < n && i < 64; i++) {
        const char *nm = render_pass_name(i);
        int k = 0;
        while (k < GP.bench_count && strcmp(GP.bench_names[k], nm)) k++;
        if (k == GP.bench_count && GP.bench_count < 64) dg_strcpy(GP.bench_names[GP.bench_count++], 32, nm);
        if (k < 64) GP.bench_ms[k] += render_pass_ms(i);
    }
    if (--GP.bench_left > 0) return;
    float post = 0, all = 0;
    log_info("post_bench %s: %d frames, render %ux%u", GP.bench_label, GP.bench_frames, (unsigned)render_width(), (unsigned)render_height());
    for (int k = 0; k < GP.bench_count; k++) {
        float ms = GP.bench_ms[k] / (float)GP.bench_frames;
        all += ms;
        if (!is_scene_pass(GP.bench_names[k])) post += ms;
        log_info("  %-16s %6.3f ms%s", GP.bench_names[k], ms, is_scene_pass(GP.bench_names[k]) ? "  (scene)" : "");
    }
    GP.bench_total_post = post;
    GP.bench_total_all = all;
    log_info("post_bench %s: post %.3f ms, all passes %.3f ms, memory %.1f MB", GP.bench_label, post, all, (double)render_post_memory() / (1024.0 * 1024.0));
}

static void screenshot_dir(char *out, size_t n) {
    snprintf(out, n, "%sscreenshots", platform_base_dir());
    SDL_CreateDirectory(out);
}

// screenshot_hdr [name]: PNG (display) + EXR (linear HDR) of the next frame.
static void cmd_screenshot_hdr(int argc, const char **argv) {
    char dir[512], png[600], exr[600];
    screenshot_dir(dir, sizeof(dir));
    char name[64];
    if (argc > 1) dg_strcpy(name, sizeof(name), argv[1]);
    else snprintf(name, sizeof(name), "shot_%llu", (unsigned long long)SDL_GetTicks());
    snprintf(png, sizeof(png), "%s/%s.png", dir, name);
    snprintf(exr, sizeof(exr), "%s/%s.exr", dir, name);
    if (argc > 2) {   // explicit full paths: screenshot_hdr name <png> <exr>
        dg_strcpy(png, sizeof(png), argv[2]);
        if (argc > 3) dg_strcpy(exr, sizeof(exr), argv[3]);
    }
    render_screenshot(png);
    render_capture_hdr(exr);
}

static void cmd_photomode(int argc, const char **argv) { game_post_set_photo_mode(argc > 1 ? atoi(argv[1]) != 0 : !GP.photo); }

static void cmd_post_status(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const PostSettings *s = &GP.last;
    log_info("post: preset %s aa %d up %d scale %.2f ao %d contact %d ssr %d ssgi %d vol %d mb %d dof %d bloom %d tm %d ev %.2f hdr %d/%d",
             game_post_preset_name(game_post_detect_preset()), s->aa, s->upscaler, s->render_scale, s->ao, s->contact_shadows, s->ssr, s->ssgi,
             s->volumetrics, s->motion_blur, s->dof, s->bloom, s->tonemapper, render_exposure_ev(), render_hdr_supported(), render_hdr_active());
    log_info("post: fog density %.4f height %.1f falloff %.3f anisotropy %.2f shafts %.2f", s->fog_density, s->fog_height, s->fog_falloff, s->fog_anisotropy, s->sun_shafts);
}

// ------------------------------------------------------------------ init

void game_post_init(void) {
    memset(&GP, 0, sizeof(GP));
    const u32 A = CVAR_ARCHIVE;
    GP.preset = cvar_reg_int("r_post_preset", 2, 0, 4, CVAR_TIER, "post quality preset: 0 low, 1 medium, 2 high, 3 ultra, 4 custom");
    GP.preset->on_change = on_preset;
    GP.aa = cvar_find("r_aa");
    GP.upscaler = cvar_reg_int("r_upscaler", 0, 0, 4, A | CVAR_EDITABLE, "upscaler: 0 off, 1 TAA-U, 2 FSR 1, 3 FSR 2 (slot), 4 DLSS (slot)");
    GP.upscale = cvar_reg_float("r_upscale_ratio", 0.67f, 0.5f, 1.0f, A | CVAR_EDITABLE, "render resolution / output with an upscaler");
    GP.sharpen = cvar_reg_float("r_sharpen", 0.35f, 0.0f, 1.0f, A | CVAR_EDITABLE, "contrast adaptive sharpening");
    GP.ao = cvar_reg_int("r_ao", 2, 0, 2, CVAR_TIER, "ambient occlusion (GTAO): 0 off, 1 low, 2 high");
    GP.contact = cvar_reg_int("r_contact_shadows", 2, 0, 2, CVAR_TIER, "screen-space contact shadows: 0 off, 1 low (8 steps), 2 high (14 steps)");
    GP.ssr = cvar_reg_int("r_ssr", 2, 0, 2, CVAR_TIER, "screen-space reflections: 0 off, 1 half res, 2 full res");
    GP.ssgi = cvar_reg_int("r_ssgi", 0, 0, 1, CVAR_TIER, "screen-space diffuse GI");
    GP.vol = cvar_reg_int("r_volumetrics", 2, 0, 3, CVAR_TIER, "volumetric fog / light shafts: 0 off .. 3 ultra");
    GP.mb = cvar_reg_int("r_motion_blur", 1, 0, 2, A | CVAR_EDITABLE, "motion blur: 0 off, 1 camera, 2 camera + objects");
    GP.mb_strength = cvar_reg_float("r_motion_blur_strength", 0.5f, 0.0f, 1.0f, A | CVAR_EDITABLE, "shutter fraction (0.5 = 180 degrees)");
    GP.dof = cvar_reg_int("r_dof", 1, 0, 2, A | CVAR_EDITABLE, "depth of field: 0 off, 1 aiming down sights, 2 always");
    GP.bloom = cvar_find("r_bloom");
    GP.bloom_q = cvar_reg_int("r_bloom_quality", 2, 0, 2, CVAR_TIER, "bloom mip chain: 0 tiny (4 mips), 1 low (5), 2 high (7)");
    GP.bloom_int = cvar_find("r_bloom_intensity");
    GP.flare = cvar_reg_float("r_lens_flare", 0.0f, 0.0f, 2.0f, CVAR_LOCKED, "lens flare ghosts / halo");
    GP.lens_dirt = cvar_reg_float("r_lens_dirt", 0.0f, 0.0f, 2.0f, CVAR_LOCKED, "lens dirt on bloom");
    GP.chromatic = cvar_reg_float("r_chromatic", 0.15f, 0.0f, 2.0f, A | CVAR_EDITABLE, "chromatic aberration");
    GP.vignette = cvar_find("r_vignette");
    GP.grain = cvar_reg_float("r_film_grain", 0.04f, 0.0f, 1.0f, A | CVAR_EDITABLE, "film grain");
    GP.distortion = cvar_reg_float("r_lens_distortion", 0.0f, -1.0f, 1.0f, CVAR_LOCKED, "lens distortion (barrel > 0)");
    GP.tonemap = cvar_reg_int("r_tonemapper", 1, 0, 2, CVAR_LOCKED, "tonemapper: 0 ACES (fit), 1 AgX, 2 Khronos PBR Neutral");
    GP.exp_comp = cvar_reg_float("r_exposure", 0.0f, -4.0f, 4.0f, CVAR_LOCKED, "exposure compensation (EV)");
    GP.auto_exp = cvar_reg_bool("r_auto_exposure", true, CVAR_LOCKED, "automatic exposure (histogram eye adaptation)");
    GP.brightness = cvar_find("r_brightness");
    GP.gamma = cvar_find("r_gamma");
    GP.colorblind = cvar_reg_int("r_colorblind", 0, 0, 3, A | CVAR_EDITABLE, "colour-blind correction: 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia");
    GP.lut = cvar_reg_string("r_lut", "", CVAR_LOCKED, "grading LUT (.cube, relative to the game folder); empty = neutral");
    GP.debug = cvar_reg_int("r_post_debug", 0, 0, 32, CVAR_LOCKED, "post debug view: 1 ao 2 contact 3 ssr 4 ssgi 5 velocity 6 normal 7 material 8 depth 9 hiz 10 coc 11 luminance 12 taa 13 froxel 14 bloom 15 flare 16 mb tiles 17 fog");
    GP.compare = cvar_reg_bool("r_post_compare", false, CVAR_LOCKED, "split A/B: left raw (exposure + tonemap), right full stack");
    GP.paper = cvar_reg_float("r_hdr_paper_white", 200.0f, 100.0f, 300.0f, A | CVAR_EDITABLE, "HDR10: paper white (nits)");
    GP.maxnits = cvar_reg_float("r_hdr_max_nits", 1000.0f, 400.0f, 10000.0f, A | CVAR_EDITABLE, "HDR10: display peak (nits)");
    GP.feedback = cvar_reg_float("r_post_feedback", 1.0f, 0.5f, 1.0f, A | CVAR_EDITABLE, "strength of the gameplay screen effects (damage, low health, flash ...)");
    GP.profile = cvar_reg_int("r_post_profile", 0, 0, 2, CVAR_LOCKED, "post profiler overlay: 1 passes, 2 passes + memory");
    GP.volumes_draw = cvar_reg_bool("r_post_volumes_draw", false, CVAR_LOCKED, "draw the post volumes");
    GP.underwater = cvar_reg_float("r_fb_underwater", 0.0f, 0.0f, 1.0f, CVAR_LOCKED, "underwater screen effect (test hook)");
    GP.fog_scale = cvar_reg_float("r_fog_scale", 1.0f, 0.0f, 10.0f, A | CVAR_LOCKED | CVAR_RULE, "volumetric fog density multiplier");
    GP.dof_quality = cvar_reg_int("r_dof_quality", 2, 0, 2, CVAR_TIER, "depth of field gather: 0 16 taps (disk), 1 32 (hybrid), 2 48 (bokeh)");
    GP.vol_lights = cvar_reg_bool("r_vol_local_lights", true, CVAR_TIER, "point lights scatter in the volumetric fog (off: fog only)");
    GP.photo_focus = 5.0f;
    GP.photo_fstop = 2.8f;
    GP.photo_focal = 50.0f;
    GP.photo_tm = -1;
    GP.photo_sat = 1.0f;
    GP.lut_loaded_mod = -1;
    cmd_register("post_volumes", cmd_volumes, "list the post volumes of the level and their weights here");
    cmd_register("post_reload", cmd_reload_volumes, "reload the level's post volumes");
    cmd_register("post_flash", cmd_flash, "post_flash [strength]: flashbang screen effect");
    cmd_register("nightvision", cmd_nightvision, "toggle the night vision effect");
    cmd_register("post_bench", cmd_bench, "post_bench [frames] [label]: average GPU ms per render pass");
    cmd_register("perf_stop", cmd_perf_stop, "perf_stop [frames=300] [label]: avg / worst CPU and GPU frame time, draws, triangles over N frames");
    cmd_register("post_status", cmd_post_status, "print the active post settings");
    cmd_register("screenshot_hdr", cmd_screenshot_hdr, "screenshot_hdr [name]: PNG + linear EXR into screenshots/");
    cmd_register("photomode", cmd_photomode, "toggle photo mode (HUD hidden, camera / grade controls)");
    game_look_init();
}

// ------------------------------------------------------------------ per frame

void game_post_flash(float strength) { GP.flash = fmaxf(GP.flash, saturate(strength)); }
bool game_post_photo_mode(void) { return GP.photo; }
void game_post_set_photo_mode(bool on) {
    GP.photo = on;
    log_info("photo mode %s", on ? "on (HUD hidden; photomode 0 to leave)" : "off");
}
float game_post_bench_post_ms(void) { return GP.bench_total_post; }

static const char *lut_name(void) {
    const char *l = GP.lut->str;
    return (!strcmp(l, "none") || !strcmp(l, "off") || !strcmp(l, "0")) ? "" : l;
}

static void build_settings(PostSettings *s, const PostFrameInfo *fi) {
    post_settings_default(s);
    s->aa = DG_CLAMP(GP.aa->i, 0, 3);
    s->upscaler = DG_CLAMP(GP.upscaler->i, 0, 4);
    s->render_scale = GP.upscale->f;
    s->sharpen = GP.sharpen->f;
    s->ao = GP.ao->i;
    s->contact_shadows = GP.contact->i;
    s->ssr = GP.ssr->i;
    s->ssgi = GP.ssgi->i;
    s->volumetrics = GP.vol->i;
    s->vol_local_lights = GP.vol_lights->i != 0;
    s->dof_quality = GP.dof_quality->i;
    // Engine Fidelity Off: no post-processing at all (the renderer's direct output path).
    s->direct_output = fid_loaded() && !fid_level_post(fid_level());
    s->motion_blur = GP.mb->i;
    s->mb_intensity = GP.mb_strength->f;
    s->dof = GP.dof->i;
    s->bloom = GP.bloom->i ? DG_MAX(GP.bloom_q->i, 1) : 0;
    s->bloom_mips = GP.bloom_q->i == 0 ? 4 : 0;   // tiny (Engine Fidelity Low)
    s->bloom_intensity = GP.bloom_int->f;
    s->bloom_dirt = GP.lens_dirt->f;
    s->flare = GP.flare->f;
    s->anamorphic = GP.flare->f * 0.5f;
    s->chromatic = GP.chromatic->f;
    s->vignette = GP.vignette->f;
    s->grain = GP.grain->f;
    s->distortion = GP.distortion->f;
    s->tonemapper = DG_CLAMP(GP.tonemap->i, 0, 2);
    s->exposure_auto = GP.auto_exp->i;
    s->exposure_comp = GP.exp_comp->f;
    s->exposure_manual = 1.5f + GP.exp_comp->f;
    s->brightness = GP.brightness->f;
    s->user_gamma = GP.gamma->f;
    s->colorblind = GP.colorblind->i;
    s->debug_view = GP.debug->i;
    s->compare_split = GP.compare->i != 0;
    s->paper_white = GP.paper->f;
    s->max_nits = GP.maxnits->f;
    s->lut_weight = lut_name()[0] ? 1.0f : 0.0f;
    // The Look (docs/LOOKS.md): its baked grade and its finish, scaled by the effect cvars above as
    // strengths and bounded by Engine Fidelity. Post volumes still override on top.
    game_look_apply(s);
    // Level fog -> volumetric density (the lit shaders' analytic fog is off while froxels run).
    s->fog_density = fmaxf(fi->level_fog * 1.5f, 0.004f) * GP.fog_scale->f;
    s->fog_albedo = v3(0.9f, 0.92f, 0.95f);
    // Post volumes: global grade, then local boxes by priority.
    for (int i = 0; i < GP.vol_count; i++) apply_volume(s, &GP.vols[i], volume_weight(&GP.vols[i], fi->cam_pos));
    // ADS-only depth of field focuses through the GPU autofocus (crosshair depth).
    s->dof_autofocus = 1;
}

static void apply_feedback(PostSettings *s, const PostFrameInfo *fi, float dt) {
    float k = GP.feedback->f;
    Game *g = G;
    // The viewed player's health / death (a spectated player's in first person); the free camera
    // of a dead spectator is not "dead".
    Player *p = game_view_player();
    if (spectator_mode() == SPEC_FREE) p = NULL;
    float hp_frac = 1.0f, dead = 0.0f;
    if (p && g && g->world) {
        Health *h = &g->world->health[ENT_INDEX(p->ent)];
        hp_frac = h->max_hp > 0 ? h->hp / h->max_hp : 1.0f;
        dead = p->sim.move.mode == MOVE_DEAD ? 1.0f : 0.0f;
    }
    float low_target = dead > 0 ? 0.0f : saturate((0.35f - hp_frac) / 0.25f);
    GP.lowhealth += (low_target - GP.lowhealth) * (1.0f - expf(-dt * 3.0f));
    GP.desat += (dead - GP.desat) * (1.0f - expf(-dt * 1.5f));
    float sprint_t = fi->sprinting ? 1.0f : 0.0f;
    GP.sprint += (sprint_t - GP.sprint) * (1.0f - expf(-dt * 4.0f));
    GP.flash = fmaxf(0.0f, GP.flash - dt * 0.45f);
    GP.nightvision_w += ((GP.nightvision ? 1.0f : 0.0f) - GP.nightvision_w) * (1.0f - expf(-dt * 8.0f));
    float hm = 0.0f;
    if (g && g->hitmarker_time > 0) hm = saturate(g->hitmarker_time / crosshair_hitmarker_duration()) * (g->hitmarker_kill ? 0.6f : 0.3f);
    s->fb_damage = (g ? saturate(g->damage_flash) : 0.0f) * k;
    s->fb_lowhealth = GP.lowhealth * k;
    s->fb_flash = GP.flash * k;
    s->fb_hitmarker = hm * k;
    // Fire animation (docs/FIRE_ANIM.md layer 5): the 1-2 tick blur spike on the viewmodel and the
    // chromatic / vignette tick of heavy weapons (already 0 at Low fidelity).
    {
        float blur = 0.0f, chroma = 0.0f;
        game_fire_post(&blur, &chroma);
        s->mb_viewmodel = blur * 0.5f;
        s->chromatic += chroma * 0.6f;
        s->vignette += chroma * 0.12f;
    }
    s->fb_sprint = GP.sprint * 0.6f * k;
    s->fb_underwater = GP.underwater->f;
    s->fb_nightvision = GP.nightvision_w;
    s->fb_desaturate = GP.desat * 0.85f * k;
    if (GP.nightvision_w > 0.01f) s->exposure_comp += 2.5f * GP.nightvision_w;   // amplified light
    if (GP.desat > 0.01f) s->fb_tint = v3_lerp(v3(1, 1, 1), v3(0.85f, 0.8f, 0.8f), GP.desat);
}

static void apply_photo(PostSettings *s) {
    if (!GP.photo) return;
    if (GP.photo_dof) {
        s->dof = 2;
        s->dof_autofocus = GP.photo_dof == 1;
        s->dof_focus = GP.photo_focus;
        s->dof_fstop = GP.photo_fstop;
        s->dof_focal_mm = GP.photo_focal;
        s->dof_max_coc = 24.0f;
    }
    s->exposure_comp += GP.photo_exposure;
    s->vignette += GP.photo_vignette;
    s->grain += GP.photo_grain;
    s->wb_temp += GP.photo_temp;
    s->saturation *= GP.photo_sat;
    if (GP.photo_tm >= 0) {
        s->tonemapper = GP.photo_tm;
        s->look_lut = -1;   // a photo-mode tonemapper replaces the look's grade
    }
    s->motion_blur = 0;
}

void game_post_frame(const PostFrameInfo *fi) {
    float dt = fminf((float)g_engine.frame_dt, 0.1f);
    const char *map = G && G->g_map ? G->g_map->str : "";
    if (strcmp(map, GP.vol_level)) load_volumes(map);
    // LUT file (reloaded when r_lut changes).
    if ((float)GP.lut->modified != GP.lut_loaded_mod) {
        GP.lut_loaded_mod = (float)GP.lut->modified;
        render_load_lut(lut_name());
    }
    game_look_frame();
    PostSettings s;
    build_settings(&s, fi);
    daynight_post(&s);
    weather_post(&s);
    game_look_modulate(&s);
    apply_feedback(&s, fi, dt);
    game_death_post(&s);   // death sequence / downed variant (game_death.c)
    peek_post(&s);   // door peek: fisheye through the visor
    apply_photo(&s);
    // With ADS DoF on (game_sights.c), aiming switches the DoF exactly when the aim completes;
    // the older button-driven "ADS only" mode stays for ads_dof 0.
    render_set_camera_fx(fi->ads && !sights_dof_managed(), 0.0f);
    render_post_settings(&s);
    GP.last = s;
    if (GP.volumes_draw->i)
        for (int i = 0; i < GP.vol_count; i++)
            if (!GP.vols[i].global) dbg_aabb(GP.vols[i].mn, GP.vols[i].mx, volume_weight(&GP.vols[i], fi->cam_pos) > 0.99f ? DBG_GREEN : DBG_YELLOW, 0);
    bench_frame();
    perf_stop_frame();
}

// ------------------------------------------------------------------ overlays

// r_post_profile: GPU ms of every frame graph pass, the post total against the preset budget.
void game_post_draw_overlay(void) {
    if (!GP.profile->i) return;
    float s = ui_scale();
    float x = ui_width() - 300 * s, y = 80 * s;
    int n = render_pass_count();
    float post = 0, all = 0;
    for (int i = 0; i < n; i++) {
        float ms = render_pass_ms(i);
        all += ms;
        if (!is_scene_pass(render_pass_name(i))) post += ms;
    }
    int preset = game_post_detect_preset();
    float budget = preset >= 2 ? 4.0f : 2.0f;
    ui_rect(x - 10 * s, y - 8 * s, 290 * s, (float)(n + 4) * 15 * s + 16 * s, UI_RGBA(0, 0, 0, 170));
    u32 rw = 0, rh = 0;
    render_render_size(&rw, &rh);
    ui_textf(x, y, FONT_SMALL, UI_RGBA(255, 220, 140, 255), UI_ALIGN_LEFT, "post %.2f ms / %.1f (%s)  all %.2f ms", post, budget, game_post_preset_name(preset), all);
    y += 15 * s;
    ui_textf(x, y, FONT_SMALL, UI_RGBA(200, 200, 200, 255), UI_ALIGN_LEFT, "render %ux%u  EV %.2f  %s", rw, rh, render_exposure_ev(), render_hdr_active() ? "HDR10" : "SDR");
    y += 15 * s;
    if (GP.profile->i >= 2) {
        ui_textf(x, y, FONT_SMALL, UI_RGBA(200, 200, 200, 255), UI_ALIGN_LEFT, "frame graph memory %.1f MB", (double)render_post_memory() / (1024.0 * 1024.0));
        y += 15 * s;
    }
    for (int i = 0; i < n; i++) {
        float ms = render_pass_ms(i);
        bool scene = is_scene_pass(render_pass_name(i));
        u32 c = scene ? UI_RGBA(150, 150, 160, 255) : (ms > 0.5f ? UI_RGBA(255, 140, 120, 255) : UI_RGBA(220, 230, 220, 255));
        ui_textf(x, y, FONT_SMALL, c, UI_ALIGN_LEFT, "%-16s %6.3f", render_pass_name(i), ms);
        ui_rect(x + 180 * s, y + 3 * s, fminf(ms * 40.0f, 90.0f) * s, 8 * s, c);
        y += 15 * s;
    }
}

// Photo mode panel (Nuklear): camera / lens / grade controls and the capture button.
#include "nuklear_config.h"
void game_post_draw_photo_ui(void) {
    if (!GP.photo) return;
    struct nk_context *ctx = ui_nk();
    float s = ui_scale();
    if (nk_begin(ctx, "PHOTO MODE", nk_rect(20 * s, 20 * s, 300 * s, 470 * s), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE)) {
        static const char *dof_modes[] = { "DoF off", "DoF autofocus", "DoF manual" };
        nk_layout_row_dynamic(ctx, 24 * s, 1);
        GP.photo_dof = nk_combo(ctx, dof_modes, 3, GP.photo_dof, (int)(22 * s), nk_vec2(260 * s, 120 * s));
        nk_property_float(ctx, "Focus (m)", 0.2f, &GP.photo_focus, 500.0f, 0.1f, 0.02f);
        nk_property_float(ctx, "f-stop", 0.7f, &GP.photo_fstop, 22.0f, 0.1f, 0.02f);
        nk_property_float(ctx, "Focal (mm)", 12.0f, &GP.photo_focal, 300.0f, 1.0f, 0.2f);
        nk_property_float(ctx, "Exposure (EV)", -5.0f, &GP.photo_exposure, 5.0f, 0.1f, 0.02f);
        nk_property_float(ctx, "Temperature", -1.0f, &GP.photo_temp, 1.0f, 0.02f, 0.005f);
        nk_property_float(ctx, "Saturation", 0.0f, &GP.photo_sat, 2.0f, 0.02f, 0.005f);
        nk_property_float(ctx, "Vignette +", 0.0f, &GP.photo_vignette, 1.0f, 0.02f, 0.005f);
        nk_property_float(ctx, "Grain +", 0.0f, &GP.photo_grain, 1.0f, 0.02f, 0.005f);
        static const char *tms[] = { "Tonemap: setting", "ACES", "AgX", "PBR Neutral" };
        int tm = nk_combo(ctx, tms, 4, GP.photo_tm + 1, (int)(22 * s), nk_vec2(260 * s, 140 * s));
        GP.photo_tm = tm - 1;
        nk_layout_row_dynamic(ctx, 30 * s, 1);
        if (nk_button_label(ctx, "Capture PNG + EXR")) cmd_screenshot_hdr(1, NULL);
        if (nk_button_label(ctx, "Leave photo mode")) game_post_set_photo_mode(false);
        nk_layout_row_dynamic(ctx, 18 * s, 1);
        nk_label(ctx, "noclip to fly the camera", NK_TEXT_LEFT);
    }
    nk_end(ctx);
}
