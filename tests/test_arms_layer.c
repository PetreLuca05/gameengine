// First-person arms layer (src/render/arms_layer.c, docs/CAMERA_HOLDPOINT.md "Rule 3"): the arms
// camera is the pose eye; the rigid layer re-projects without touching anything else.
//  1) No presentation / tuning: the arms view is the eye's view matrix (the arms sit where the body has them).
//  2) ADS: the presented sight lands on the screen centre for any Hands FOV, push, roll and scale.
//  3) Tuned offset / rotation / scale move the layer on screen (and nothing in the input changes).
//  4) Equip: at the selection the item (and the forearms around it) is below the arms frustum; after
//     the raise the layer is exactly the hold's.
//  5) Aim: the barrel's vanishing point lands where the world camera shows the shot (sway / kick
//     counter with Hands FOV != world FOV).
//  6) Weapon Field of View: the presets keep the hip sight on its pixel, aimed it stays centred.
//  7) The arms camera moves (never turns) without touching the input; aimed it is back on the eye.
#include "render/arms_layer.h"
#include "player/holdpoint.h"
#include "player/fov.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_fails;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static vec4 mul4(mat4 m, vec4 v) {
    return v4(m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w, m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
              m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w, m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w);
}
static vec2 ndc(mat4 proj, mat4 view, vec3 p) {
    vec4 c = mul4(mat4_mul(proj, view), v4(p.x, p.y, p.z, 1));
    return v2(c.x / c.w, c.y / c.w);
}
static float mdiff(mat4 a, mat4 b) {
    float d = 0;
    for (int i = 0; i < 16; i++) d = fmaxf(d, fabsf(a.m[i] - b.m[i]));
    return d;
}

static ArmsLayerIn base_in(Xform eye) {
    ArmsLayerIn in;
    memset(&in, 0, sizeof(in));
    in.pose_eye = eye;
    in.present = xf_identity();
    in.has_item = true;
    // A rifle at the hip: grip 18 cm right, 22 cm down, 35 cm ahead of the eye; sight 9 cm above the grip.
    in.item_cam = xf_make(v3(0.18f, -0.22f, -0.35f), quat_identity());
    in.pivot = v3(0, 0.01f, -0.15f);
    in.sight = v3(0, 0.09f, -0.04f);
    in.length = 0.9f;
    in.hp_state = HP_HIP;
    in.item_id = "rifle";
    in.equip_t = -1.0f;
    in.fov_world = 75.0f;
    in.fov_hands = 65.0f;
    in.kick_scale = 1.0f;
    in.sway_scale = 1.0f;
    in.dt = 1.0f / 60.0f;
    return in;
}

