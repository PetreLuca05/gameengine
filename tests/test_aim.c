// Unit tests for the aim model math (gameplay/aim.c, docs/AIM.md): muzzle and barrel claims, the
// uncorrected barrel line, the obstruction rule, muzzle-inside-geometry recovery, the crosshair's spread ring and smoothing.
// Build: part of the normal CMake build (target test_aim); run: ctest or build/bin/test_aim.
#include "gameplay/aim.h"
#include "gameplay/recoil.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// Muzzle sockets (camera space) of the hip and ADS holds of the rifle class: hip beside and below
// the eye, ADS on the view axis under the sight.
static const vec3 k_muzzles[] = { { 0.14f, -0.10f, -0.62f }, { 0.0f, -0.045f, -0.78f }, { 0.11f, -0.13f, -0.35f }, { 0.0f, -0.03f, -0.95f } };

static void test_claim(void) {
    for (int i = 0; i < 4; i++) {
        i16 q[3];
        aim_claim_pack(k_muzzles[i], q);
        CHECK(aim_claim_valid(q), "claim %d valid", i);
        vec3 u = aim_claim_unpack(q);
        CHECK(v3_dist(u, k_muzzles[i]) <= 0.001f, "claim %d round trip %.4f m", i, v3_dist(u, k_muzzles[i]));
    }
    i16 z[3] = { 0, 0, 0 };
    CHECK(!aim_claim_valid(z), "zero claim = none");
    i16 q[3];
    aim_claim_pack(v3(0, 0, 0), q);
    CHECK(aim_claim_valid(q), "a muzzle on the eye still packs as a claim");
}

static void test_frames(void) {
    vec3 eye = v3(3, 1.6f, -7);
    for (int k = 0; k < 16; k++) {
        float yaw = (float)k * 0.41f - 3.0f, pitch = (float)k * 0.09f - 0.7f;
        // Camera forward = the view direction of the simulation.
        vec3 f = v3_sub(aim_cam_to_world(eye, yaw, pitch, v3(0, 0, -1)), eye);
        CHECK(v3_dist(f, dir_from_angles(yaw, pitch)) < 1e-5f, "forward matches dir_from_angles (yaw %.2f pitch %.2f)", yaw, pitch);
        vec3 r = v3_sub(aim_cam_to_world(eye, yaw, pitch, v3(1, 0, 0)), eye);
        CHECK(v3_dist(r, right_from_yaw(yaw)) < 1e-5f, "right matches right_from_yaw");
        vec3 w = aim_cam_to_world(eye, yaw, pitch, k_muzzles[k & 3]);
        CHECK(v3_dist(aim_world_to_cam(eye, yaw, pitch, w), k_muzzles[k & 3]) < 1e-5f, "world <-> camera round trip");
    }
}

static float ang_deg(vec3 a, vec3 b) { return atan2f(v3_len(v3_cross(a, b)), v3_dot(a, b)) * DG_RAD2DEG; }

