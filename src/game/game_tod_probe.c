// Time-of-day continuity probe (docs/DAYNIGHT.md "Continuity"): per-frame measurements of the
// world clock, the sky's sun / moon, the shading and shadow light directions, the analytic shadow
// tip of a pole, the far cascades' light-direction lag and the client clock correction.
//
//   tod_test continuity <seconds> [quit] [tag]   statistics over the run, then PASS / FAIL
//   tod_test trace <frames> <csv>                 one CSV row per frame (investigations, tools/tests)
//   tod_test shots <frames> <dir>                 a linear HDR EXR per frame (<dir>/f<frame>.exr)
//
// Every per-frame step is compared with the step the clock's rate predicts from the frame's real
// time (dt x rate game seconds): within +-5 % on every frame, no zero-delta frame, the azimuth
// monotonic, the elevation never reversing inside a half day. A frame whose rate changed
// (time_scale, time_pause, tod_demo) must step between what the old and the new rate give (no jump
// on the change); a time_set blend must never step more than its ease allows. Hitches (dt > 0.1 s)
// and instant changes (a jump at a new TimeState version without a rate change) are left out.
#include "game_internal.h"
#include "render/render.h"
#include "world/time_of_day.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TP_TOL 0.05          // +-5 % of the expected per-frame step
#define TP_REF_CASTER 4.0    // m: the reference caster of the far-cascade lag (as render.c's hold test)
#define TP_TWO_PI 6.283185307179586

typedef struct TpStat {
    int n, out, zero;
    double min, max, sum;
} TpStat;

static struct {
    int mode;                    // 1 = continuity running
    double end_time;
    bool quit;
    char tag[32];
    bool have_prev;
    int settle;
    double prev_sec, prev_rate;
    vec3 prev_sun, prev_key, prev_fill;
    bool prev_moon_key;
    u16 prev_ver;
    u32 prev_luts, prev_refilters;
    TpStat clock, sun, key, moon, tip, key_lut, key_ibl;
    int changes, change_bad, blend_frames, blend_bad;
    double blend_worst;
    int az_back, el_back, frames;
    double dt_sum, dt_max;
    double lag_texels[4], lag_deg[4];
    int held[4], casc_frames;
    double corr_max;
    int snaps0;
    // GPU cost (render_gpu_frame_ms, the sky passes): this run's frames
    double gpu_ms, sky_ms[6];
    int gpu_n;
    // trace / shots
    FILE *f;
    int left;
    int shots;
    char dir[400];
} TP;

static void stat_reset(TpStat *s) {
    memset(s, 0, sizeof(*s));
    s->min = 1e30;
    s->max = -1e30;
}

static void stat_add(TpStat *s, double r) {
    s->n++;
    s->sum += r;
    if (r < s->min) s->min = r;
    if (r > s->max) s->max = r;
    if (fabs(r - 1.0) > TP_TOL) s->out++;
    if (r < 0.05) s->zero++;
}

static bool stat_ok(const TpStat *s) { return s->out == 0 && s->zero == 0; }

static void stat_log(const char *name, const TpStat *s) {
    if (!s->n) { log_info("tod_test continuity %s: %-9s no frames", TP.tag, name); return; }
    log_info("tod_test continuity %s: %-9s %5d frames  step / expected %.4f .. %.4f (mean %.4f)  outside +-5 %% %d  zero %d -> %s", TP.tag, name, s->n, s->min,
             s->max, s->sum / s->n, s->out, s->zero, stat_ok(s) ? "ok" : "FAIL");
}

// Angle between two directions, robust for tiny angles (atan2 of the cross and the dot, in double).
static double ang(vec3 a, vec3 b) {
    double ax = a.x, ay = a.y, az = a.z, bx = b.x, by = b.y, bz = b.z;
    double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    return atan2(sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz);
}

static double elev_d(vec3 v) { return asin(fmax(-1.0, fmin(1.0, (double)v.y / sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z)))); }
static double azim_d(vec3 v) {
    double a = atan2((double)v.x, -(double)v.z);
    return a < 0 ? a + TP_TWO_PI : a;
}