int main(void) {
    ArmsLayerDef d;
    arms_layer_defaults(&d);
    ArmsLayerState st;
    ArmsLayerOut out;
    float yaw = 0.7f, pitch = -0.3f;
    Xform eye = xf_make(v3(3.0f, 1.62f, -4.0f), quat_euler(yaw, pitch, 0));
    // 1) Identity.
    {
        arms_layer_reset(&st);
        ArmsLayerIn in = base_in(eye);
        arms_layer_eval(&d, &st, &in, &out);
        float e = mdiff(out.view, mat4_fps_view(eye.t, yaw, pitch, 0));
        CHECK(e < 1e-4f, "identity layer: the arms view differs from the eye's by %g", e);
        printf("  identity: arms view = eye view within %.2g\n", e);
    }
    // 2) ADS: presented item = the sight on the axis at 12 cm; any Hands FOV / push / roll / scale.
    {
        float worst = 0;
        for (int k = 0; k < 8; k++) {
            arms_layer_reset(&st);
            ArmsLayerIn in = base_in(eye);
            in.hp_state = HP_ADS;
            in.ads_w = 1.0f;
            Xform presented = xf_make(v3_sub(v3(0, 0, -0.12f), in.sight), quat_identity());   // sight at (0, 0, -0.12)
            in.present = xf_mul(presented, xf_inverse(in.item_cam));
            in.fov_hands = 40.0f + 6.0f * (float)k;
            ArmsPose *p = arms_layer_edit(&d, HP_ADS, NULL);
            p->offset = v3(0.05f, 0.05f, 0.01f * (float)k);   // lateral parts are ignored in ADS
            p->rotation = v3(5, 5, 3.0f * (float)k);
            p->scale = 0.95f + 0.02f * (float)k;
            arms_layer_eval(&d, &st, &in, &out);
            mat4 proj = mat4_perspective(in.fov_hands * DG_DEG2RAD, 16.0f / 9.0f, 0.01f, 100.0f);
            vec3 sight_w = xf_point(eye, xf_point(in.item_cam, in.sight));
            vec2 n = ndc(proj, out.view, sight_w);
            worst = fmaxf(worst, v2_len(n));
        }
        arms_layer_defaults(&d);
        CHECK(worst < 1e-4f, "ADS: the sight is %g (NDC) off the screen centre", worst);
        printf("  ADS: the sight within %.2g NDC of the centre (8 Hands FOVs / pushes / rolls / scales)\n", worst);
    }
    // 3) Tuning moves the layer on screen, the input is untouched.
    {
        arms_layer_reset(&st);
        ArmsLayerIn in = base_in(eye), before;
        before = in;
        arms_layer_eval(&d, &st, &in, &out);
        mat4 proj = mat4_perspective(in.fov_hands * DG_DEG2RAD, 16.0f / 9.0f, 0.01f, 100.0f);
        vec3 grip_w = xf_point(eye, in.item_cam.t);
        vec2 a = ndc(proj, out.view, grip_w);
        ArmsPose *p = arms_layer_edit(&d, HP_HIP, NULL);
        p->offset = v3(-0.02f, 0.01f, 0.03f);
        p->rotation = v3(2, -3, 4);
        p->scale = 1.05f;
        arms_layer_reset(&st);
        arms_layer_eval(&d, &st, &in, &out);
        vec2 b = ndc(proj, out.view, grip_w);
        CHECK(v2_len(v2_sub(a, b)) > 0.01f, "tuning didn't move the layer on screen");
        CHECK(!memcmp(&in, &before, sizeof(in)), "the evaluation changed its input");
        printf("  tuning: the grip moved %.3f NDC on screen; the input unchanged\n", v2_len(v2_sub(a, b)));
        arms_layer_defaults(&d);
    }
    // 4) Equip raise.
    {
        ArmsLayerIn in = base_in(eye);
        mat4 proj = mat4_perspective(in.fov_hands * DG_DEG2RAD, 16.0f / 9.0f, 0.01f, 100.0f);
        arms_layer_reset(&st);
        in.equip_t = 0.0f;
        arms_layer_eval(&d, &st, &in, &out);
        // The item from the muzzle to 30 cm behind the grip (the hands), 5 cm around its axis, is below
        // the bottom edge (screen up = -clip y with this projection; behind the eye: not on screen).
        float highest = -1e9f;
        for (int k = 0; k < 64; k++) {
            float z = -in.length + (in.length + 0.3f) * (float)(k / 4) / 15.0f;
            vec3 p = xf_point(in.item_cam, v3((k & 1) ? 0.05f : -0.05f, (k & 2) ? 0.05f : -0.05f, z));
            vec3 w = xf_point(eye, p);
            vec4 c = mul4(mat4_mul(proj, out.view), v4(w.x, w.y, w.z, 1));
            if (c.w <= 0.0f) continue;
            highest = fmaxf(highest, -c.y / c.w);
        }
        CHECK(highest < -1.0f, "equip start: part of the item is on screen (NDC up %.2f)", highest);
        in.equip_t = d.equip_time + 0.01f;
        ArmsLayerOut done, none;
        arms_layer_eval(&d, &st, &in, &done);
        in.equip_t = -1.0f;
        arms_layer_eval(&d, &st, &in, &none);
        CHECK(mdiff(done.view, none.view) < 1e-6f && done.raise == 0.0f, "after the raise the layer isn't the hold's");
        printf("  equip: the item's highest point starts at NDC %.2f (below -1), the raise ends on the hold\n", highest);
    }
    // 5) Aim correction: the barrel (the layer's forward) on the shot's world pixel.
    {
        float worst = 0;
        for (int k = 0; k < 6; k++) {
            ArmsLayerIn in = base_in(eye);
            in.aim_deg = v2(1.5f - 0.6f * (float)k, -1.0f + 0.5f * (float)k);
            in.aim_pitch = pitch;
            in.fov_hands = 50.0f + 6.0f * (float)k;
            arms_layer_reset(&st);
            arms_layer_eval(&d, &st, &in, &out);
            // The shot in the world: dir(yaw - a, pitch + b).
            float sy = yaw - in.aim_deg.x * DG_DEG2RAD, sp = pitch + in.aim_deg.y * DG_DEG2RAD;
            vec3 dir = quat_rotate(quat_euler(sy, sp, 0), v3(0, 0, -1));
            mat4 wproj = mat4_perspective(in.fov_world * DG_DEG2RAD, 16.0f / 9.0f, 0.04f, 1500.0f);
            vec2 shot = ndc(wproj, mat4_fps_view(eye.t, yaw, pitch, 0), v3_madd(eye.t, dir, 1000.0f));
            // The barrel: a far point along the eye's forward, seen through the arms layer.
            mat4 aproj = mat4_perspective(in.fov_hands * DG_DEG2RAD, 16.0f / 9.0f, 0.01f, 100.0f);
            vec3 fwd = quat_rotate(eye.r, v3(0, 0, -1));
            vec2 barrel = ndc(aproj, out.view, v3_madd(eye.t, fwd, 90.0f));
            worst = fmaxf(worst, v2_len(v2_sub(shot, barrel)));
        }
        CHECK(worst < 2e-3f, "aim: the barrel is %g NDC from the shot's pixel", worst);
        printf("  aim: the barrel within %.2g NDC of the shot's pixel (6 offsets x Hands FOVs)\n", worst);
    }
    // 6) Weapon Field of View: the presets' arms FOVs with the lateral scale keep the hip sight on its
    //    pixel (within 1 % of the screen) and the item's on-screen length; its thickness (a 4 cm
    //    cross-section at the sight) grows with Narrow and shrinks with Wide; aimed, the sight stays centred.
    {
        static const float presets[WEAPON_FOV_COUNT] = { 60.0f, 70.0f, 85.0f };
        float ref_v = fov_h16_to_v(presets[WEAPON_FOV_DEFAULT]);
        vec2 ref_px = v2(0, 0);
        float worst = 0, worst_ads = 0, size[WEAPON_FOV_COUNT];
        for (int w = 0; w < WEAPON_FOV_COUNT; w++) {
            float v = fov_h16_to_v(presets[w]);
            arms_layer_reset(&st);
            ArmsLayerIn in = base_in(eye);
            in.fov_hands = v;
            in.lateral_scale = fov_lateral_scale(v, ref_v);
            arms_layer_eval(&d, &st, &in, &out);
            mat4 proj = mat4_perspective(v * DG_DEG2RAD, 16.0f / 9.0f, 0.01f, 100.0f);
            vec3 sight_w = xf_point(eye, xf_point(in.item_cam, in.sight)), side_w = xf_point(eye, xf_point(in.item_cam, v3_add(in.sight, v3(0.04f, 0, 0))));
            vec2 px = ndc(proj, out.view, sight_w);
            size[w] = v2_len(v2_sub(ndc(proj, out.view, side_w), px));
            if (w == WEAPON_FOV_NARROW) ref_px = px;   // (any preset: they must all agree)
            worst = fmaxf(worst, v2_len(v2_sub(px, ref_px)) * 0.5f);   // NDC -> fraction of the screen
            // Aimed: the presented sight on the axis lands on the centre whatever the preset.
            arms_layer_reset(&st);
            in.hp_state = HP_ADS;
            in.ads_w = 1.0f;
            in.present = xf_mul(xf_make(v3_sub(v3(0, 0, -0.12f), in.sight), quat_identity()), xf_inverse(in.item_cam));
            arms_layer_eval(&d, &st, &in, &out);
            worst_ads = fmaxf(worst_ads, v2_len(ndc(proj, out.view, sight_w)));
        }
        CHECK(worst < 0.01f, "Weapon FOV: the hip sight moves %.4f of the screen across the presets", worst);
        CHECK(size[WEAPON_FOV_NARROW] > size[WEAPON_FOV_DEFAULT] && size[WEAPON_FOV_DEFAULT] > size[WEAPON_FOV_WIDE], "Weapon FOV: Narrow must draw the item larger, Wide smaller (%.3f %.3f %.3f)",
              size[0], size[1], size[2]);
        CHECK(worst_ads < 1e-4f, "Weapon FOV: aimed, the sight is %g NDC off the centre", worst_ads);
        printf("  Weapon FOV: the hip sight within %.2g of the screen across Narrow / Default / Wide (4 cm at the sight %.3f / %.3f / %.3f NDC), aimed %.2g NDC off the centre\n",
               worst, size[0], size[1], size[2], worst_ads);
    }
    // 7) The arms camera's own position (the hip framing): the view moves by it in camera space and
    //    never turns (the eye's rotation), the input is untouched; aimed it is back on the eye, the
    //    presented sight on the screen centre.
    {
        arms_layer_defaults(&d);
        ArmsPose *p = arms_layer_edit(&d, HP_HIP, "rifle");
        p->cam_pos = v3(0.03f, -0.05f, -0.22f);   // right, up, forward: 22 cm back
        arms_layer_reset(&st);
        ArmsLayerIn in = base_in(eye), before = base_in(eye);
        arms_layer_eval(&d, &st, &in, &out);
        mat4 eye_view = mat4_fps_view(eye.t, yaw, pitch, 0);
        // view = translate(-camera) x the eye's view: the same rotation, the camera's offset.
        mat4 want = mat4_mul(mat4_translate(v3(-0.03f, 0.05f, -0.22f)), eye_view);
        float e = mdiff(out.view, want), rot = 0;
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++) rot = fmaxf(rot, fabsf(out.view.m[c * 4 + r] - eye_view.m[c * 4 + r]));
        CHECK(e < 1e-4f, "camera: the arms view is %g off translate(-camera) x the eye's view", e);
        CHECK(rot < 1e-5f, "camera: the arms view turned (%g)", rot);
        CHECK(!memcmp(&in, &before, sizeof(in)), "camera: the input changed");
        // Aimed: back on the eye, the sight centred.
        arms_layer_reset(&st);
        in.hp_state = HP_ADS;
        in.ads_w = 1.0f;
        in.present = xf_mul(xf_make(v3_sub(v3(0, 0, -0.12f), in.sight), quat_identity()), xf_inverse(in.item_cam));
        arms_layer_edit(&d, HP_ADS, "rifle")->cam_pos = v3(0.03f, -0.05f, -0.22f);   // even if the data had one
        arms_layer_eval(&d, &st, &in, &out);
        mat4 proj = mat4_perspective(in.fov_hands * DG_DEG2RAD, 16.0f / 9.0f, 0.01f, 100.0f);
        float off = v2_len(ndc(proj, out.view, xf_point(eye, xf_point(in.item_cam, in.sight))));
        CHECK(off < 1e-4f, "camera: aimed, the sight is %g NDC off the centre", off);
        printf("  camera: the view moves (%.2g off), never turns (%.2g), aimed the sight %.2g NDC off the centre\n", e, rot, off);
        arms_layer_defaults(&d);
    }
    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