// No correction: the bullet goes along the barrel's forward axis, whatever the camera sees. Hip
// muzzles keep their natural offset from the camera ray (the same at every distance for a barrel
// parallel to the view), ADS barrels lie along the view axis; every pellet stays in the cone around
// the barrel line; the barrel claim round-trips and turns with the view.
static void test_barrel(void) {
    const float dists[] = { 10.0f, 50.0f, 200.0f };
    for (int m = 0; m < 4; m++) {
        for (int di = 0; di < 3; di++) {
            float yaw = 0.3f * (float)m, pitch = -0.1f + 0.05f * (float)di;
            vec3 eye = v3(0, 1.6f, 0);
            vec3 cam = dir_from_angles(yaw, pitch);
            vec3 mz = aim_cam_to_world(eye, yaw, pitch, k_muzzles[m]);
            // The barrel parallel to the view (camera space -Z), packed and unpacked like the claim.
            i16 c[3];
            aim_dir_pack(v3(0, 0, -1), c);
            CHECK(aim_claim_valid(c), "barrel claim valid");
            vec3 d = quat_rotate(aim_view_rot(yaw, pitch), aim_dir_unpack(c));
            CHECK(ang_deg(d, cam) < 0.01f, "barrel claim round trip along the view");
            // The impact: on the barrel ray, the muzzle's offset from the camera ray kept (no convergence).
            vec3 hit = v3_madd(mz, d, dists[di]);
            vec3 off = v3_sub(hit, v3_madd(eye, cam, v3_dot(v3_sub(hit, eye), cam)));
            vec3 off0 = v3_sub(mz, v3_madd(eye, cam, v3_dot(v3_sub(mz, eye), cam)));
            CHECK(v3_dist(off, off0) < 1e-3f, "muzzle %d at %.0f m: the offset from the camera ray stays %.3f m (no convergence)", m, dists[di], v3_len(off0));
            float cone = 2.2f;
            Rng rng = rng_make(0x51u + (u32)(m * 3 + di));
            float worst = 0.0f;
            for (int s = 0; s < 200; s++) {
                vec3 sd = recoil_cone_dir(d, cone, &rng);
                float t = dists[di] / fmaxf(v3_dot(sd, d), 1e-4f);
                worst = fmaxf(worst, v3_dist(v3_madd(mz, sd, t), hit));
            }
            float allowed = tanf(cone * 0.5f * DG_DEG2RAD) * dists[di] * 1.01f + 1e-3f;
            CHECK(worst <= allowed, "spread at %.0f m: %.3f m off the barrel line, cone radius %.3f m", dists[di], worst, allowed);
        }
    }
    // A canted hip barrel (3 deg in, 1 deg up) packs to within the i16 resolution.
    vec3 cant = v3_norm(v3(-0.0524f, 0.0175f, -1.0f));
    i16 q[3];
    aim_dir_pack(cant, q);
    CHECK(ang_deg(aim_dir_unpack(q), cant) < 0.005f, "canted barrel round trip");
    // The recoil of earlier shots in the tick turns the barrel like the view.
    float yaw = 0.4f, pitch = 0.1f, dy = -0.02f, dp = 0.03f;
    vec3 b = quat_rotate(aim_view_rot(yaw, pitch), cant);
    vec3 b2 = aim_view_delta(b, yaw, pitch, dy, dp);
    vec3 want = quat_rotate(aim_view_rot(yaw + dy, pitch + dp), cant);
    CHECK(v3_dist(b2, want) < 1e-5f, "view delta turns the barrel with the view");
    CHECK(v3_dist(aim_view_delta(b, yaw, pitch, 0, 0), b) < 1e-7f, "no delta, no change");
    // Thrown items still aim from the palm at P (the only convergence left).
    vec3 cam = v3(0, 0, -1);
    CHECK(v3_dist(aim_converge(v3(0, 0, -0.6f), v3(0, 0, -0.3f), cam), cam) < 1e-6f, "P behind the hand keeps the camera direction");
}

// Host check of the barrel direction: in tolerance verbatim, a cheat turned back onto the cone.
static void test_validate_dir(void) {
    vec3 host = v3_norm(v3(-0.03f, 0.01f, -1.0f));
    vec3 near = v3_norm(v3(-0.02f, 0.015f, -1.0f));
    AimValidateDir v = aim_validate_dir(near, true, host, true, 3.0f);
    CHECK(!v.clamped && v.dir.x == near.x && v.dir.y == near.y && v.dir.z == near.z, "barrel claim in tolerance used as is");
    CHECK(v.error_deg > 0.0f && v.error_deg < 1.0f, "barrel claim error %.3f deg", v.error_deg);
    vec3 cheat = v3_norm(v3(0.5f, 0.0f, -1.0f));   // pointing 27 deg off the gun
    v = aim_validate_dir(cheat, true, host, true, 3.0f);
    float ang = ang_deg(v.dir, host);
    CHECK(v.clamped && fabsf(ang - 3.0f) < 0.01f, "cheating barrel claim clamped onto the 3 deg cone (%.3f deg)", ang);
    CHECK(v3_dot(v.dir, cheat) > v3_dot(host, cheat), "clamped towards the claim");
    v = aim_validate_dir(v3(0, 0, 0), false, host, true, 3.0f);
    CHECK(v3_dist(v.dir, host) < 1e-6f, "no claim: the host's barrel");
    v = aim_validate_dir(cheat, true, host, false, 3.0f);
    CHECK(!v.clamped && v3_dist(v.dir, cheat) < 1e-6f, "no host pose: the claim");
}

