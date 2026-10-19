// Unit tests for the shot origin math (gameplay/aim.c, docs/AIM.md): the first-person camera claim
// (pack / unpack, the host's tolerance and reach checks), the camera frame, the recovery out of walls.
// Build: part of the normal CMake build (target test_aim); run: ctest or build/bin/test_aim.
#include "gameplay/aim.h"
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

// First-person cameras relative to the simulated eye (camera space): standing, crouched, leaning.
static const vec3 k_cams[] = { { 0.02f, -0.09f, -0.11f }, { 0.0f, 0.30f, -0.05f }, { -0.12f, 0.02f, 0.0f } };

static void test_claim(void) {
    for (int i = 0; i < 3; i++) {
        i16 p[3];
        aim_claim_pack(k_cams[i], p);
        vec3 u = aim_claim_unpack(p);
        CHECK(aim_claim_valid(p) && v3_dist(u, k_cams[i]) < 0.0006f, "claim %d round trip (%.4f m)", i, v3_dist(u, k_cams[i]));
    }
    i16 z[3] = { 0, 0, 0 };
    CHECK(!aim_claim_valid(z), "all zero = no claim");
    i16 p[3];
    aim_claim_pack(v3(0, 0, 0), p);
    CHECK(aim_claim_valid(p), "a camera exactly on the eye still packs as a claim");
}

static void test_validate(void) {
    vec3 host = k_cams[0];
    AimValidate v = aim_validate_claim(v3_add(host, v3(0.03f, 0, 0)), true, host, true, 0.12f, 0.6f);
    CHECK(!v.clamped && fabsf(v.error - 0.03f) < 1e-5f && v3_dist(v.cam, v3_add(host, v3(0.03f, 0, 0))) < 1e-6f, "in tolerance: used as is");
    v = aim_validate_claim(v3_add(host, v3(0.5f, 0, 0)), true, host, true, 0.12f, 0.6f);
    CHECK(v.clamped && fabsf(v3_dist(v.cam, host) - 0.12f) < 1e-4f, "out of tolerance: onto the tolerance sphere (%.3f m)", v3_dist(v.cam, host));
    v = aim_validate_claim(v3(0, 0, -3.0f), true, host, false, 0.12f, 0.6f);
    CHECK(v.clamped && fabsf(v3_len(v.cam) - 0.6f) < 1e-4f, "no host pose: the reach cap (%.3f m)", v3_len(v.cam));
    v = aim_validate_claim(v3(0, 0, 0), false, host, true, 0.12f, 0.6f);
    CHECK(!v.clamped && v3_dist(v.cam, host) < 1e-6f, "no claim: the host's pose");
}

static void test_frames(void) {
    for (int i = 0; i < 16; i++) {
        float yaw = (float)i * 0.41f - 3.0f, pitch = (float)(i % 7) * 0.2f - 0.6f;
        vec3 eye = v3((float)i, 1.6f, -(float)i * 0.5f);
        vec3 w = aim_cam_to_world(eye, yaw, pitch, k_cams[i % 3]);
        vec3 c = aim_world_to_cam(eye, yaw, pitch, w);
        CHECK(v3_dist(c, k_cams[i % 3]) < 1e-5f, "camera frame round trip %d", i);
        vec3 f = v3_sub(aim_cam_to_world(eye, yaw, pitch, v3(0, 0, -1)), eye);
        vec3 d = dir_from_angles(yaw, pitch);
        CHECK(v3_dist(f, d) < 1e-5f, "camera -Z is the view direction %d", i);
    }
}

static void test_recover(void) {
    vec3 eye = v3(0, 1.6f, 0), cam = v3(0, 1.6f, -0.3f);
    CHECK(v3_dist(aim_recover_origin(eye, cam, 1.0f, 0.02f), cam) < 1e-6f, "no hit: the point itself");
    vec3 r = aim_recover_origin(eye, cam, 0.5f, 0.02f);
    CHECK(fabsf(r.z + 0.13f) < 1e-5f, "a wall halfway: 2 cm short of it (%.3f)", r.z);
    CHECK(v3_dist(aim_recover_origin(eye, eye, 0.0f, 0.02f), eye) < 1e-6f, "the eye itself");
}

int main(void) {
    test_claim();
    test_validate();
    test_frames();
    test_recover();
    printf("test_aim: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
