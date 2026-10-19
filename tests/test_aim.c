// Unit tests for the aim model math (gameplay/aim.c, docs/AIM.md): muzzle and barrel claims, the hold
// aim (the gun turned so its barrel line passes through the aim point; the bullet never corrected),
// the obstruction rule, the host's checks against the re-aimed pose, muzzle-inside-geometry recovery,
// the crosshair's spread ring and smoothing.
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

// The rule without a parallax allowance (a head-on surface), the barrel line through P (miss 0).
static AimClass cls_t(float hit, float depth, float far, float parallax, float miss, const AimObstructRule *r, bool friendly, bool recovered) {
    AimClassIn in = { hit, depth, far, parallax, miss, friendly, recovered };
    return aim_classify(&in, r);
}
static AimClass aim_classify_t(float hit, float depth, float far, const AimObstructRule *r, bool friendly) { return cls_t(hit, depth, far, 0.0f, 0.0f, r, friendly, false); }

// The obstruction rule (docs/AIM.md "Obstruction").
static void test_classify(void) {
    AimObstructRule r = { 0.25f, 0.75f, 0.05f };
    const float depth = 0.7f;   // the hip muzzle ~0.7 m ahead of the camera
    CHECK(aim_classify_t(-1.0f, depth, 300.0f, &r, false) == AIM_CLEAR, "no barrel hit, the line through P (the sky) = clear");
    // The camera sees a target at 50 m, the barrel line hits a wall at 1 m: obstructed.
    float far = aim_obstruct_far(50.0f, 300.0f);
    CHECK(aim_classify_t(1.0f, depth, far, &r, false) == AIM_OBSTRUCTED, "wall at 1 m, target at 50 m = obstructed");
    CHECK(aim_classify_t(0.1f, depth, far, &r, false) == AIM_BARREL, "hit 0.1 m from the muzzle = barrel blocked");
    CHECK(aim_classify_t(0.25f, depth, far, &r, false) == AIM_BARREL, "hit at obstruct_near = barrel blocked");
    CHECK(cls_t(3.0f, depth, far, 0.0f, 0.0f, &r, false, true) == AIM_BARREL, "muzzle inside geometry (recovered) = barrel blocked");
    // The line through P: Q is P, whatever the surface (head on, oblique, the ground, far terrain).
    far = aim_obstruct_far(40.0f, 300.0f);
    CHECK(aim_classify_t(40.0f - depth, depth, far, &r, false) == AIM_CLEAR, "barrel line through P at 40 m = clear");
    CHECK(aim_classify_t(38.3f - depth, depth, far, &r, false) == AIM_CLEAR, "1.7 m short on an oblique wall at 40 m = clear (5 %% margin)");
    CHECK(aim_classify_t(36.0f - depth, depth, far, &r, false) == AIM_OBSTRUCTED, "4 m short at 40 m = obstructed");
    float sg = sinf(6.5f * DG_DEG2RAD);
    CHECK(cls_t(15.0f - depth, depth, 15.0f, aim_parallax(0.0f, sg, 15.0f), 0.0f, &r, false, false) == AIM_CLEAR, "the ground 15 m ahead at 6.5 deg, the line through P = clear");
    CHECK(cls_t(145.0f - depth, depth, 145.0f, aim_parallax(0.0f, sinf(0.6f * DG_DEG2RAD), 145.0f), 0.0f, &r, false, false) == AIM_CLEAR,
          "far terrain 145 m out at 0.6 deg, the line through P = clear");
    // Nothing for the camera to see: far = max_range.
    far = aim_obstruct_far(-1.0f, 300.0f);
    CHECK(far == 300.0f, "no camera hit: far = max_range");
    CHECK(aim_classify_t(150.0f, depth, far, &r, false) == AIM_OBSTRUCTED, "barrel hits a pole the open sky camera misses = obstructed");
    CHECK(cls_t(-1.0f, depth, 300.0f, 0.0f, 0.001f, &r, false, false) == AIM_CLEAR, "sky, the line 1 mm off P = clear");
    // The line misses P (the gun was moved): obstructed even without a closer hit.
    CHECK(cls_t(-1.0f, depth, 50.0f, 0.0f, 0.4f, &r, false, false) == AIM_OBSTRUCTED, "no barrel hit, the line 0.4 m off a 50 m P = obstructed");
    CHECK(cls_t(10.0f - depth, depth, 10.0f, 0.0f, 0.3f, &r, false, false) == AIM_OBSTRUCTED, "pulled gun: lands on the 10 m wall 0.3 m off P = obstructed");
    CHECK(cls_t(1.3f - depth, depth, 1.3f, 0.0f, 0.02f, &r, false, false) == AIM_OBSTRUCTED, "1.3 m wall, the hold short of P by 2 cm = obstructed");
    // The reach tolerance: 5 mm close, 0.1 deg far.
    CHECK(fabsf(aim_reach_tol(2.0f) - 0.005f) < 1e-6f, "tolerance at 2 m = 5 mm (%.4f)", aim_reach_tol(2.0f));
    CHECK(fabsf(aim_reach_tol(300.0f) - 300.0f * tanf(0.1f * DG_DEG2RAD)) < 1e-4f && aim_reach_tol(300.0f) > 0.52f, "tolerance at 300 m = 0.52 m (%.4f)", aim_reach_tol(300.0f));
    CHECK(cls_t(-1.0f, depth, 2.0f, 0.0f, 0.0049f, &r, false, false) == AIM_CLEAR && cls_t(-1.0f, depth, 2.0f, 0.0f, 0.0051f, &r, false, false) == AIM_OBSTRUCTED,
          "the 5 mm tolerance at 2 m");
    // Close range: the camera sees a wall 3 m away, the barrel hits it 2.3 m from the muzzle.
    far = aim_obstruct_far(3.0f, 300.0f);
    CHECK(aim_classify_t(2.3f, depth, far, &r, false) == AIM_CLEAR, "the same wall at 3 m = clear");
    CHECK(aim_classify_t(1.2f, depth, far, &r, false) == AIM_OBSTRUCTED, "a frame 1.9 m ahead of a 3 m wall = obstructed");
    CHECK(fabsf(aim_obstruct_margin(&r, 100.0f, 0.0f) - 5.0f) < 1e-5f && fabsf(aim_obstruct_margin(&r, 2.0f, 0.0f) - 0.75f) < 1e-6f, "margin = max(abs, rel x far)");
    // Parallax of a line off P on a grazing surface (wpn_hold_aim_max 0, or past the limit): the
    // depth rule allows it, the miss rule still shows where the bullet goes.
    float par = aim_parallax(0.12f, sg, 15.0f);
    CHECK(par > 1.0f && par < 1.1f, "parallax at 6.5 deg: %.3f m", par);
    CHECK(cls_t(15.0f - 1.05f - depth, depth, 15.0f, par, 0.0f, &r, false, false) == AIM_CLEAR, "on the ground just short of P (depth rule) = clear");
    CHECK(cls_t(15.0f - 1.05f - depth, depth, 15.0f, par, 0.12f, &r, false, false) == AIM_OBSTRUCTED, "... but 12 cm off P = obstructed (miss rule)");
    CHECK(cls_t(11.0f - depth, depth, 15.0f, par, 0.0f, &r, false, false) == AIM_OBSTRUCTED, "a crate 4 m short of P = obstructed");
    CHECK(aim_parallax(0.12f, 1.0f, 15.0f) < 1e-6f && aim_parallax(0.5f, 0.0f, 10.0f) <= 3.0f && aim_parallax(0.5f, 0.0f, 100.0f) <= 25.0f,
          "head on: none; edge on: capped at max(3 m, 25 %% of far)");
    CHECK(aim_classify_t(30.0f, depth, 300.0f, &r, true) == AIM_FRIENDLY, "teammate on the barrel line = friendly");
    CHECK(aim_classify_t(0.1f, depth, 300.0f, &r, true) == AIM_FRIENDLY, "teammate at the barrel = friendly (before barrel)");
    CHECK(cls_t(-1.0f, depth, 300.0f, 0.0f, 0.0f, &r, true, false) == AIM_CLEAR, "no hit, no teammate");
    CHECK(!strcmp(aim_class_name(AIM_BARREL), "barrel") && !strcmp(aim_class_name(AIM_CLEAR), "clear") && !strcmp(aim_class_name(AIM_OBSTRUCTED), "obstructed"), "class names");
}