// The rule without a parallax allowance (a head-on surface).
static AimClass aim_classify_t(float hit, float depth, float far, const AimObstructRule *r, bool friendly) { return aim_classify(hit, depth, far, 0.0f, r, friendly); }

// The obstruction rule (docs/AIM.md "Obstruction").
static void test_classify(void) {
    AimObstructRule r = { 0.25f, 0.75f, 0.05f };
    const float depth = 0.7f;   // the hip muzzle ~0.7 m ahead of the camera
    CHECK(aim_classify_t(-1.0f, depth, 300.0f, &r, false) == AIM_CLEAR, "no barrel hit = clear");
    // The camera sees a target at 50 m, the barrel line hits a wall at 1 m: obstructed.
    float far = aim_obstruct_far(50.0f, 300.0f);
    CHECK(aim_classify_t(1.0f, depth, far, &r, false) == AIM_OBSTRUCTED, "wall at 1 m, target at 50 m = obstructed");
    CHECK(aim_classify_t(0.1f, depth, far, &r, false) == AIM_BARREL, "hit 0.1 m from the muzzle = barrel blocked");
    CHECK(aim_classify_t(0.25f, depth, far, &r, false) == AIM_BARREL, "hit at obstruct_near = barrel blocked");
    // Parallax: the camera ray hits a wall at 40 m, the barrel ray lands on it next to P (hip offset)
    // or slightly short on an oblique face: not an obstruction.
    far = aim_obstruct_far(40.0f, 300.0f);
    CHECK(aim_classify_t(40.0f - depth, depth, far, &r, false) == AIM_CLEAR, "barrel ray beside P at 40 m = clear (parallax)");
    CHECK(aim_classify_t(38.3f - depth, depth, far, &r, false) == AIM_CLEAR, "1.7 m short on an oblique wall at 40 m = clear (5 %% margin)");
    CHECK(aim_classify_t(36.0f - depth, depth, far, &r, false) == AIM_OBSTRUCTED, "4 m short at 40 m = obstructed");
    // Nothing for the camera to see: far = max_range.
    far = aim_obstruct_far(-1.0f, 300.0f);
    CHECK(far == 300.0f, "no camera hit: far = max_range");
    CHECK(aim_classify_t(150.0f, depth, far, &r, false) == AIM_OBSTRUCTED, "barrel hits a pole the open sky camera misses = obstructed");
    // Close range: the camera sees a wall 3 m away, the barrel hits it 2.3 m from the muzzle.
    far = aim_obstruct_far(3.0f, 300.0f);
    CHECK(aim_classify_t(2.3f, depth, far, &r, false) == AIM_CLEAR, "the same wall at 3 m = clear");
    CHECK(aim_classify_t(1.2f, depth, far, &r, false) == AIM_OBSTRUCTED, "a frame 1.9 m ahead of a 3 m wall = obstructed");
    CHECK(fabsf(aim_obstruct_margin(&r, 100.0f, 0.0f) - 5.0f) < 1e-5f && fabsf(aim_obstruct_margin(&r, 2.0f, 0.0f) - 0.75f) < 1e-6f, "margin = max(abs, rel x far)");
    // Grazing: the camera hits the ground 15 m ahead at 6.5 deg; a hip barrel 0.12 m off the camera
    // ray reaches the ground ~1 m sooner. Parallax, not an obstruction; a crate 4 m short still is.
    float sg = sinf(6.5f * DG_DEG2RAD), par = aim_parallax(0.12f, sg, 15.0f);
    CHECK(par > 1.0f && par < 1.1f, "parallax at 6.5 deg: %.3f m", par);
    CHECK(aim_classify(15.0f - 1.05f - depth, depth, 15.0f, par, &r, false) == AIM_CLEAR, "barrel on the ground just short of P = clear");
    CHECK(aim_classify(11.0f - depth, depth, 15.0f, par, &r, false) == AIM_OBSTRUCTED, "a crate 4 m short of P = obstructed");
    CHECK(aim_parallax(0.12f, 1.0f, 15.0f) < 1e-6f && aim_parallax(0.5f, 0.0f, 10.0f) <= 3.0f && aim_parallax(0.5f, 0.0f, 100.0f) <= 25.0f,
          "head on: none; edge on: capped at max(3 m, 25 %% of far)");
    // ADS in the open: terrain 145 m out at a 0.6 deg grazing angle, a barrel 0.07 deg off the ray and
    // 4.5 cm under it lands ~21 m short on the same slope: parallax. A wall 60 m short is not.
    float sep = 0.045f + 145.0f * tanf(0.07f * DG_DEG2RAD);
    par = aim_parallax(sep, sinf(0.6f * DG_DEG2RAD), 145.0f);
    CHECK(aim_classify(124.0f - depth, depth, 145.0f, par, &r, false) == AIM_CLEAR, "far grazing terrain 21 m short = clear (%.1f m allowance)", par);
    CHECK(aim_classify(85.0f - depth, depth, 145.0f, par, &r, false) == AIM_OBSTRUCTED, "a wall 60 m short = obstructed");
    CHECK(aim_classify_t(30.0f, depth, 300.0f, &r, true) == AIM_FRIENDLY, "teammate on the barrel line = friendly");
    CHECK(aim_classify_t(0.1f, depth, 300.0f, &r, true) == AIM_FRIENDLY, "teammate at the barrel = friendly");
    CHECK(!strcmp(aim_class_name(AIM_BARREL), "barrel") && !strcmp(aim_class_name(AIM_CLEAR), "clear") && !strcmp(aim_class_name(AIM_OBSTRUCTED), "obstructed"), "class names");
}