// The shadow tip of a 10 m pole at the origin under a light towards L (ground plane).
static void tip(vec3 L, double *x, double *z) {
    double y = fmax((double)L.y, 1e-4);
    *x = -10.0 * L.x / y;
    *z = -10.0 * L.z / y;
}

static void continuity_start(int argc, const char **argv) {
    stat_reset(&TP.clock), stat_reset(&TP.sun), stat_reset(&TP.key), stat_reset(&TP.moon), stat_reset(&TP.tip), stat_reset(&TP.key_lut), stat_reset(&TP.key_ibl);
    float secs = argc > 2 ? (float)atof(argv[2]) : 10.0f;
    TP.mode = 1;
    TP.end_time = g_engine.real_time + fmax(secs, 1.0f);
    TP.quit = false;
    snprintf(TP.tag, sizeof(TP.tag), "%s", "run");
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "quit")) TP.quit = true;
        else snprintf(TP.tag, sizeof(TP.tag), "%s", argv[i]);
    }
    TP.have_prev = false;
    TP.settle = 3;
    TP.changes = TP.change_bad = TP.blend_frames = TP.blend_bad = 0;
    TP.blend_worst = 0;
    TP.az_back = TP.el_back = TP.frames = 0;
    TP.dt_sum = TP.dt_max = 0;
    for (int k = 0; k < 4; k++) TP.lag_texels[k] = TP.lag_deg[k] = 0, TP.held[k] = 0;
    TP.casc_frames = 0;
    TP.corr_max = 0;
    TP.snaps0 = -1;
    TP.gpu_ms = 0;
    TP.gpu_n = 0;
    for (int k = 0; k < 6; k++) TP.sky_ms[k] = 0;
    log_info("tod_test continuity %s: %.0f s", TP.tag, secs);
}

static void continuity_finish(const TodProbeIn *in) {
    TP.mode = 0;
    double fps = TP.frames ? TP.frames / fmax(TP.dt_sum, 1e-6) : 0;
    log_info("tod_test continuity %s: %d frames, %.1f fps (frame time %.2f ms mean, %.2f ms max), day %.1f s, %s", TP.tag, TP.frames, fps,
             TP.frames ? TP.dt_sum / TP.frames * 1000.0 : 0, TP.dt_max * 1000.0, in->day_length, in->authority ? "host" : "client");
    {
        double n = (double)DG_MAX(TP.gpu_n, 1);
        log_info("tod_test continuity %s: GPU frame %.3f ms | sky_luts %.4f  sky_view %.4f  sky_clouds %.4f  sky_cshadow %.4f  sky_env %.4f  sky %.4f ms per frame", TP.tag,
                 TP.gpu_ms / n, TP.sky_ms[0] / n, TP.sky_ms[1] / n, TP.sky_ms[2] / n, TP.sky_ms[3] / n, TP.sky_ms[4] / n, TP.sky_ms[5] / n);
    }
    stat_log("clock", &TP.clock);
    stat_log("sky sun", &TP.sun);
    stat_log("key", &TP.key);
    stat_log("moon", &TP.moon);
    stat_log("pole tip", &TP.tip);
    stat_log("key@LUT", &TP.key_lut);
    stat_log("key@IBL", &TP.key_ibl);
    log_info("tod_test continuity %s: sky sun azimuth backwards %d, elevation reversals %d -> %s", TP.tag, TP.az_back, TP.el_back,
             TP.az_back == 0 && TP.el_back == 0 ? "ok" : "FAIL");
    log_info("tod_test continuity %s: rate changes %d, stepped outside the old / new rate %d -> %s", TP.tag, TP.changes, TP.change_bad, TP.change_bad == 0 ? "ok" : "FAIL");
    log_info("tod_test continuity %s: blend frames %d, over the ease's bound %d (worst %.3f of it) -> %s", TP.tag, TP.blend_frames, TP.blend_bad, TP.blend_worst,
             TP.blend_bad == 0 ? "ok" : "FAIL");
    bool casc_ok = true;
    for (int k = 0; k < 4; k++) {
        if (!TP.casc_frames) break;
        bool ok = TP.lag_texels[k] <= 0.5 + 1e-6;
        casc_ok = casc_ok && ok;
        log_info("tod_test continuity %s: cascade %d kept %d / %d frames, light lag max %.4f deg = %.3f texels of a %.0f m caster -> %s", TP.tag, k, TP.held[k],
                 TP.casc_frames, TP.lag_deg[k], TP.lag_texels[k], TP_REF_CASTER, ok ? "ok" : "FAIL");
    }
    int snaps = TP.snaps0 >= 0 ? in->clock_snaps - TP.snaps0 : 0;
    bool clock_ok = in->authority || (TP.corr_max <= in->clock_slew + 1e-6 && snaps == 0);
    if (!in->authority)
        log_info("tod_test continuity %s: client clock correction max %.4f of the frame's advance (slew %.4f), snaps %d -> %s", TP.tag, TP.corr_max, in->clock_slew, snaps,
                 clock_ok ? "ok" : "FAIL");
    bool ok = stat_ok(&TP.clock) && stat_ok(&TP.sun) && stat_ok(&TP.key) && stat_ok(&TP.moon) && stat_ok(&TP.tip) && TP.az_back == 0 && TP.el_back == 0 &&
              TP.change_bad == 0 && TP.blend_bad == 0 && casc_ok && clock_ok && TP.sun.n + TP.key.n + TP.moon.n > 0;
    log_info("tod_test continuity %s: %s", TP.tag, ok ? "PASS" : "FAIL");
    if (TP.quit) cbuf_add("quit");
}