// ------------------------------------------------------------------ hold aim

// Hold geometries (camera space), built like the hold point: the grip at the style's hip position,
// the item turned by its yaw (toe-in), the muzzle / pivot sockets in item space.
typedef struct HoldGeo { const char *name; vec3 grip; float yaw_deg; vec3 muzzle, pivot; } HoldGeo;
static const HoldGeo k_geo[] = {
    { "rifle", { 0.11f, -0.19f, -0.18f }, 1.5f, { 0.0f, 0.06f, -0.62f }, { 0.0f, 0.01f, -0.15f } },
    { "pistol", { 0.09f, -0.17f, -0.36f }, 2.0f, { 0.0f, 0.075f, -0.19f }, { 0.0f, 0.0f, 0.05f } },
    { "lmg+suppressor", { 0.11f, -0.22f, -0.16f }, 1.5f, { 0.0f, 0.06f, -0.90f }, { 0.0f, 0.01f, -0.17f } },
};

static AimHoldRaw geo_raw(const HoldGeo *g, float max_deg) {
    quat r = quat_axis_angle(v3(0, 1, 0), g->yaw_deg * DG_DEG2RAD);
    AimHoldRaw raw;
    raw.muzzle = v3_add(g->grip, quat_rotate(r, g->muzzle));
    raw.barrel = quat_rotate(r, v3(0, 0, -1));
    raw.pivot = v3_add(g->grip, quat_rotate(r, g->pivot));
    raw.w = 1.0f;
    raw.max_rad = max_deg * DG_DEG2RAD;
    raw.valid = true;
    return raw;
}