// A muzzle inside a wall (the half-space x > wall) never yields an origin inside it.
static void test_recovery(void) {
    vec3 eye = v3(0, 1.6f, 0);
    int bad = 0;
    for (int k = 0; k < 64; k++) {
        float wall = 0.05f + 0.01f * (float)k;
        float ang = -1.2f + 0.04f * (float)k;
        vec3 mz = v3_add(eye, v3(0.9f * cosf(ang), 0.3f * sinf(ang * 3.0f), 0.9f * sinf(ang)));
        if (mz.x <= wall) continue;   // only muzzles inside the wall
        float f = (wall - eye.x) / (mz.x - eye.x);   // where the eye -> muzzle cast hits it
        vec3 o = aim_recover_origin(eye, mz, f, 0.02f);
        if (o.x >= wall) bad++;
        CHECK(v3_dist(o, eye) <= v3_dist(mz, eye), "recovered origin between the eye and the muzzle");
    }
    CHECK(bad == 0, "%d recovered origins inside the wall", bad);
    CHECK(v3_dist(aim_recover_origin(eye, v3(1, 1.6f, 0), 1.0f, 0.02f), v3(1, 1.6f, 0)) < 1e-6f, "free muzzle unchanged");
    CHECK(v3_dist(aim_recover_origin(eye, v3(1, 1.6f, 0), 0.0f, 0.02f), eye) < 1e-6f, "wall at the eye: the eye");
}

