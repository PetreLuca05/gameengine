// Nominal eye calibration: see eye_nominal.h. The inputs mirror the game's post-pass assembly
// (src/gameplay/characters.c post_one): procedural crouch drop, the kneel while downed, the surface
// swim, the weapon's own sockets in the hand.
#include "eye_nominal.h"
#include "anim/character_rt.h"
#include "gameplay/items.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static Xform root_at0(void) { return xf_make(v3(0, 0, 0), quat_identity()); }

// The weapon whose sockets stand for a hold style (the first of the item database).
static const WeaponDef *style_weapon(const AnimLibrary *L, const ItemDB *db, int style, int *index) {
    for (int i = 0; db && i < db->weapon_count; i++)
        if (proc_hold_style_index(&L->proc, db->weapons[i].hold_style) == style) {
            *index = i;
            return &db->weapons[i];
        }
    return NULL;
}

bool eye_nominal_calibrate(const AnimLibrary *L, const CharacterDef *def, const ItemDB *db, EyeNominalTable *t, int warm, int sample) {
    if (!L || !def || !character_socket(def, "camera")) return false;
    if (warm <= 0) warm = 45;
    if (sample <= 0) sample = 120;
    eye_table_defaults(t);
    const float dt = 1.0f / 60.0f;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    vec3 *pts = (vec3 *)malloc(sizeof(vec3) * (size_t)sample);
    for (int s = 0; s < EYE_STYLES; s++) {
        int wi = -1;
        const WeaponDef *wd = s ? style_weapon(L, db, s, &wi) : NULL;
        if (s && !wd) continue;
        t->style_valid[s] = true;
        for (int p = 0; p < EYE_POSTURES; p++)
            for (int i = 0; i < t->speed_n[p]; i++)
                for (int k = 0; k < EYE_PITCH_N; k++) {
                    charrt_init(rt, L, def);
                    CharInputs in;
                    memset(&in, 0, sizeof(in));
                    in.grounded = p != EYE_HANG && p != EYE_MANTLE && p != EYE_SWIM;
                    in.weapon_type = s;
                    in.weapon_id = wd ? (u16)(wi + 1) : 0;
                    in.aim_pitch = t->pitch[k] * DG_DEG2RAD;
                    in.move_local = v2(0, 1);
                    float v = t->speed[p][i];
                    in.speed = p == EYE_MANTLE || p == EYE_HANG ? 0.0f : v;
                    in.sprint = p == EYE_STAND && v >= 6.5f;
                    in.crouch = p == EYE_CROUCH || p == EYE_DOWNED;
                    in.downed = p == EYE_DOWNED;
                    in.hanging = p == EYE_HANG;
                    in.mantling = p == EYE_MANTLE;
                    in.mantle_t = p == EYE_MANTLE ? v : 0.0f;
                    in.health = 100.0f;
                    vec3 sum = v3(0, 0, 0);
                    int n = 0;
                    for (int tk = 0; tk < warm + sample; tk++) {
                        charrt_set_inputs(rt, &in);
                        charrt_tick(rt, dt);
                        charrt_eval_base(rt);
                        ProcInput pi;
                        charrt_proc_input(rt, &pi, root_at0(), dt);
                        pi.item = wd && p != EYE_HANG && p != EYE_MANTLE ? &wd->sockets : NULL;
                        if (in.crouch) pi.crouch_drop = 0.38f;
                        if (p == EYE_SWIM) {
                            // At the surface: the eye pl_swim_eye (0.16) above the water, the feet a standing eye below it.
                            pi.swim = 1;
                            pi.swim_speed = v;
                            pi.water_y = 1.65f - 0.16f;
                            pi.foot_ik = false;
                            pi.lean = false;
                            if (pi.item) {
                                pi.vehicle_onehand = true;
                                pi.swim_hold_mask = 2;
                            }
                        }
                        charrt_post(rt, &pi);
                        if (tk < warm) continue;
                        Xform eye;
                        if (!character_socket_world(def, "camera", rt->final_model, &eye)) continue;
                        // Model space faces -Z: (right, up, forward) = (x, y, -z).
                        vec3 e = v3(eye.t.x, eye.t.y, -eye.t.z);
                        pts[n++] = e;
                        sum = v3_add(sum, e);
                    }
                    if (!n) continue;
                    vec3 mean = v3_scale(sum, 1.0f / (float)n);
                    t->off[p][s][i][k] = mean;
                    for (int j = 0; j < n; j++) t->residual[p] = fmaxf(t->residual[p], v3_dist(pts[j], mean));
                }
    }
    free(pts);
    free(rt);
    t->anim_hash = anim_manifest_combined(&L->manifest);
    t->valid = t->style_valid[0];
    return t->valid;
}
