// Unit tests for the aim model math (gameplay/aim.c, docs/AIM.md): muzzle claims, convergence,
// obstruction classes, muzzle-inside-geometry recovery, the crosshair's spread ring and smoothing.
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

// Converged bullets hit the camera ray's aim point (within the spread at that distance).
static void test_convergence(void) {
    const float dists[] = { 10.0f, 50.0f, 200.0f };
    for (int m = 0; m < 4; m++) {
        for (int di = 0; di < 3; di++) {
            float yaw = 0.3f * (float)m, pitch = -0.1f + 0.05f * (float)di;
            vec3 eye = v3(0, 1.6f, 0);
            vec3 cam = dir_from_angles(yaw, pitch);
            vec3 p = v3_madd(eye, cam, dists[di]);
            vec3 mz = aim_cam_to_world(eye, yaw, pitch, k_muzzles[m]);
            vec3 d = aim_converge(mz, p, cam);
            float lp = v3_dist(mz, p);
            vec3 hit = v3_madd(mz, d, lp);
            CHECK(v3_dist(hit, p) < 1e-3f * dists[di], "muzzle %d at %.0f m: converged impact %.4f m from P", m, dists[di], v3_dist(hit, p));
            // ADS muzzles are on the view axis: nearly parallel to the camera ray.
            float ang = acosf(fminf(1.0f, v3_dot(d, cam))) * DG_RAD2DEG;
            if (m == 1 || m == 3) CHECK(ang < 0.3f, "ADS convergence angle %.3f deg at %.0f m", ang, dists[di]);
            // Spread cone around the converged direction: every pellet lands within the cone's radius of P.
            float cone = 2.2f;
            Rng rng = rng_make(0x51u + (u32)(m * 3 + di));
            float worst = 0.0f;
            for (int s = 0; s < 200; s++) {
                vec3 sd = recoil_cone_dir(d, cone, &rng);
                // Intersect with the plane through P facing the bullet.
                float t = lp / fmaxf(v3_dot(sd, d), 1e-4f);
                worst = fmaxf(worst, v3_dist(v3_madd(mz, sd, t), p));
            }
            float allowed = tanf(cone * 0.5f * DG_DEG2RAD) * lp * 1.01f + 1e-3f;
            CHECK(worst <= allowed, "spread at %.0f m: %.3f m off P, cone radius %.3f m", dists[di], worst, allowed);
        }
    }
    // P at / behind the muzzle (aim point inside the barrel's reach): the camera direction.
    vec3 cam = v3(0, 0, -1);
    CHECK(v3_dist(aim_converge(v3(0, 0, -0.6f), v3(0, 0, -0.3f), cam), cam) < 1e-6f, "P behind the muzzle keeps the camera direction");
    CHECK(v3_dist(aim_converge(v3(0, 0, -0.6f), v3(0, 0, -0.61f), cam), cam) < 1e-6f, "P at the muzzle keeps the camera direction");
}

static void test_classify(void) {
    CHECK(aim_classify(-1.0f, 10.0f, 0.25f, false) == AIM_CLEAR, "no hit = clear");
    CHECK(aim_classify(10.0f, 10.0f, 0.25f, false) == AIM_CLEAR, "hit at P = clear");
    CHECK(aim_classify(0.1f, 10.0f, 0.25f, false) == AIM_BARREL, "hit 0.1 m from the muzzle = barrel blocked");
    CHECK(aim_classify(0.3f, 10.0f, 0.25f, false) == AIM_PARTIAL, "hit 0.3 m from the muzzle = partial");
    CHECK(aim_classify(4.0f, 10.0f, 0.25f, false) == AIM_PARTIAL, "corner 4 m along = partial");
    CHECK(aim_classify(4.0f, 10.0f, 0.25f, true) == AIM_FRIENDLY, "teammate = friendly");
    CHECK(aim_classify(0.1f, 10.0f, 0.25f, true) == AIM_FRIENDLY, "teammate at the barrel = friendly");
    CHECK(!strcmp(aim_class_name(AIM_BARREL), "barrel") && !strcmp(aim_class_name(AIM_CLEAR), "clear"), "class names");
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
    test_convergence();
    test_classify();
    test_recovery();
    test_validate();
    test_crosshair();
    printf("test_aim: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
