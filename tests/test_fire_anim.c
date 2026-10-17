// Fire animation (src/anim/fire_anim.c) and global sway (src/player/sway.c): timing asymmetry,
// mass model, the lag chain, accumulation, perceptual gating, attachments, determinism, and
// sway identical for every weapon. docs/FIRE_ANIM.md "Tests".
#include "anim/fire_anim.h"
#include "player/sway.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DT (1.0f / 60.0f)
#define N 600

typedef struct Trace {
    float pitch[N], back[N], hand_pitch[N], hand_err[N], cam_pitch[N], cam_back[N], fov[N], blur[N];
    int shot_tick[256], shots;
} Trace;

// Fires `shots` shots at `rpm` from tick 2 (0 = one semi shot), the trigger held while firing.
static void run(const FireAnimDef *d, int shots, float rpm, float ads_t, float perceptual, Trace *t) {
    memset(t, 0, sizeof(*t));
    FireAnimState s;
    fire_anim_reset(&s);
    float interval = rpm > 0 ? 60.0f / rpm : 0.0f;
    float next = 2.0f * DT;
    int fired = 0;
    for (int i = 0; i < N; i++) {
        float t0 = i * DT, t1 = t0 + DT;
        // Shots inside this tick (sub-tick exact, as recoil_fire_budget).
        while (fired < DG_MAX(1, shots) && next < t1 - 1e-6f) {
            FireShot sh = { ads_t, (next - t0) / DT, 1234u + (u32)fired * 7919u, 30 - fired - 1, interval, 1.0f };
            fire_anim_shot(&s, d, &sh);
            if (t->shots < 256) t->shot_tick[t->shots++] = i;
            fired++;
            next += interval > 0 ? interval : 1e9f;
        }
        bool held = fired < DG_MAX(1, shots);
        FireAnimOut o;
        fire_anim_step(&s, d, DT, held, perceptual, &o);
        t->pitch[i] = o.gun_rot.x;
        t->back[i] = o.gun_pos.z;
        t->hand_pitch[i] = o.gun_rot.x + o.hand_rot.x;
        t->hand_err[i] = v3_len(o.hand_pos);
        t->cam_pitch[i] = o.cam_rot.x;
        t->cam_back[i] = o.cam_back;
        t->fov[i] = o.fov_punch;
        t->blur[i] = o.blur;
        float sum = o.gun_rot.x + o.gun_rot.y + o.gun_rot.z + o.gun_pos.y + o.gun_pos.z + o.cam_rot.x + o.cam_rot.y + o.cam_rot.z + o.hand_pos.z + o.fov_punch;
        CHECK(isfinite(sum), "NaN at tick %d", i);
    }
}

