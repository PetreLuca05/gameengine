// Hip-fire vs ADS recoil (docs/RECOIL.md "Hip vs ADS"): the per-type hip multipliers give their
// configured ratios and blend with the aim progress; in ADS the hold point turns the gun about the
// sight socket, so over 600 ticks of full-auto kick at the maximum scale the sight stays on the
// view axis, for every weapon x optic in the shipped data. Run from the build output directory.
#include "gameplay/recoil.h"
#include "gameplay/items.h"
#include "gameplay/attachments.h"
#include "player/holdpoint.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
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

static RecoilMods mods(float ads_t) {
    RecoilMods m;
    memset(&m, 0, sizeof(m));
    m.fire_mode = FM_AUTO;
    m.extra = 1;
    m.scale = 1;
    m.ads_t = ads_t;
    return m;
}

static void test_ratios(void) {
    for (int t = RT_RIFLE; t <= RT_SNIPER; t++) {
        RecoilParams p;
        recoil_params_default(&p);
        recoil_type_defaults(&p, (RecoilType)t);
        RecoilMods hip = mods(0), ads = mods(1);
        float ah = recoil_aim_mult(&p, &hip), aa = recoil_aim_mult(&p, &ads);
        CHECK(fabsf(ah - p.hip_aim_mult) < 1e-5f, "type %d: hip aim mult %.3f == %.3f", t, ah, p.hip_aim_mult);
        CHECK(fabsf(aa - p.ads_mult * p.ads_aim_mult) < 1e-5f, "type %d: ADS aim as configured", t);
        CHECK(p.hip_aim_mult < 1.0f, "type %d: the camera climbs less from the hip", t);
        RecoilState s;
        memset(&s, 0, sizeof(s));
        float sh = recoil_spread(&p, &s, &hip), sa = recoil_spread(&p, &s, &ads);
        CHECK(fabsf(sh - p.base_hip * p.hip_spread_mult) < 1e-4f && fabsf(sa - p.base_ads) < 1e-4f, "type %d: spread hip / ADS", t);
        // Bloom: a hip mag-dump opens the cone wider than an ADS one.
        RecoilState bh, ba;
        memset(&bh, 0, sizeof(bh));
        memset(&ba, 0, sizeof(ba));
        for (int k = 0; k < 30; k++) { recoil_shot(&p, &bh, &hip); recoil_shot(&p, &ba, &ads); }
        CHECK(bh.bloom > ba.bloom * 1.1f || p.hip_bloom_max_mult <= 1.0f, "type %d: hip bloom %.2f > ADS %.2f", t, bh.bloom, ba.bloom);
        CHECK(bh.bloom <= recoil_bloom_max(&p, 0) + 1e-4f, "type %d: hip bloom capped", t);
        // Continuous blend: no jump from hip to ADS.
        float prev = ah, worst = 0;
        for (int k = 1; k <= 100; k++) {
            RecoilMods m = mods((float)k / 100.0f);
            float v = recoil_aim_mult(&p, &m);
            worst = fmaxf(worst, fabsf(v - prev));
            prev = v;
        }
        CHECK(worst < 0.02f, "type %d: aim multiplier blends smoothly (step %.4f)", t, worst);
        CHECK(fabsf(recoil_kick_mult(&p, 0) - p.hip_kick_mult) < 1e-6f && fabsf(recoil_kick_mult(&p, 1) - p.vm_ads_scale) < 1e-6f, "type %d: kick mults", t);
    }
}

static u32 rng = 777u;
static float frand(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 16777216.0f;
}