static void continuity_frame(const TodConfig *cfg, const TodSky *s, const RenderEnvironment *env, const TodProbeIn *in) {
    double dt = g_engine.frame_dt;
    bool new_ver = in->version != TP.prev_ver;
    TP.prev_ver = in->version;
    u32 luts = render_sky_lut_builds(), refl = render_sky_refilters();
    bool lut_frame = luts != TP.prev_luts, ibl_frame = refl != TP.prev_refilters;
    TP.prev_luts = luts, TP.prev_refilters = refl;
    if (TP.snaps0 < 0) TP.snaps0 = in->clock_snaps;
    bool hitch = dt <= 0.0 || dt > 0.1;
    if (TP.settle <= 0) {
        static const char *names[6] = { "sky_luts", "sky_view", "sky_clouds", "sky_cshadow", "sky_env", "sky" };
        TP.gpu_ms += render_gpu_frame_ms();
        TP.gpu_n++;
        int np = render_pass_count();
        for (int i = 0; i < np; i++)
            for (int k = 0; k < 6; k++)
                if (!strcmp(render_pass_name(i), names[k])) TP.sky_ms[k] += render_pass_ms(i);
    }
    if (hitch) { TP.have_prev = false; TP.settle = 2; }
    else if (TP.settle > 0) TP.settle--;
    else if (TP.have_prev) {
        double step = in->sec - TP.prev_sec;
        double r_lo = fmin(TP.prev_rate, in->rate), r_hi = fmax(TP.prev_rate, in->rate);
        if (in->blending || in->blend_sec != 0.0) {
            // A time_set blend: the ease (smoothstep) of its offset never steps more than 1.5 x the
            // offset / the duration per second on top of the rate.
            double bound = (r_hi + 1.5 * fabs(in->blend_sec) / fmax(in->blend_s, 1e-3)) * dt * (1.0 + TP_TOL) + 1e-6;
            TP.blend_frames++;
            TP.blend_worst = fmax(TP.blend_worst, fabs(step) / bound);
            if (fabs(step) > bound) TP.blend_bad++;
        } else if (r_hi <= 0.0) {
            // paused
        } else if (r_hi - r_lo > 1e-6 * r_hi) {
            // The rate changed: the step lies between what the old and the new rate give.
            TP.changes++;
            if (step < r_lo * dt * (1.0 - TP_TOL) || step > r_hi * dt * (1.0 + TP_TOL)) {
                TP.change_bad++;
                log_info("tod_test continuity %s: rate change %.1f -> %.1f game s/s stepped %.2f game s (allowed %.2f .. %.2f)", TP.tag, TP.prev_rate, in->rate, step,
                         r_lo * dt * (1.0 - TP_TOL), r_hi * dt * (1.0 + TP_TOL));
            }
        } else if (new_ver && fabs(step - in->rate * dt) > TP_TOL * in->rate * dt) {
            // An instant change (time_set instant, time_days, presets): a deliberate jump.
        } else {
            TP.frames++;
            TP.dt_sum += dt;
            TP.dt_max = fmax(TP.dt_max, dt);
            double dsec = dt * in->rate;   // the clock's advance at its rate
            stat_add(&TP.clock, step / dsec);
            // The sky sun on its diurnal circle: 2 asin(cos dec sin(dH / 2)).
            double dH = dsec / 86400.0 * TP_TWO_PI;
            double exp_sun = 2.0 * asin(cos((double)s->declination) * sin(dH * 0.5));
            stat_add(&TP.sun, ang(TP.prev_sun, s->sun_dir) / exp_sun);
            double da = azim_d(s->sun_dir) - azim_d(TP.prev_sun);
            if (da > 3.14159) da -= TP_TWO_PI;
            if (da < -3.14159) da += TP_TWO_PI;
            if (da < 0.0) TP.az_back++;
            // Elevation: rising before solar noon (hour angle < 0), falling after.
            double H = (s->days - floor(s->days) - 0.5) * TP_TWO_PI;
            double de = elev_d(s->sun_dir) - elev_d(TP.prev_sun);
            if (fabs(sin(H)) > 0.02 && de * -sin(H) < 0.0) TP.el_back++;
            // The shading / shadow key and the moon's light (the second light by day, the key at night).
            TodSky e;
            tod_sky(cfg, s->days - dsec / 86400.0, &e);
            double exp_moon = ang(e.moon_dir, s->moon_dir);
            bool same_body = env->moon_key == TP.prev_moon_key;
            if (same_body && !env->moon_key && env->sun_dir.y > 0.03f && TP.prev_key.y > 0.03f) {
                double r = ang(TP.prev_key, env->sun_dir) / exp_sun;
                stat_add(&TP.key, r);
                if (lut_frame) stat_add(&TP.key_lut, r);
                if (ibl_frame) stat_add(&TP.key_ibl, r);
                if (env->sun_dir.y > 0.05f) {
                    double x0, z0, x1, z1, ex0, ez0, ex1, ez1;
                    tip(TP.prev_key, &x0, &z0), tip(env->sun_dir, &x1, &z1);
                    tip(e.sun_dir, &ex0, &ez0), tip(s->sun_dir, &ex1, &ez1);
                    double m = hypot(x1 - x0, z1 - z0), x = hypot(ex1 - ex0, ez1 - ez0);
                    if (x > 1e-7) stat_add(&TP.tip, m / x);
                }
                if (env->fill_dir.y > 0.03f && TP.prev_fill.y > 0.03f && exp_moon > 1e-9) stat_add(&TP.moon, ang(TP.prev_fill, env->fill_dir) / exp_moon);
            } else if (same_body && env->moon_key && env->sun_dir.y > 0.03f && TP.prev_key.y > 0.03f && exp_moon > 1e-9) {
                stat_add(&TP.moon, ang(TP.prev_key, env->sun_dir) / exp_moon);
            }
            // Far cascades (PF-0028): the light direction a kept tile was drawn with vs this frame's, as
            // the shadow motion of the reference caster in texels of the tile.
            float lag[4], texel[4];
            bool held[4];
            int nc = render_shadow_cascade_lag(lag, texel, held);
            if (nc > 0) {
                TP.casc_frames++;
                double se = fmax(sin(elev_d(env->sun_dir)), sin(3.0 * DG_DEG2RAD));
                for (int k = 0; k < nc && k < 4; k++) {
                    if (held[k]) TP.held[k]++;
                    double t = (double)lag[k] * DG_DEG2RAD * TP_REF_CASTER / (se * se) / fmax((double)texel[k], 1e-6);
                    if (t > TP.lag_texels[k]) TP.lag_texels[k] = t;
                    if (lag[k] > TP.lag_deg[k]) TP.lag_deg[k] = lag[k];
                }
            }
            if (!in->authority) TP.corr_max = fmax(TP.corr_max, fabs(in->clock_corr));
        }
    }
    if (!hitch) {
        TP.have_prev = true;
        TP.prev_sec = in->sec;
        TP.prev_rate = in->rate;
        TP.prev_sun = s->sun_dir;
        TP.prev_key = env->sun_dir;
        TP.prev_fill = env->fill_dir;
        TP.prev_moon_key = env->moon_key;
    }
    if (g_engine.real_time >= TP.end_time) continuity_finish(in);
}