static void test_timing_and_mass(void) {
    for (int c = 0; c < FC_COUNT; c++) {
        FireAnimDef d;
        fire_anim_class_defaults(&d, c);
        Trace t;
        run(&d, 1, 0, 0, 1, &t);
        FaMetrics m = fire_anim_metrics(t.pitch, N, 0.05f);
        int kick = m.peak_tick - t.shot_tick[0];
        int settle = m.settle_tick - m.peak_tick;
        // Kick within 1 tick of the shot; the return / settle at least 10x the kick time; overshoot.
        CHECK(kick >= 0 && kick <= 1, "%s: peak %d ticks after the shot", fire_class_name(c), kick);
        CHECK(settle >= 10 * DG_MAX(1, kick + 1) - 10 && settle >= 10, "%s: settle %d ticks", fire_class_name(c), settle);
        CHECK(m.overshoot > 0.05f, "%s: overshoot %.3f", fire_class_name(c), m.overshoot);
        FaMetrics mb = fire_anim_metrics(t.back, N, 0.05f);
        CHECK(mb.peak_tick - t.shot_tick[0] <= 1, "%s: kickback peak %d ticks after the shot", fire_class_name(c), mb.peak_tick - t.shot_tick[0]);
        // Hands lag the gun by 1-2 ticks, the glue error stays <= 1 cm and is back under 2 mm.
        FaMetrics mh = fire_anim_metrics(t.hand_pitch, N, 0.05f);
        int lag = mh.peak_tick - m.peak_tick;
        CHECK(lag >= 1 && lag <= 2, "%s: hand lag %d ticks", fire_class_name(c), lag);
        float maxe = 0;
        for (int i = 0; i < N; i++) maxe = fmaxf(maxe, t.hand_err[i]);
        CHECK(maxe <= d.hand_max_err + 1e-6f, "%s: glue error %.4f", fire_class_name(c), maxe);
        int glued = -1;
        for (int i = t.shot_tick[0]; i < N; i++) if (t.hand_err[i] < 0.002f) { glued = i - t.shot_tick[0]; break; }
        CHECK(glued >= 0 && glued <= 6, "%s: hands back under 2 mm after %d ticks", fire_class_name(c), glued);
        // The camera reacts after the gun.
        FaMetrics mc = fire_anim_metrics(t.cam_pitch, N, 0.05f);
        CHECK(mc.peak_tick > m.peak_tick + 1, "%s: camera peak tick %d, gun %d", fire_class_name(c), mc.peak_tick, m.peak_tick);
        // Audio transient = the shot tick (fx_sound on the shot): the gun's peak is on the same tick.
        CHECK(m.peak_tick == t.shot_tick[0] || mb.peak_tick == t.shot_tick[0], "%s: peak not on the audio tick", fire_class_name(c));
        printf("  %-8s mass %4.1f  peak %6.2f deg @%d  settle %3d ticks  overshoot %3.0f%%  back %.1f cm  hand lag %d  cam peak +%d\n", fire_class_name(c), d.mass, m.peak,
               kick, settle, m.overshoot * 100, mb.peak * 100, lag, mc.peak_tick - m.peak_tick);

        // Heavier -> smaller peak, longer settle (monotonic).
        float prev_peak = 1e9f;
        int prev_settle = -1;
        for (int k = 0; k < 5; k++) {
            FireAnimDef h = d;
            h.mass = d.ref_mass * (0.6f + 0.4f * (float)k);
            fire_anim_resolve(&h);
            Trace th;
            run(&h, 1, 0, 0, 1, &th);
            FaMetrics mm = fire_anim_metrics(th.pitch, N, 0.05f);
            int st = mm.settle_tick - mm.peak_tick;
            CHECK(fabsf(mm.peak) < prev_peak, "%s: mass %.1f peak %.3f not below %.3f", fire_class_name(c), h.mass, mm.peak, prev_peak);
            CHECK(st >= prev_settle, "%s: mass %.1f settle %d below %d", fire_class_name(c), h.mass, st, prev_settle);
            prev_peak = fabsf(mm.peak);
            prev_settle = st;
        }
    }
}

static void test_accumulation(void) {
    const float rpms[3] = { 600, 900, 1200 };
    int classes[3] = { FC_RIFLE, FC_SMG, FC_LMG };
    for (int ci = 0; ci < 3; ci++)
        for (int r = 0; r < 3; r++) {
            FireAnimDef d;
            fire_anim_class_defaults(&d, classes[ci]);
            Trace t;
            run(&d, 30, rpms[r], 0, 1, &t);
            // Running peak over each of the first 5 shot intervals grows, and never returns to rest.
            float last = 0;
            int grow = 0;
            for (int s = 0; s < 5; s++) {
                int a = t.shot_tick[s], b = t.shot_tick[s + 1];
                float pk = 0;
                for (int i = a; i <= b; i++) pk = fmaxf(pk, t.pitch[i]);
                if (pk > last + 1e-4f) grow++;
                last = fmaxf(last, pk);
            }
            CHECK(grow >= 4, "%s @%.0f rpm: peak grew on %d of the first 5 shots", fire_class_name(classes[ci]), rpms[r], grow);
            float minv = 1e9f;
            for (int i = t.shot_tick[1]; i < t.shot_tick[20]; i++) minv = fminf(minv, t.pitch[i]);
            CHECK(minv > 0.2f, "%s @%.0f rpm: the gun snapped back to rest between shots (%.3f)", fire_class_name(classes[ci]), rpms[r], minv);
        }
}

static void test_perceptual(void) {
    FireAnimDef d;
    fire_anim_class_defaults(&d, FC_RIFLE);
    Trace hi, lo;
    run(&d, 1, 0, 0, 1.0f, &hi);
    run(&d, 1, 0, 0, 0.0f, &lo);
    float fh = 0, bh = 0, fl = 0, bl = 0;
    for (int i = 0; i < N; i++) { fh = fmaxf(fh, hi.fov[i]); bh = fmaxf(bh, hi.blur[i]); fl = fmaxf(fl, lo.fov[i]); bl = fmaxf(bl, lo.blur[i]); }
    CHECK(fh > 1.5f && bh > 0.9f, "FOV punch %.2f / blur %.2f at full fidelity", fh, bh);
    CHECK(fl == 0.0f && bl == 0.0f, "FOV punch %.2f / blur %.2f at Low", fl, bl);
    // FOV punch: hard on the shot tick, released in ~6-8 ticks.
    int s0 = hi.shot_tick[0], rel = 0;
    CHECK(hi.fov[s0] > 0.8f * d.fov_punch, "FOV punch on the shot tick %.2f", hi.fov[s0]);
    for (int i = s0; i < N; i++) if (hi.fov[i] > 0.02f) rel = i - s0;
    CHECK(rel >= 5 && rel <= 8, "FOV release %d ticks", rel);
    int blur_ticks = 0;
    for (int i = 0; i < N; i++) if (hi.blur[i] > 0) blur_ticks++;
    CHECK(blur_ticks >= 1 && blur_ticks <= 2, "blur spike %d ticks", blur_ticks);
}

