// skin_audit (docs/CHARACTERS.md "Own proportions"): every full-body skin against the canonical rig -
// the clavicle and acromion (upper-arm joint) widths of its own bind pose vs the Dummy's, and the same
// widths posed (the local player's current pose, else the reference pose) with the skeleton's joint
// positions (the old mapping) and with the skin's own proportions (skm_proportioned_world).
#include "game_internal.h"
#include "anim/anim_library.h"
#include "render/skeletal_mesh.h"
#include "gameplay/characters.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include <math.h>
#include <string.h>

#define LIB (g_engine.anim)

static float width(const Xform *w, int l, int r) { return l >= 0 && r >= 0 ? v3_dist(w[l].t, w[r].t) : 0.0f; }

// The mesh's own bind of a skeleton bone (its file bind), else the reference pose.
static Xform file_bind(const SkeletalMesh *m, const Skeleton *s, int k) {
    for (int j = 0; j < m->bone_count; j++)
        if (m->bones[j].skel == k) return m->bones[j].has_fix ? xf_mul(m->bones[j].bind_delta, m->bones[j].bind_model) : m->bones[j].bind_model;
    return s->bind_model[k];
}

static void cmd_skin_audit(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!LIB || !LIB->loaded) { log_info("skin_audit: no animation library"); return; }
    const Skeleton *s = &LIB->skeleton.skel;
    int lc = s->role[BR_L_SHOULDER], rc = s->role[BR_R_SHOULDER], la = s->role[BR_L_ARM], ra = s->role[BR_R_ARM];
    // The pose: the local player's, else the reference pose (the T-pose).
    static SkeletonInstance si;
    skel_inst_init(&si, &LIB->skeleton);
    World *w = game_world();
    Player *p = game_local_player();
    const CharacterInst *c = w && p ? char_get(w, p->ent) : NULL;
    bool posed = c && c->si;
    if (posed) memcpy(si.world, c->si->world, sizeof(Xform) * (size_t)si.count);
    else memcpy(si.world, s->bind_model, sizeof(Xform) * (size_t)si.count);
    float rig_c = width(s->bind_model, lc, rc), rig_a = width(s->bind_model, la, ra);
    float pose_c = width(si.world, lc, rc), pose_a = width(si.world, la, ra);
    log_info("skin_audit: Dummy (canonical) bind: clavicles %.3f m, acromions %.3f m; %s: clavicles %.3f, acromions %.3f", rig_c, rig_a,
             posed ? "posed (the local player)" : "reference pose", pose_c, pose_a);
    for (int k = 0; k < LIB->skin_count; k++) {
        const CharSkinDef *sk = &LIB->skins[k];
        if (sk->mesh < 0) { log_info("  %-8s unavailable", sk->id); continue; }
        const SkeletalMesh *m = &LIB->meshes[sk->mesh];
        Xform fb[ANIM_MAX_BONES];
        for (int b = 0; b < s->count; b++) fb[b] = file_bind(m, s, b);
        static Xform pw[ANIM_MAX_BONES];
        skm_proportioned_world(m, &si, pw);
        float fc = width(fb, lc, rc), fa = width(fb, la, ra), nc = width(pw, lc, rc), na = width(pw, la, ra);
        log_info("  %-8s bind: clavicles %.3f m (%+.0f%%), acromions %.3f m (%+.0f%%) | posed, old mapping: acromions %.3f (%+.0f%% vs its bind) | posed, own proportions: acromions %.3f (%+.1f%%) | max bind delta %.1f mm %.1f deg",
                 sk->id, fc, (fc / rig_c - 1) * 100, fa, (fa / rig_a - 1) * 100, pose_a, (pose_a / fa - 1) * 100, na, (na / fa - 1) * 100,
                 m->max_bind_delta_m * 1000.0f, m->max_bind_delta_deg);
        (void)nc;
    }
}

void skin_audit_register(void) {
    cmd_register("skin_audit", cmd_skin_audit, "skin_audit: every skin's shoulder widths (clavicles, acromions) in its bind pose vs the canonical rig, and posed with the old and the own-proportion mapping");
}