static void trace_frame(const TodSky *s, const RenderEnvironment *env, const TodProbeIn *in) {
    float lag[4] = { 0 }, texel[4] = { 0 };
    bool held[4] = { 0 };
    render_shadow_cascade_lag(lag, texel, held);
    fprintf(TP.f, "%llu,%.6f,%d,%.6f,%.6f,%.7f,%.7f,%.7f,%.7f,%.7f,%d,%.7f,%.7f,%.7f,%.7f,%.5f,%.5f,%.4f,%u,%u,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%d,%d,%d,%d,%u,%.6f,%d,%.4f\n",
            (unsigned long long)g_engine.frame_index, g_engine.frame_dt, (int)in->authority, in->tick, in->sec, s->hours, elev_d(s->sun_dir) * DG_RAD2DEG,
            azim_d(s->sun_dir) * DG_RAD2DEG, elev_d(env->sun_dir) * DG_RAD2DEG, azim_d(env->sun_dir) * DG_RAD2DEG, (int)env->moon_key, elev_d(s->moon_dir) * DG_RAD2DEG,
            azim_d(s->moon_dir) * DG_RAD2DEG, elev_d(env->fill_dir) * DG_RAD2DEG, azim_d(env->fill_dir) * DG_RAD2DEG, env->sun_intensity, env->fill_intensity,
            env->mie_scale, render_sky_lut_builds(), render_sky_refilters(), env->cloud_offset.x, env->cloud_offset.y, lag[0], lag[1], lag[2], lag[3], (int)held[0],
            (int)held[1], (int)held[2], (int)held[3], (unsigned)in->version, in->clock_corr, in->clock_snaps, in->rate);
    if (--TP.left <= 0) {
        fclose(TP.f);
        TP.f = NULL;
        log_info("tod_test trace: done");
    }
}