static void test_attachments(void) {
    FireAnimDef base, sup, brake;
    fire_anim_class_defaults(&base, FC_RIFLE);
    sup = brake = base;
    FaAttMod m;
    const char *sj = "{\"mass\":0.45,\"dist\":0.68,\"impulse\":{\"pitch\":0.85},\"settle_mult\":1.3,\"droop\":1.2}";
    cJSON *j = cJSON_Parse(sj);
    fire_anim_att_parse(&m, j);
    cJSON_Delete(j);
    fire_anim_add_attachment(&sup, &m);
    fire_anim_resolve(&sup);
    j = cJSON_Parse("{\"impulse\":{\"pitch\":0.75,\"yaw\":1.4},\"omega_mult\":1.15,\"zeta_add\":0.1}");
    fire_anim_att_parse(&m, j);
    cJSON_Delete(j);
    fire_anim_add_attachment(&brake, &m);
    fire_anim_resolve(&brake);
    Trace tb, ts, tk;
    run(&base, 1, 0, 0, 1, &tb);
    run(&sup, 1, 0, 0, 1, &ts);
    run(&brake, 1, 0, 0, 1, &tk);
    FaMetrics mb = fire_anim_metrics(tb.pitch, N, 0.05f), ms = fire_anim_metrics(ts.pitch, N, 0.05f), mk = fire_anim_metrics(tk.pitch, N, 0.05f);
    CHECK(ms.peak < mb.peak && ms.settle_tick > mb.settle_tick, "suppressor: peak %.2f vs %.2f, settle %d vs %d", ms.peak, mb.peak, ms.settle_tick, mb.settle_tick);
    CHECK(mk.peak < mb.peak && mk.settle_tick < mb.settle_tick, "brake: peak %.2f vs %.2f, settle %d vs %d", mk.peak, mb.peak, mk.settle_tick, mb.settle_tick);
    CHECK(fire_anim_inertia(&sup, FA_PITCH) > fire_anim_inertia(&base, FA_PITCH), "suppressor adds pitch inertia");
    // Sustained fire with the suppressor droops the muzzle (front heavy): the mean pitch late in a
    // mag dump sits lower than without it, relative to the peak.
    printf("  attachments: base peak %.2f settle %d | suppressor %.2f settle %d | brake %.2f settle %d\n", mb.peak, mb.settle_tick - mb.peak_tick, ms.peak,
           ms.settle_tick - ms.peak_tick, mk.peak, mk.settle_tick - mk.peak_tick);
}

static void test_determinism_and_ads(void) {
    FireAnimDef d;
    fire_anim_class_defaults(&d, FC_RIFLE);
    Trace a, b, ads;
    run(&d, 30, 750, 0, 1, &a);
    run(&d, 30, 750, 0, 1, &b);
    CHECK(!memcmp(&a, &b, sizeof(a)), "mag dump not bit-identical");
    run(&d, 30, 750, 1, 1, &ads);
    float ph = 0, pa = 0;
    for (int i = 0; i < N; i++) { ph = fmaxf(ph, a.pitch[i]); pa = fmaxf(pa, ads.pitch[i]); }
    CHECK(pa < ph * 0.7f, "ADS rotation %.2f vs hip %.2f", pa, ph);
    // ADS: no vertical translation (forward kick + rotation only).
    FireAnimState s;
    fire_anim_reset(&s);
    FireShot sh = { 1.0f, 0, 77, 10, 0, 1.0f };
    fire_anim_shot(&s, &d, &sh);
    float up = 0;
    for (int i = 0; i < 60; i++) {
        FireAnimOut o;
        fire_anim_step(&s, &d, DT, false, 1, &o);
        up = fmaxf(up, fabsf(o.gun_pos.y));
    }
    CHECK(up < 1e-6f, "ADS vertical translation %.5f", up);
}

