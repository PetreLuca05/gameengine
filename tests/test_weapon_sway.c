// Viewmodel sway (src/player/weapon_sway.c, docs/FIRE_ANIM.md "Viewmodel sway"):
//  1) Frame-rate independence: the same turn / walk / sprint gives the same offsets at 30, 60 and 144 Hz.
//  2) Look sway: the muzzle trails the turn (yaw left turning right, tilt into it), overshoots once
//     the turn stops and settles; the targets are clamped.
//  3) ADS multipliers: aimed, each layer is its hip value x its multipliers.
//  4) Movement sway: roll into the strafe, shift against it; a sudden stop kicks the spring.
//  5) Bob: the phase follows the ground distance, freezes in the air and standing; the amplitude
//     decays without a snap.
//  6) Sprint pose: eased in over sprint_in, out over sprint_out (faster).
//  7) Config overrides keep the keys they don't name; no NaN at dt 0 / huge dt.
#include "player/weapon_sway.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_fails;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A scripted run: a turn right at 120 deg/s for 1/3 s, then a strafe right + forward walk, a stop, a
// sprint, a stop; sampled every 1/6 s (a whole number of frames at 30, 60 and 144 Hz).
typedef struct Sample { vec3 rot, pos; } Sample;
#define SAMPLES 18   // 3 s

static void scripted(float t, WeaponSwayInput *in, float dt) {
    memset(in, 0, sizeof(*in));
    in->grounded = true;
    in->bob_scale = 1.0f;
    float yaw_rate = t < 1.0f / 3.0f ? 120.0f : 0.0f;
    in->mouse_dx = yaw_rate * dt;
    vec3 v = v3(0, 0, 0);
    if (t >= 0.5f && t < 1.5f) v = v3(2.0f, 0, 3.0f);          // strafe right + forward
    if (t >= 2.0f && t < 2.5f) { v = v3(0, 0, 6.8f); in->sprinting = true; }
    in->velocity_local = v;
    in->velocity = v;
    in->speed = sqrtf(v.x * v.x + v.z * v.z);
}

static void run(float hz, Sample out[SAMPLES]) {
    WeaponSwayConfig c;
    weapon_sway_defaults(&c);
    WeaponSwayState s;
    weapon_sway_reset(&s);
    int n = (int)lrintf(hz * 3.0f), per = (int)lrintf(hz / 6.0f), k = 0;
    float dt = 1.0f / hz;
    for (int i = 0; i < n; i++) {
        WeaponSwayInput in;
        scripted((float)i * dt, &in, dt);
        SwayOutput o;
        weapon_sway_update(&s, &c, &in, dt, &o);
        if ((i + 1) % per == 0 && k < SAMPLES) out[k++] = (Sample){ o.rot_deg, o.pos_offset };
    }
}

static float vmax(vec3 a) { return fmaxf(fabsf(a.x), fmaxf(fabsf(a.y), fabsf(a.z))); }

static void test_rate_independence(void) {
    Sample a[SAMPLES], b[SAMPLES], c[SAMPLES];
    run(30.0f, a);
    run(60.0f, b);
    run(144.0f, c);
    float worst_r = 0, worst_p = 0;
    for (int i = 0; i < SAMPLES; i++) {
        worst_r = fmaxf(worst_r, fmaxf(vmax(v3_sub(a[i].rot, c[i].rot)), vmax(v3_sub(b[i].rot, c[i].rot))));
        worst_p = fmaxf(worst_p, fmaxf(vmax(v3_sub(a[i].pos, c[i].pos)), vmax(v3_sub(b[i].pos, c[i].pos))));
    }
    printf("rate independence: worst rotation %.3f deg, position %.2f mm (30 / 60 vs 144 Hz)\n", worst_r, worst_p * 1000.0f);
    // The input itself is sampled per frame (a 30 Hz frame sees the turn stop up to 1/30 s late at
    // most once): within 0.35 deg / 1 mm of each other.
    CHECK(worst_r < 0.35f, "rotation differs by %.3f deg", worst_r);
    CHECK(worst_p < 0.001f, "position differs by %.2f mm", worst_p * 1000.0f);
}