// ADS: 600 ticks of random full-auto kicks at the maximum scale; the sight socket stays on the
// view axis (x = y = 0 in camera space) once the hold point has blended in.
static float ads_kick_run(const WeaponDef *eff, const HoldPointStyle *style, bool iron, float *worst_z) {
    HoldPointState st;
    memset(&st, 0, sizeof(st));
    HoldPointInput in;
    memset(&in, 0, sizeof(in));
    in.style = style;
    in.ovr = &eff->sockets.hp;
    in.anchor = xf_make(eff->sockets.anchor.t, quat_identity());
    in.sight = eff->sockets.sight;
    in.pivot = eff->sockets.pivot;
    in.length = eff->sockets.length;
    in.has_item = true;
    in.ads = true;
    in.dt = 1.0f / 60.0f;
    in.grounded = true;
    in.sway_scale = 1.0f;
    in.kick_params.set = true;
    in.kick_params.ads_push = eff->recoil.ads_forward_push;
    in.kick_params.ads_roll = eff->recoil.ads_roll;
    in.kick_params.ads_misalign_deg = iron ? eff->recoil.ads_max_misalign_deg : 0.0f;
    in.kick_params.kick_hip = eff->recoil.anchor_hip;
    in.kick_params.kick_ads = eff->recoil.anchor_ads;
    in.kick_params.sway_hip = eff->recoil.sway_anchor_hip;
    in.kick_params.sway_ads = eff->recoil.sway_anchor_ads;
    HoldPointOutput out;
    float worst = 0;
    *worst_z = 0;
    const float ks = 3.0f;   // wpn_kick_scale maximum
    for (int t = 0; t < 660; t++) {
        // Spring-like random kicks: back, up, pitch, yaw, roll at three times the configured size.
        const RecoilParams *r = &eff->recoil;
        float a = (t % 4 == 0) ? 1.0f : 0.4f;
        in.kick_pos = v3(r->vm_back * 0.15f * (frand() - 0.5f) * ks * a, r->vm_up * ks * a, r->vm_back * ks * a);
        in.kick_rot = v3(r->vm_pitch * ks * a, r->vm_yaw * (frand() * 2 - 1) * ks * a, r->vm_roll * (frand() * 2 - 1) * ks * a);
        in.aim_yaw += (frand() - 0.5f) * 0.02f;     // mouse movement (look lag, if enabled)
        in.aim_pitch += (frand() - 0.5f) * 0.01f;
        holdpoint_eval(&st, &in, &out);
        if (t < 60) continue;   // blending into ADS
        vec3 s = xf_point(out.item, in.sight);
        float off = sqrtf(s.x * s.x + s.y * s.y);
        worst = fmaxf(worst, off);
        if (s.z > *worst_z || *worst_z == 0) *worst_z = s.z;
    }
    return worst;
}

static void test_anchor(void) {
    static ItemDB db;
    memset(&db, 0, sizeof(db));
    CHECK(items_load(&db, "data/items.json", "data/weapons"), "items load");
    holdpoint_set_ads_relief_scale(0.5f);
    holdpoint_set_look_lag_scale(1.0f);
    int combos = 0, bad = 0;
    float worst_all = 0;
    for (int w = 0; w < db.weapon_count; w++) {
        const WeaponDef *base = &db.weapons[w];
        if (base->kind == WPN_MELEE) continue;
        HoldPointStyle style;
        holdpoint_style_defaults(&style, base->hold_style);
        style.ads_scale = 0.0f;
        // Iron sights + every optic the weapon accepts.
        for (int a = 0; a <= db.attachment_count; a++) {
            const AttachmentDef *ad = a ? attach_def(&db, (u8)a) : NULL;
            if (a && (!ad || ad->slot != ATT_OPTIC || !attach_allowed(base, ad))) continue;
            if (a && !base->att_slots[ATT_OPTIC].present) continue;
            u8 att[ATT_SLOTS] = { 0 };
            att[ATT_OPTIC] = (u8)a;
            static WeaponDef eff;
            attach_weapon_effective(&db, base, att, &eff);
            CHECK(eff.recoil.anchor_ads == RA_SIGHT, "%s: ADS anchor is the sight", base->id);
            float wz;
            float off = ads_kick_run(&eff, &style, a == 0, &wz);
            combos++;
            worst_all = fmaxf(worst_all, off);
            if (off > 1e-4f) { bad++; printf("  %s + %s: sight off axis by %.5f m\n", base->id, ad ? ad->id : "iron", off); }
            CHECK(wz <= -0.049f, "%s + %s: the sight stays in front of the eye (z %.4f)", base->id, ad ? ad->id : "iron", wz);
        }
    }
    CHECK(combos >= 20 && bad == 0, "every weapon x optic keeps the sight on the view axis under maximum ADS kick (%d combos, %d off, worst %.6f m)", combos, bad, worst_all);
    printf("  ADS anchor: %d weapon x optic combos, worst sight offset %.6f m over 600 ticks at 3x kick\n", combos, worst_all);
}

int main(void) {
    printf("recoil hip / ADS tests\n");
    test_ratios();
    test_anchor();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