// Double precision reference: the converged fixed point (the exact turn), and the clamped line's miss.
typedef struct { double x, y, z; } d3;
static d3 D3(vec3 v) { d3 r = { v.x, v.y, v.z }; return r; }
static d3 dsub(d3 a, d3 b) { d3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static d3 dadd(d3 a, d3 b) { d3 r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static d3 dsc(d3 a, double s) { d3 r = { a.x * s, a.y * s, a.z * s }; return r; }
static double ddot(d3 a, d3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static d3 dcross(d3 a, d3 b) { d3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }
static d3 dnorm(d3 a) { return dsc(a, 1.0 / sqrt(ddot(a, a))); }
// v turned by the shortest rotation taking unit a onto unit b (Rodrigues).
static d3 drot(d3 a, d3 b, d3 v) {
    d3 k = dcross(a, b);
    double s = sqrt(ddot(k, k)), c = ddot(a, b);
    if (s < 1e-15) return v;
    k = dsc(k, 1.0 / s);
    return dadd(dadd(dsc(v, c), dsc(dcross(k, v), s)), dsc(k, ddot(k, v) * (1.0 - c)));
}
static double dang(d3 a, d3 b) { return atan2(sqrt(ddot(dcross(a, b), dcross(a, b))), ddot(a, b)); }
// The exact direction u: P on {C + R(u) b + s u}.
static d3 ref_u(const AimHoldRaw *raw, vec3 p) {
    d3 C = D3(raw->pivot), B = D3(raw->barrel), mc = dsub(D3(raw->muzzle), C);
    d3 b = dsub(mc, dsc(B, ddot(mc, B))), u = B;
    for (int i = 0; i < 80; i++) u = dnorm(dsub(dsub(D3(p), C), drot(B, u, b)));
    return u;
}
static double ref_miss(const AimHoldRaw *raw, d3 u, vec3 p) {
    d3 C = D3(raw->pivot), B = D3(raw->barrel);
    d3 m = dadd(C, drot(B, u, dsub(D3(raw->muzzle), C))), v = dsub(D3(p), m);
    double t = ddot(v, u);
    if (t < 0) t = 0;
    d3 off = dsub(v, dsc(u, t));
    return sqrt(ddot(off, off));
}
static d3 dturn(d3 from, d3 to, double ang) {
    d3 perp = dsub(to, dsc(from, ddot(to, from)));
    return dnorm(dadd(dsc(from, cos(ang)), dsc(dnorm(perp), sin(ang))));
}

static void test_hold_aim(void) {
    const float dists[] = { 2.0f, 5.0f, 25.0f, 100.0f, 300.0f };
    for (int g = 0; g < 3; g++) {
        AimHoldRaw raw = geo_raw(&k_geo[g], 12.0f);
        float worst_miss = 0.0f, worst_ang = 0.0f;
        for (int di = 0; di < 7; di++) {
            // On the axis at 2 .. 300 m, and two off-axis points (a target low left at 8 m, high right at 40 m).
            vec3 p = di < 5 ? v3(0, 0, -dists[di]) : (di == 5 ? v3(-0.9f, -0.6f, -8.0f) : v3(2.5f, 1.8f, -40.0f));
            AimHold h = aim_hold_solve(&raw, p);
            AimHold h2 = aim_hold_solve(&raw, p);
            CHECK(!memcmp(&h, &h2, sizeof(h)), "%s: repeat solves bit-identical", k_geo[g].name);
            double want = dang(D3(raw.barrel), ref_u(&raw, p)) * (180.0 / 3.14159265358979323846);
            worst_miss = fmaxf(worst_miss, h.miss);
            worst_ang = fmaxf(worst_ang, fabsf(h.angle_deg - (float)want));
            CHECK(!h.clamped && h.miss < 1e-4f, "%s at %.1f m: the barrel line passes %.4f mm from P", k_geo[g].name, v3_len(p), h.miss * 1000.0f);
            CHECK(fabsf(h.angle_deg - (float)want) < 0.01f, "%s at %.1f m: turned %.4f deg, exact %.4f", k_geo[g].name, v3_len(p), h.angle_deg, want);
            // The bullet: along the aimed barrel, no spread: it lands on P.
            vec3 hit = v3_madd(h.muzzle, h.barrel, v3_dot(v3_sub(p, h.muzzle), h.barrel));
            CHECK(v3_dist(hit, p) < 1e-4f, "%s at %.1f m: the shot lands %.4f mm from P", k_geo[g].name, v3_len(p), v3_dist(hit, p) * 1000.0f);
            vec3 sd = recoil_cone_dir(h.barrel, 0.0f, NULL);
            CHECK(sd.x == h.barrel.x && sd.y == h.barrel.y && sd.z == h.barrel.z, "cone 0: the barrel");
            // Pellets: a cone around the barrel line, i.e. around P.
            Rng rng = rng_make(0x77u + (u32)(g * 11 + di));
            float worst = 0.0f, cone = 2.2f, dpm = v3_dist(p, h.muzzle);
            for (int k = 0; k < 64; k++) {
                vec3 pd = recoil_cone_dir(h.barrel, cone, &rng);
                float t = v3_dot(v3_sub(p, h.muzzle), h.barrel) / fmaxf(v3_dot(pd, h.barrel), 1e-4f);
                worst = fmaxf(worst, v3_dist(v3_madd(h.muzzle, pd, t), p));
            }
            CHECK(worst <= tanf(cone * 0.5f * DG_DEG2RAD) * dpm * 1.01f + 1e-3f, "%s at %.1f m: pellets %.3f m from P", k_geo[g].name, v3_len(p), worst);
            // The claim as packed into the UserCmd (mm, unit x 32767) still ends at P.
            i16 qm[3], qd[3];
            aim_claim_pack(h.muzzle, qm);
            aim_dir_pack(h.barrel, qd);
            float pm = aim_line_miss(aim_claim_unpack(qm), aim_dir_unpack(qd), p);
            float lim = di == 0 ? 0.001f : aim_reach_tol(v3_len(p));
            CHECK(pm < lim, "%s at %.1f m: the packed claim passes %.2f mm from P (limit %.2f)", k_geo[g].name, v3_len(p), pm * 1000.0f, lim * 1000.0f);
        }
        printf("  hold aim %-15s 2 .. 300 m + off axis: miss <= %.4f mm, angle within %.5f deg of the exact turn\n", k_geo[g].name, worst_miss * 1000.0f, worst_ang);
    }
    // The rifle's turn by distance (the docs' table).
    {
        AimHoldRaw raw = geo_raw(&k_geo[0], 12.0f);
        printf("  hold aim rifle hip turn:");
        for (int di = 0; di < 5; di++) printf(" %.0f m %.2f deg", dists[di], aim_hold_solve(&raw, v3(0, 0, -dists[di])).angle_deg);
        printf("\n");
    }
    // A planar case against the closed form: theta = phi - asin(beta / |d|).
    {
        AimHoldRaw raw = { { 0.0f, -0.10f, -0.8f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, -0.15f, -0.3f }, 1.0f, 20.0f * DG_DEG2RAD, true };
        for (int di = 0; di < 5; di++) {
            double D = dists[di], dz = D - 0.3, dy = 0.15, dl = sqrt(dz * dz + dy * dy);
            double theta = atan2(dy, dz) - asin(0.05 / dl);
            AimHold h = aim_hold_solve(&raw, v3(0, 0, -dists[di]));
            CHECK(fabsf(h.angle_deg - (float)(theta * 180.0 / 3.14159265358979323846)) < 0.01f, "planar at %.0f m: %.4f deg, closed form %.4f", D, h.angle_deg,
                  theta * 180.0 / 3.14159265358979323846);
        }
    }
    // ADS: the gun turns about the sight (on the view axis); the bore 5.2 cm under it reaches P.
    {
        vec3 sight_item = v3(0.0f, 0.112f, 0.04f), grip = v3_sub(v3(0, 0, -0.24f), sight_item);
        AimHoldRaw raw = { v3_add(grip, v3(0.0f, 0.06f, -0.62f)), v3(0, 0, -1), v3(0, 0, -0.24f), 1.0f, AIM_HOLD_ADS_MAX_DEG * DG_DEG2RAD, true };
        float worst = 0.0f;
        for (int di = 0; di < 5; di++) {
            AimHold h = aim_hold_solve(&raw, v3(0, 0, -dists[di]));
            vec3 sp = v3_add(raw.pivot, quat_rotate(h.rot, v3_sub(v3(0, 0, -0.24f), raw.pivot)));
            CHECK(sp.x == 0.0f && sp.y == 0.0f && sp.z == -0.24f, "ADS at %.0f m: the sight stays on the axis bit-exact", dists[di]);
            CHECK(h.miss < 1e-4f && h.angle_deg <= AIM_HOLD_ADS_MAX_DEG && !h.clamped, "ADS at %.0f m: %.4f mm from P, turned %.3f deg", dists[di], h.miss * 1000.0f, h.angle_deg);
            worst = fmaxf(worst, h.miss);
        }
        printf("  hold aim ADS irons: 2 m %.2f deg, 25 m %.3f deg, miss <= %.4f mm\n", aim_hold_solve(&raw, v3(0, 0, -2.0f)).angle_deg,
               aim_hold_solve(&raw, v3(0, 0, -25.0f)).angle_deg, worst * 1000.0f);
    }
    // Past the limit (1.2 m needs ~10 deg; an 8 deg limit): the turn stops there, the line misses P by
    // the exact amount.
    {
        AimHoldRaw raw = geo_raw(&k_geo[0], 8.0f);
        vec3 p = v3(0, 0, -1.2f);
        AimHold h = aim_hold_solve(&raw, p);
        d3 uc = dturn(D3(raw.barrel), ref_u(&raw, p), 8.0 * 3.14159265358979323846 / 180.0);
        double want = ref_miss(&raw, uc, p);
        CHECK(h.clamped && fabsf(h.angle_deg - 8.0f) < 1e-3f, "1.2 m: clamped at %.4f deg", h.angle_deg);
        CHECK(fabs(h.miss - want) < 5e-4, "1.2 m: misses P by %.2f mm, exact %.2f mm", h.miss * 1000.0f, want * 1000.0);
        CHECK(h.miss > aim_reach_tol(1.2f), "1.2 m: beyond the reach tolerance (obstructed)");
        // P behind the muzzle (or within 5 cm ahead of it): no turn.
        AimHold hb = aim_hold_solve(&raw, v3(0, 0, -0.5f));
        CHECK(hb.angle_deg == 0.0f && hb.rot.w == 1.0f && hb.muzzle.x == raw.muzzle.x && hb.barrel.z == raw.barrel.z, "P behind the muzzle: identity");
        // Weight: none, half.
        AimHoldRaw r0 = raw, r5 = raw;
        r0.w = 0.0f;
        r5.w = 0.5f;
        AimHold full = aim_hold_solve(&raw, v3(0, 0, -5.0f)), h0 = aim_hold_solve(&r0, v3(0, 0, -5.0f)), h5 = aim_hold_solve(&r5, v3(0, 0, -5.0f));
        CHECK(h0.angle_deg == 0.0f && h0.rot.w == 1.0f && h0.muzzle.y == raw.muzzle.y, "w 0: identity");
        CHECK(fabsf(h5.angle_deg - full.angle_deg * 0.5f) < 1e-3f, "w 0.5: half the turn (%.4f of %.4f deg)", h5.angle_deg, full.angle_deg);
        AimHoldRaw ri = raw;
        ri.valid = false;
        CHECK(aim_hold_solve(&ri, v3(0, 0, -5.0f)).angle_deg == 0.0f, "invalid raw: identity");
        CHECK(fabsf(aim_line_miss(v3(0, 0, 0), v3(0, 0, -1), v3(0.3f, 0, -4)) - 0.3f) < 1e-6f && fabsf(aim_line_miss(v3(0, 0, 0), v3(0, 0, -1), v3(0, 0.4f, 1)) - sqrtf(1.16f)) < 1e-6f,
              "line miss: perpendicular distance, behind the origin the distance to it");
    }
}

// Host check of the hold-aimed claims (weapon_aim_solve): against the pose re-aimed at the host's P,
// else at the shooter's own aim_dist along its own camera ray; a cheat is off every point of the ray.
static void test_host_hold(void) {
    AimHoldRaw raw = geo_raw(&k_geo[0], 12.0f);
    vec3 eye_cam = v3(0, 0, 0);
    // The client saw a crate at 2 m, the host (lag) a wall at 20 m.
    AimHold claim = aim_hold_solve(&raw, v3(0, 0, -2.0f)), ref = aim_hold_solve(&raw, v3(0, 0, -20.0f));
    AimValidateDir vd = aim_validate_dir(claim.barrel, true, ref.barrel, true, 3.0f);
    CHECK(vd.clamped, "2 m claim vs the 20 m reference: %.2f deg apart", vd.error_deg);
    AimHold alt = aim_hold_solve(&raw, v3_madd(eye_cam, v3(0, 0, -1), 2.0f));
    AimValidateDir va = aim_validate_dir(claim.barrel, true, alt.barrel, true, 3.0f);
    CHECK(!va.clamped && va.dir.x == claim.barrel.x && va.dir.y == claim.barrel.y && va.dir.z == claim.barrel.z, "... at the client's own 2 m: used as is (%.4f deg)", va.error_deg);
    AimValidate vm = aim_validate_muzzle(claim.muzzle, true, ref.muzzle, true, 0.3f, 1.5f);
    CHECK(!vm.clamped && vm.error < 0.1f, "the muzzle vs the re-aimed reference: %.1f mm (in tolerance)", vm.error * 1000.0f);
    // A cheat: a barrel 6 deg right of the gun aimed anywhere on the ray. Off every point's pose by > 3 deg.
    vec3 cheat = quat_rotate(quat_axis_angle(v3(0, 1, 0), -6.0f * DG_DEG2RAD), ref.barrel);
    bool off_all = true;
    for (float dd = 1.0f; dd < 400.0f; dd *= 1.25f)
        if (!aim_validate_dir(cheat, true, aim_hold_solve(&raw, v3(0, 0, -dd)).barrel, true, 3.0f).clamped) off_all = false;
    CHECK(off_all, "a 6 deg cheat is out of tolerance of the pose aimed at every point of the camera ray");
    AimValidateDir vc = aim_validate_dir(cheat, true, ref.barrel, true, 3.0f);
    CHECK(vc.clamped && fabsf(ang_deg(vc.dir, ref.barrel) - 3.0f) < 0.01f, "the cheat clamped onto the 3 deg cone (%.3f deg)", ang_deg(vc.dir, ref.barrel));
    // Host and client fire the same bits for an accepted claim.
    vec3 eye = v3(12.5f, 1.62f, -3.25f);
    i16 q[3];
    aim_dir_pack(claim.barrel, q);
    vec3 dc = aim_dir_unpack(q);
    vec3 a = quat_rotate(aim_view_rot(0.7f, -0.2f), aim_validate_dir(dc, true, alt.barrel, true, 3.0f).dir), b = quat_rotate(aim_view_rot(0.7f, -0.2f), dc);
    CHECK(a.x == b.x && a.y == b.y && a.z == b.z, "host direction == client direction for an accepted claim");
    (void)eye;
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
    test_hold_aim();
    test_host_hold();
    test_recovery();
    test_validate();
    test_validate_dir();
    test_crosshair();
    printf("test_aim: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