static void test_look(void) {
    WeaponSwayConfig c;
    weapon_sway_defaults(&c);
    WeaponSwayState s;
    weapon_sway_reset(&s);
    float dt = 1.0f / 60.0f;
    WeaponSwayInput in;
    memset(&in, 0, sizeof(in));
    SwayOutput o;
    // Turn right at 90 deg/s for 0.5 s: yaw left (+), roll clockwise (-), drag left (-x).
    for (int i = 0; i < 30; i++) {
        in.mouse_dx = 90.0f * dt;
        weapon_sway_update(&s, &c, &in, dt, &o);
    }
    vec3 r_turn = o.layer_rot[WSW_LOOK], p_turn = o.layer_pos[WSW_LOOK];
    CHECK(r_turn.y > 1.0f && r_turn.z < -0.5f && p_turn.x < -0.002f, "turning right: yaw %.2f roll %.2f x %.4f", r_turn.y, r_turn.z, p_turn.x);
    // Stop: it swings past zero (the weight) and settles.
    float min_yaw = 1e9f;
    int settled = -1;
    for (int i = 0; i < 120; i++) {
        in.mouse_dx = 0.0f;
        weapon_sway_update(&s, &c, &in, dt, &o);
        min_yaw = fminf(min_yaw, o.layer_rot[WSW_LOOK].y);
        if (settled < 0 && i > 10 && vmax(o.layer_rot[WSW_LOOK]) < 0.02f) settled = i;
    }
    printf("look: turning yaw %.2f roll %.2f deg drag %.1f mm; overshoot %.2f deg; settled in %d frames\n", r_turn.y, r_turn.z, p_turn.x * 1000, -min_yaw, settled);
    CHECK(min_yaw < -0.03f * r_turn.y, "no overshoot after the turn (%.3f)", min_yaw);
    CHECK(min_yaw > -0.5f * r_turn.y, "overshoot too large (%.3f of %.3f)", min_yaw, r_turn.y);
    CHECK(settled > 0 && settled < 60, "settles within 1 s (%d frames)", settled);
    // Clamps: a flick at 2000 deg/s never goes past the clamps (+ the spring's overshoot margin).
    weapon_sway_reset(&s);
    float peak_r = 0, peak_p = 0;
    for (int i = 0; i < 60; i++) {
        in.mouse_dx = 2000.0f * dt;
        in.mouse_dy = 2000.0f * dt;
        weapon_sway_update(&s, &c, &in, dt, &o);
        peak_r = fmaxf(peak_r, vmax(o.layer_rot[WSW_LOOK]));
        peak_p = fmaxf(peak_p, vmax(o.layer_pos[WSW_LOOK]));
    }
    printf("look clamp: peak %.2f deg (max %.1f), %.1f mm (max %.1f)\n", peak_r, c.look_max_rot, peak_p * 1000, c.look_max_pos * 1000);
    CHECK(peak_r <= c.look_max_rot * 1.15f, "rotation %.2f over the clamp", peak_r);
    CHECK(peak_p <= c.look_max_pos * 1.15f, "position %.4f over the clamp", peak_p);
}

static void test_ads(void) {
    WeaponSwayConfig c;
    weapon_sway_defaults(&c);
    WeaponSwayState s0, s1;
    weapon_sway_reset(&s0);
    weapon_sway_reset(&s1);
    float dt = 1.0f / 60.0f;
    SwayOutput o0, o1;
    for (int i = 0; i < 40; i++) {
        WeaponSwayInput in;
        memset(&in, 0, sizeof(in));
        in.mouse_dx = 60.0f * dt;
        in.mouse_dy = -30.0f * dt;
        in.grounded = true;
        in.bob_scale = 1.0f;
        in.velocity_local = v3(2.0f, 0, 2.0f);
        in.speed = sqrtf(8.0f);
        weapon_sway_update(&s0, &c, &in, dt, &o0);
        in.ads_blend = 1.0f;
        weapon_sway_update(&s1, &c, &in, dt, &o1);
    }
    for (int l = 0; l < WSW_LAYERS; l++) {
        float rr = vmax(o1.layer_rot[l]) / fmaxf(vmax(o0.layer_rot[l]), 1e-9f);
        float rp = vmax(o1.layer_pos[l]) / fmaxf(vmax(o0.layer_pos[l]), 1e-9f);
        printf("ads layer %d: rotation x%.3f (want %.2f), position x%.3f (want %.2f)\n", l, rr, c.ads_rot[l], rp, c.ads_pos[l]);
        CHECK(fabsf(rr - c.ads_rot[l]) < 0.01f, "layer %d rotation x%.3f", l, rr);
        CHECK(fabsf(rp - c.ads_pos[l]) < 0.01f, "layer %d position x%.3f", l, rp);
    }
}