void tod_probe_frame(World *w, const TodConfig *cfg, const TodSky *s, const RenderEnvironment *env, const TodProbeIn *in) {
    (void)w;
    if (TP.mode == 1) continuity_frame(cfg, s, env, in);
    if (TP.f && TP.left > 0) trace_frame(s, env, in);
    if (TP.shots > 0) {
        char p[460];
        snprintf(p, sizeof(p), "%s/f%06llu.exr", TP.dir, (unsigned long long)g_engine.frame_index);
        render_capture_hdr(p);
        TP.shots--;
    }
}

bool tod_probe_command(int argc, const char **argv) {
    const char *what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "continuity")) {
        continuity_start(argc, argv);
        return true;
    }
    if (!strcmp(what, "trace")) {
        if (argc < 4) { log_info("usage: tod_test trace <frames> <csv>"); return true; }
        if (TP.f) fclose(TP.f);
        TP.f = fopen(argv[3], "w");
        TP.left = atoi(argv[2]);
        if (!TP.f) { log_info("tod_test trace: cannot open %s", argv[3]); return true; }
        fprintf(TP.f, "frame,dt,auth,tick,sec,hours,sun_e,sun_a,key_e,key_a,key_moon,moon_e,moon_a,fill_e,fill_a,key_i,fill_i,mie,luts,refilters,cloud_x,cloud_y,"
                      "lag0,lag1,lag2,lag3,held0,held1,held2,held3,ver,clock_corr,clock_snaps,rate\n");
        log_info("tod_test trace: %d frames -> %s", TP.left, argv[3]);
        return true;
    }
    if (!strcmp(what, "shots")) {
        if (argc < 4) { log_info("usage: tod_test shots <frames> <dir>"); return true; }
        TP.shots = atoi(argv[2]);
        snprintf(TP.dir, sizeof(TP.dir), "%s", argv[3]);
        SDL_CreateDirectory(TP.dir);
        return true;
    }
    return false;
}