// Host check: in tolerance the client's claim is used verbatim (host and client fire from the same
// bits), out of tolerance it is moved onto the tolerance sphere, and nothing is ever out of reach.
static void test_validate(void) {
    vec3 host = k_muzzles[0];
    vec3 near = v3_add(host, v3(0.05f, 0.02f, -0.03f));
    AimValidate v = aim_validate_muzzle(near, true, host, true, 0.3f, 1.5f);
    CHECK(!v.clamped && v.cam.x == near.x && v.cam.y == near.y && v.cam.z == near.z, "claim in tolerance used as is");
    CHECK(fabsf(v.error - v3_dist(near, host)) < 1e-6f, "claim error reported");
    vec3 cheat = v3_add(host, v3(0.0f, 0.0f, -1.0f));   // a barrel poked a metre forward (round corners)
    v = aim_validate_muzzle(cheat, true, host, true, 0.3f, 1.5f);
    CHECK(v.clamped && fabsf(v3_dist(v.cam, host) - 0.3f) < 1e-4f, "cheating claim clamped onto the tolerance sphere (%.3f m)", v3_dist(v.cam, host));
    v = aim_validate_muzzle(v3(0, 0, -5.0f), true, host, false, 0.3f, 1.5f);
    CHECK(v.clamped && fabsf(v3_len(v.cam) - 1.5f) < 1e-4f, "no host pose: reach cap (%.3f m)", v3_len(v.cam));
    v = aim_validate_muzzle(v3(0, 0, 0), false, host, true, 0.3f, 1.5f);
    CHECK(v3_dist(v.cam, host) < 1e-6f, "no claim: the host's pose");
    // Host and client: the same claim, eye and view give bit-identical origins.
    vec3 eye = v3(12.5f, 1.62f, -3.25f);
    vec3 a = aim_cam_to_world(eye, 0.7f, -0.2f, aim_validate_muzzle(near, true, host, true, 0.3f, 1.5f).cam);
    vec3 b = aim_cam_to_world(eye, 0.7f, -0.2f, near);
    CHECK(a.x == b.x && a.y == b.y && a.z == b.z, "host origin == client origin for an accepted claim");
}

// The spread ring at the obstruction, and the crosshair's move to it.
static void test_crosshair(void) {
    float fov = 75.0f * DG_DEG2RAD, h = 1080.0f;
    for (int i = 1; i <= 4; i++) {
        float spread = 0.5f * (float)i, d = 3.0f * (float)i;
        // Seen from the muzzle's own distance it is the crosshair's cone gap.
        float gap = tanf(spread * 0.5f * DG_DEG2RAD) * (h * 0.5f) / tanf(fov * 0.5f);
        float px = aim_spread_px(spread, d, d, fov, h);
        CHECK(fabsf(px - gap) < 0.01f, "spread ring %.2f px = cone gap %.2f px", px, gap);
    }
    // A wall 0.5 m from the muzzle seen from 0.7 m: a smaller ring than the cone at the eye.
    CHECK(aim_spread_px(2.0f, 0.5f, 0.7f, fov, h) < aim_spread_px(2.0f, 1.0f, 1.0f, fov, h), "closer impact, smaller ring");
    // Smoothing: frame-rate independent and on target (< 1 px of Q) within a second.
    float off60 = 0.0f, off144 = 0.0f, target = 412.0f;
    for (int f = 0; f < 60; f++) off60 += (target - off60) * aim_lerp_k(1.0f / 60.0f, 0.1f);
    for (int f = 0; f < 144; f++) off144 += (target - off144) * aim_lerp_k(1.0f / 144.0f, 0.1f);
    CHECK(fabsf(off60 - target) < 1.0f && fabsf(off144 - target) < 1.0f, "crosshair within 1 px of Q after 1 s (%.3f / %.3f px)", fabsf(off60 - target), fabsf(off144 - target));
    float a60 = 0.0f, a144 = 0.0f;
    for (int f = 0; f < 6; f++) a60 += (target - a60) * aim_lerp_k(1.0f / 60.0f, 0.1f);
    for (int f = 0; f < 14; f++) a144 += (target - a144) * aim_lerp_k(1.0f / 144.0f, 0.1f);
    float a144x = a144 + (target - a144) * (1.0f - expf(-(0.1f - 14.0f / 144.0f) / 0.1f));
    CHECK(fabsf(a60 - a144x) < 1.0f, "0.1 s at 60 / 144 Hz: %.2f vs %.2f px", a60, a144x);
    CHECK(aim_lerp_k(0.016f, 0.0f) == 1.0f, "lerp 0 = snap");
}

int main(void) {
    test_claim();
    test_frames();
    test_barrel();
    test_classify();
    test_recovery();
    test_validate();
    test_validate_dir();
    test_crosshair();
    printf("test_aim: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