static void test_move(void) {
    WeaponSwayConfig c;
    weapon_sway_defaults(&c);
    WeaponSwayState s;
    weapon_sway_reset(&s);
    float dt = 1.0f / 60.0f;
    WeaponSwayInput in;
    memset(&in, 0, sizeof(in));
    in.grounded = true;
    SwayOutput o;
    for (int i = 0; i < 90; i++) {
        in.velocity_local = v3(c.walk_speed, 0, 0);
        in.speed = c.walk_speed;
        weapon_sway_update(&s, &c, &in, dt, &o);
    }
    vec3 r = o.layer_rot[WSW_MOVE], p = o.layer_pos[WSW_MOVE];
    printf("move: strafing right roll %.2f deg, x %.1f mm\n", r.z, p.x * 1000);
    CHECK(fabsf(r.z + c.move_roll) < 0.05f, "roll into the strafe %.2f", r.z);
    CHECK(fabsf(p.x + c.move_strafe_pos) < 0.0005f, "shift against the strafe %.4f", p.x);
    // A sudden stop: the gun carries on to the right (past the rest position) before it settles.
    float max_x = -1e9f;
    for (int i = 0; i < 60; i++) {
        in.velocity_local = v3(0, 0, 0);
        in.speed = 0;
        weapon_sway_update(&s, &c, &in, dt, &o);
        max_x = fmaxf(max_x, o.layer_pos[WSW_MOVE].x);
    }
    printf("move: after the stop the gun swings to %.1f mm (rest 0)\n", max_x * 1000);
    CHECK(max_x > 0.001f, "no settle swing after a sudden stop (%.4f)", max_x);
    // Forward: pushed back, muzzle down.
    weapon_sway_reset(&s);
    for (int i = 0; i < 90; i++) {
        in.velocity_local = v3(0, 0, c.walk_speed);
        in.speed = c.walk_speed;
        weapon_sway_update(&s, &c, &in, dt, &o);
    }
    CHECK(o.layer_pos[WSW_MOVE].z > 0.004f && o.layer_rot[WSW_MOVE].x < -0.5f, "forward: back %.4f, pitch %.2f", o.layer_pos[WSW_MOVE].z, o.layer_rot[WSW_MOVE].x);
}

static void test_bob(void) {
    WeaponSwayConfig c;
    weapon_sway_defaults(&c);
    WeaponSwayState s;
    weapon_sway_reset(&s);
    float dt = 1.0f / 60.0f;
    WeaponSwayInput in;
    memset(&in, 0, sizeof(in));
    in.grounded = true;
    in.bob_scale = 1.0f;
    in.speed = c.walk_speed;
    SwayOutput o;
    double dist = 0;
    float phase_sum = 0, last = 0;
    for (int i = 0; i < 60; i++) {
        weapon_sway_update(&s, &c, &in, dt, &o);
        float d = s.bob_phase - last;
        if (d < 0) d += 2.0f * DG_PI;
        phase_sum += d;
        last = s.bob_phase;
        dist += in.speed * dt;
    }
    float want = (float)(dist * c.bob_freq * 2.0 * DG_PI);
    printf("bob: phase %.3f rad over %.2f m (want %.3f), amplitude %.2f\n", phase_sum, dist, want, s.bob_amt);
    CHECK(fabsf(phase_sum - want) < 1e-3f, "phase follows the distance (%.4f vs %.4f)", phase_sum, want);
    CHECK(s.bob_amt > 0.95f, "walking amplitude %.2f", s.bob_amt);
    // Airborne: the phase freezes, the amplitude decays (no snap: small per-frame change).
    in.grounded = false;
    float ph = s.bob_phase, amp0 = s.bob_amt, worst = 0;
    vec3 prev = o.layer_pos[WSW_BOB];
    for (int i = 0; i < 15; i++) {   // 0.25 s
        weapon_sway_update(&s, &c, &in, dt, &o);
        worst = fmaxf(worst, vmax(v3_sub(o.layer_pos[WSW_BOB], prev)));
        prev = o.layer_pos[WSW_BOB];
    }
    printf("bob: airborne 0.25 s amplitude %.2f -> %.2f, phase %s, worst step %.2f mm\n", amp0, s.bob_amt, s.bob_phase == ph ? "frozen" : "moved", worst * 1000);
    CHECK(s.bob_phase == ph, "phase moved in the air");
    CHECK(s.bob_amt < amp0 * 0.4f && s.bob_amt > amp0 * 0.3f, "decay over 0.25 s: %.2f of %.2f", s.bob_amt, amp0);
    CHECK(worst < 0.002f, "the bob snapped (%.2f mm in a frame)", worst * 1000);
    // Standing still on the ground: frozen too.
    in.grounded = true;
    in.speed = 0;
    ph = s.bob_phase;
    weapon_sway_update(&s, &c, &in, dt, &o);
    CHECK(s.bob_phase == ph, "phase moved standing still");
    // Crouch: half the walking amplitude.
    weapon_sway_reset(&s);
    in.speed = c.walk_speed;
    in.crouching = true;
    for (int i = 0; i < 120; i++) weapon_sway_update(&s, &c, &in, dt, &o);
    CHECK(fabsf(s.bob_amt - c.bob_crouch) < 0.02f, "crouched amplitude %.2f", s.bob_amt);
}