static void test_mechanical(void) {
    FireAnimDef d;
    fire_anim_class_defaults(&d, FC_PISTOL);
    FireAnimState s;
    fire_anim_reset(&s);
    FireShot sh = { 0, 0, 5, 3, 0, 1.0f };
    fire_anim_shot(&s, &d, &sh);
    FireAnimOut o;
    fire_anim_step(&s, &d, DT, false, 1, &o);
    CHECK(o.slide > d.slide_travel * 0.9f, "slide back on the shot tick %.4f", o.slide);
    int eject = -1;
    for (int i = 1; i < 30; i++) {
        fire_anim_step(&s, &d, DT, false, 1, &o);
        if (fire_anim_take_eject(&s) && eject < 0) eject = i;
    }
    CHECK(eject > 0 && o.slide < 1e-4f, "eject at %d, slide home %.4f", eject, o.slide);
    sh.mag_left = 0;
    fire_anim_shot(&s, &d, &sh);
    for (int i = 0; i < 30; i++) fire_anim_step(&s, &d, DT, false, 1, &o);
    CHECK(o.slide > d.slide_travel * 0.99f, "slide locks back on empty %.4f", o.slide);
}

static void test_json_roundtrip(void) {
    for (int c = 0; c < FC_COUNT; c++) {
        FireAnimDef d, e;
        fire_anim_class_defaults(&d, c);
        char *js = fire_anim_to_json(&d);
        cJSON *j = cJSON_Parse(js);
        fire_anim_class_defaults(&e, (c + 1) % FC_COUNT);
        fire_anim_parse(&e, j);
        e.cls = c;
        fire_anim_resolve(&e);
        cJSON_Delete(j);
        free(js);
        Trace a, b;
        run(&d, 10, 700, 0, 1, &a);
        run(&e, 10, 700, 0, 1, &b);
        float err = 0;
        for (int i = 0; i < N; i++) err = fmaxf(err, fabsf(a.pitch[i] - b.pitch[i]));
        CHECK(err < 1e-3f, "%s: json round trip error %.5f", fire_class_name(c), err);
    }
}

// Sway: the same inputs give bit-identical sway for every weapon / attachment combination (the
// profile is global; nothing weapon-specific reaches sway_eval). 600 ticks of idle, walk, ADS.
static void test_sway_identical(void) {
    SwayProfile prof;
    sway_profile_defaults(&prof);
    float ref[3][N][9];
    for (int weapon = 0; weapon < 6; weapon++) {
        // Fire definitions differ per weapon (class / mass / attachments): sway must not.
        FireAnimDef fd;
        fire_anim_class_defaults(&fd, weapon % FC_COUNT);
        fd.mass *= 1.0f + 0.3f * (float)weapon;
        fire_anim_resolve(&fd);
        for (int mode = 0; mode < 3; mode++) {
            SwayState st;
            sway_reset(&st);
            for (int i = 0; i < N; i++) {
                SwayInput in;
                memset(&in, 0, sizeof(in));
                in.dt = DT;
                in.phase_valid = true;
                in.clock = i * DT;
                in.stride = mode == 1 ? i * DT * 2.2f : 0.0f;
                in.aim_yaw = sinf(i * 0.05f) * 0.4f;
                in.aim_pitch = cosf(i * 0.031f) * 0.2f;
                in.vel_cam = mode == 1 ? v3(0, 0, -4.0f) : v3(0, 0, 0);
                in.grounded = true;
                in.breath_k = mode == 2 ? 1.0f : 1.0f;
                in.sprint = false;
                in.sway_scale = 1.0f;
                in.bob_scale = 1.0f;
                in.look_lag_scale = 0.5f;
                in.optic = 1.0f;
                in.breath_k = 1.0f;
                in.stance = 1.0f;
                SwayOut o;
                sway_eval(&st, &prof, &in, &o);
                float v[9] = { o.hip_rot.x, o.hip_rot.y, o.hip_rot.z, o.hip_pos.x, o.hip_pos.y, o.hip_pos.z, o.ads_rot.x, o.ads_rot.y, o.ads_rot.z };
                if (weapon == 0) memcpy(ref[mode][i], v, sizeof(v));
                else CHECK(!memcmp(ref[mode][i], v, sizeof(v)), "weapon %d mode %d tick %d: sway differs", weapon, mode, i);
            }
        }
    }
}

int main(void) {
    printf("fire animation per class (hip, one shot):\n");
    test_timing_and_mass();
    test_accumulation();
    test_perceptual();
    test_attachments();
    test_determinism_and_ads();
    test_mechanical();
    test_json_roundtrip();
    test_sway_identical();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