static void test_sprint_pose(void) {
    WeaponSwayConfig c;
    weapon_sway_defaults(&c);
    WeaponSwayState s;
    weapon_sway_reset(&s);
    float dt = 1.0f / 144.0f;
    WeaponSwayInput in;
    memset(&in, 0, sizeof(in));
    in.grounded = true;
    in.sprinting = true;
    SwayOutput o;
    float t_in = -1, t = 0;
    for (int i = 0; i < 144 && t_in < 0; i++) {
        weapon_sway_update(&s, &c, &in, dt, &o);
        t += dt;
        if (o.sprint_w >= 1.0f) t_in = t;
    }
    in.sprinting = false;
    float t_out = -1;
    t = 0;
    for (int i = 0; i < 144 && t_out < 0; i++) {
        weapon_sway_update(&s, &c, &in, dt, &o);
        t += dt;
        if (o.sprint_w <= 0.0f) t_out = t;
    }
    printf("sprint pose: in %.3f s (want %.2f), out %.3f s (want %.2f)\n", t_in, c.sprint_in, t_out, c.sprint_out);
    CHECK(fabsf(t_in - c.sprint_in) <= 1.5f * dt, "in %.3f", t_in);
    CHECK(fabsf(t_out - c.sprint_out) <= 1.5f * dt, "out %.3f", t_out);
    CHECK(t_out < t_in, "out slower than in");
    // Eased: half way in time is half way in weight, the ends are flat.
    weapon_sway_reset(&s);
    in.sprinting = true;
    int half = (int)lrintf(c.sprint_in * 0.5f / dt), first = 1;
    float w_first = 0, w_half = 0;
    for (int i = 0; i < half; i++) {
        weapon_sway_update(&s, &c, &in, dt, &o);
        if (i == first - 1) w_first = o.sprint_w;
    }
    w_half = o.sprint_w;
    CHECK(fabsf(w_half - 0.5f) < 0.05f && w_first < 0.01f, "ease in-out: first frame %.4f, half %.3f", w_first, w_half);
}

static void test_config(void) {
    WeaponSwayConfig c;
    weapon_sway_defaults(&c);
    float k = c.look_k;
    cJSON *j = cJSON_Parse("{\"look\":{\"yaw\":0.05},\"sprint_pose\":{\"rot\":[1,2,3]},\"ads\":{\"bob\":[0.3,0.4]}}");
    weapon_sway_parse(&c, j);
    cJSON_Delete(j);
    CHECK(c.look_yaw == 0.05f && c.look_k == k, "override keeps the rest");
    CHECK(c.sprint_rot.x == 1 && c.sprint_rot.z == 3, "vec3 override");
    CHECK(c.ads_rot[WSW_BOB] == 0.3f && c.ads_pos[WSW_BOB] == 0.4f && c.ads_rot[WSW_LOOK] == 0.2f, "ads override");
    // dt 0 and a huge dt: finite.
    WeaponSwayState s;
    weapon_sway_reset(&s);
    WeaponSwayInput in;
    memset(&in, 0, sizeof(in));
    in.mouse_dx = 50;
    in.grounded = true;
    in.speed = 5;
    SwayOutput o;
    weapon_sway_update(&s, &c, &in, 0.0f, &o);
    weapon_sway_update(&s, &c, &in, 5.0f, &o);
    bool ok = isfinite(o.rot_deg.x) && isfinite(o.rot_deg.y) && isfinite(o.rot_deg.z) && isfinite(o.pos_offset.x) && isfinite(o.pos_offset.y) && isfinite(o.pos_offset.z);
    CHECK(ok, "NaN / inf at dt 0 / 5");
}

int main(void) {
    test_rate_independence();
    test_look();
    test_ads();
    test_move();
    test_bob();
    test_sprint_pose();
    test_config();
    printf("weapon_sway: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
