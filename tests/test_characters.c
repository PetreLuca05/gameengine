// Character pipeline tests (docs/CHARACTERS.md): Skeleton asset bake, SkeletalMesh bone subsets and
// palette gather, mesh-only leaf bones, bone-name aliases / humanoid mapping, hierarchy rejection,
// modular parts (one evaluation + one palette per character, swaps never touch the pose, hide masks,
// region cuts tile the body), instanced batching of 200 modular zombies, part replication, sockets
// and virtual bones. Run from the repository root (or the build output dir) so assets/ resolves.
#include "anim/anim_library.h"
#include "anim/character_rt.h"
#include "anim/anim_net.h"
#include "anim/anim_lod.h"
#include "anim/retarget.h"
#include "anim/skeleton.h"
#include "assets/character.h"
#include "render/skeletal_mesh.h"
#include "net/bitbuf.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

Engine g_engine;
static int g_fail, g_checks;
static AnimLibrary *L;

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

static vec3 skin_point(const mat4 *pal, const i16 *jmap, const SkinVertex *v) {
    vec3 p = v3(0, 0, 0);
    for (int k = 0; k < 4; k++) {
        if (!v->weights[k]) continue;
        int j = jmap ? jmap[v->joints[k]] : v->joints[k];
        p = v3_madd(p, mat4_mul_point(pal[j], v->pos), (float)v->weights[k] / 255.0f);
    }
    return p;
}

// A walking pose (controller evaluated) in world space, for skinning checks.
static void walking_pose(const CharacterDef *def, int ticks, CharRuntime *rt, Xform *world) {
    charrt_init(rt, L, def);
    CharInputs in;
    memset(&in, 0, sizeof(in));
    in.grounded = true;
    in.speed = 1.4f;
    in.move_local = v2(0.3f, 1.0f);
    in.health = 100;
    for (int t = 0; t < ticks; t++) {
        charrt_set_inputs(rt, &in);
        charrt_tick(rt, 1.0f / 60.0f);
        charrt_eval_base(rt);
    }
    charrt_to_world(rt, rt->base_model, xf_make(v3(1, 0, -2), quat_yaw(0.7f)), def->scale, world);
}

static int find_mesh(const char *path, const char *region) {
    for (int i = 0; i < L->mesh_count; i++)
        if (!strcmp(L->mesh_refs[i].path, path) && !strcmp(L->mesh_refs[i].region, region) && L->mesh_refs[i].inflate == 0 && L->mesh_refs[i].scale.x == 1)
            return i;
    return -1;
}

// ------------------------------------------------------------------ skeleton asset

static void test_skeleton_bake(void) {
    char buf[512];
    char *meta = platform_read_file(platform_data_path("assets/skeletons/human.json", buf, sizeof(buf)), NULL);
    CHECK(meta != NULL, "assets/skeletons/human.json missing");
    if (!meta) return;
    SkeletonAsset *a = calloc(1, sizeof(SkeletonAsset)), *b = calloc(1, sizeof(SkeletonAsset)), *c = calloc(1, sizeof(SkeletonAsset));
    skel_asset_from_skeleton(a, &L->skel, "human", "assets/human/Dummy.fbx", 1234);
    skel_asset_apply_meta(a, meta, NULL, 0);
    skel_asset_from_skeleton(b, &L->skel, "human", "assets/human/Dummy.fbx", 1234);
    skel_asset_apply_meta(b, meta, NULL, 0);
    u8 *pa, *pb, *pc;
    size_t na, nb, nc;
    skel_asset_serialize(a, &pa, &na);
    skel_asset_serialize(b, &pb, &nb);
    CHECK(na == nb && !memcmp(pa, pb, na), "skeleton bake is not deterministic");
    CHECK(skel_asset_deserialize(c, pa, na, NULL, 0), "skeleton deserialize");
    skel_asset_serialize(c, &pc, &nc);
    CHECK(nc == na && !memcmp(pa, pc, na) && skel_asset_hash(c) == skel_asset_hash(a), "skeleton round trip changed the asset");
    CHECK(skel_asset_hash(a) == skel_asset_hash(&L->skeleton), "library skeleton hash %08x vs bake %08x", skel_asset_hash(&L->skeleton), skel_asset_hash(a));
    // Contents: sockets, virtual bones, groups, LOD sets.
    const SkelSocket *grip = skel_asset_socket(a, "hand_r_grip");
    CHECK(grip && grip->bone == L->skel.role[BR_R_HAND], "hand_r_grip socket not on the right hand");
    int head_group = skel_asset_find_group(a, "head");
    CHECK(head_group >= 0 && boneset_has(&a->groups[head_group].bones, L->skel.role[BR_HEAD]) && !boneset_has(&a->groups[head_group].bones, L->skel.role[BR_SPINE]),
          "head group");
    int ub = skel_asset_find_group(a, "upper_body"), lb = skel_asset_find_group(a, "lower_body");
    CHECK(ub >= 0 && lb >= 0 && boneset_count(&a->groups[ub].bones) + boneset_count(&a->groups[lb].bones) == L->skel.count, "upper + lower body != all bones");
    CHECK(boneset_count(&a->lod_bones[0]) == L->skel.count && boneset_count(&a->lod_bones[2]) < L->skel.count, "LOD bone sets");
    BoneMask m;
    CHECK(skel_asset_group_mask(a, "fingers", &m) && m.w[L->skel.role[BR_L_INDEX1]] == 1.0f && m.w[L->skel.role[BR_L_HAND]] == 0.0f, "fingers mask");
    // Virtual bone: the left hand in the right hand's space, without touching the hierarchy.
    int vb = skel_asset_find_vbone(a, "VB hand_l_in_hand_r");
    CHECK(vb >= 0 && a->skel.count == L->skel.count, "virtual bone missing / changed the bone count");
    if (vb >= 0) {
        Xform l = skel_vbone_local(&a->vbones[vb], L->skel.bind_model);
        Xform back = xf_mul(L->skel.bind_model[L->skel.role[BR_R_HAND]], l);
        CHECK(v3_dist(back.t, L->skel.bind_model[L->skel.role[BR_L_HAND]].t) < 1e-4f, "virtual bone local does not reach its target");
    }
    printf("  skeleton: %zu bytes, %d sockets, %d virtual bones, %d groups, hash %08x\n", na, a->socket_count, a->vbone_count, a->group_count, skel_asset_hash(a));
    free(pa); free(pb); free(pc); free(a); free(b); free(c); free(meta);
}

// ------------------------------------------------------------------ meshes

static void test_mesh_subset_palette(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "zombie_placeholder");
    CHECK(def != NULL, "zombie_placeholder missing");
    if (!def) return;
    CharRuntime *rt = calloc(1, sizeof(CharRuntime));
    SkeletonInstance *si = calloc(1, sizeof(SkeletonInstance));
    Xform world[ANIM_MAX_BONES];
    walking_pose(def, 37, rt, world);
    skel_inst_init(si, &L->skeleton);
    skel_inst_set_pose(si, world);
    int body = find_mesh("assets/human/Dummy.fbx", "");
    int head = find_mesh("assets/human/Dummy.fbx", "part_head");
    CHECK(body >= 0 && head >= 0, "dummy body / head part not loaded (%d %d)", body, head);
    if (body < 0 || head < 0) { free(rt); free(si); return; }
    const SkeletalMesh *bm = &L->meshes[body], *hm = &L->meshes[head];
    CHECK(hm->bone_count < bm->bone_count && hm->bone_count <= 4, "head part bone subset %d (body %d)", hm->bone_count, bm->bone_count);
    mat4 pal[SKM_MAX_BONES];
    int n = skm_gather_palette(hm, si, NULL, pal);
    CHECK(n == hm->bone_count, "gathered %d bones", n);
    // Gather == full skeleton palette evaluation, for every vertex of the subset mesh.
    i16 jmap[SKM_MAX_BONES];
    skm_joint_map(hm, jmap);
    float worst = 0;
    const SkinLod *H = &hm->data.lods[0];
    for (u32 i = 0; i < H->vcount; i++) worst = fmaxf(worst, v3_dist(skin_point(pal, NULL, &H->verts[i]), skin_point(si->skin, jmap, &H->verts[i])));
    CHECK(worst < 1e-5f, "subset palette differs from the full evaluation by %.6f m", worst);
    // The part's vertices land exactly where the full body's same vertices do.
    mat4 bpal[SKM_MAX_BONES];
    skm_gather_palette(bm, si, NULL, bpal);
    const SkinLod *B = &bm->data.lods[0];
    float seam = 0;
    int matched = 0;
    for (u32 i = 0; i < H->vcount && matched < 200; i++)
        for (u32 k = 0; k < B->vcount; k++)
            if (v3_dist(B->verts[k].pos, H->verts[i].pos) < 1e-6f) {
                seam = fmaxf(seam, v3_dist(skin_point(bpal, NULL, &B->verts[k]), skin_point(pal, NULL, &H->verts[i])));
                matched++;
                break;
            }
    CHECK(matched > 50 && seam < 1e-5f, "part vs full body skinning: %d matched, worst %.6f m", matched, seam);
    printf("  head part: %d bones (body %d), gather vs full evaluation %.2g m, vs body %.2g m\n", hm->bone_count, bm->bone_count, worst, seam);
    // docs/perf/FINDINGS.md PF-0020: parts whose bone tables are equal share one palette copy.
    SkeletalMesh *cp = calloc(1, sizeof(SkeletalMesh));
    skm_copy(cp, bm);
    mat4 cpal[SKM_MAX_BONES];
    skm_gather_palette(cp, si, NULL, cpal);
    CHECK(skm_palette_equal(bm, cp) && !memcmp(cpal, bpal, sizeof(mat4) * (size_t)bm->bone_count), "equal bone tables must gather equal palettes");
    CHECK(!skm_palette_equal(bm, hm), "body and head bone tables taken as equal");
    cp->bones[0].has_fix = !cp->bones[0].has_fix;
    CHECK(!skm_palette_equal(bm, cp), "a per-bone fix must make the tables differ");
    skm_free(cp);
    free(cp);
    free(rt);
    free(si);
}

static void test_mesh_only_leaf(void) {
    int body = find_mesh("assets/human/Dummy.fbx", "");
    if (body < 0) return;
    SkeletalMesh *m = calloc(1, sizeof(SkeletalMesh));
    skm_copy(m, &L->meshes[body]);
    int hb = skm_bone_of_skel(m, L->skel.role[BR_HEAD]);
    CHECK(hb >= 0, "head bone not in the body mesh");
    if (hb < 0) { skm_free(m); free(m); return; }
    // A hair strand bone the Skeleton doesn't have: rides on the head.
    int j = m->bone_count++;
    SkmBone *b = &m->bones[j];
    memset(b, 0, sizeof(*b));
    dg_strcpy(b->name, sizeof(b->name), "HairTip");
    b->skel = -1;
    b->parent = (i16)hb;
    b->local = xf_make(v3(0, 0.12f, 0.04f), quat_identity());
    b->bind_model = xf_mul(m->bones[hb].bind_model, b->local);
    SkinLod *Lo = &m->data.lods[0];
    int moved = 0;
    for (u32 i = 0; i < Lo->vcount; i++) {
        SkinVertex *v = &Lo->verts[i];
        if (skm_vertex_bone(m, v) == L->skel.role[BR_HEAD] && v->pos.y > m->bones[hb].bind_model.t.y + 0.08f) {
            memset(v->joints, 0, 4);
            memset(v->weights, 0, 4);
            v->joints[0] = (u8)j;
            v->weights[0] = 255;
            moved++;
        }
    }
    CHECK(moved > 0 && skm_mesh_only_count(m) == 1, "no vertices on the leaf bone");
    CharRuntime *rt = calloc(1, sizeof(CharRuntime));
    SkeletonInstance *si = calloc(1, sizeof(SkeletonInstance));
    Xform world[ANIM_MAX_BONES];
    walking_pose(anim_lib_char_by_id(L, "player"), 23, rt, world);
    skel_inst_init(si, &L->skeleton);
    skel_inst_set_pose(si, world);
    mat4 pal[SKM_MAX_BONES];
    skm_gather_palette(m, si, NULL, pal);
    // Rigid on the parent: the leaf's vertices move exactly like the head's.
    float worst = 0;
    for (u32 i = 0; i < Lo->vcount; i++)
        if (Lo->verts[i].joints[0] == j) worst = fmaxf(worst, v3_dist(skin_point(pal, NULL, &Lo->verts[i]), mat4_mul_point(si->skin[L->skel.role[BR_HEAD]], Lo->verts[i].pos)));
    Xform tip = skm_bone_world(m, si, j), want = xf_mul(world[L->skel.role[BR_HEAD]], b->local);
    CHECK(worst < 1e-5f && v3_dist(tip.t, want.t) < 1e-5f, "leaf bone does not follow its parent (%.6f m, tip %.6f m)", worst, v3_dist(tip.t, want.t));
    // Secondary-motion hook: a local override swings the strand, nothing else.
    Xform *leaf = calloc(SKM_MAX_BONES, sizeof(Xform));
    for (int k = 0; k < m->bone_count; k++) leaf[k] = m->bones[k].local;
    leaf[j].r = quat_axis_angle(v3(1, 0, 0), 0.5f);
    mat4 pal2[SKM_MAX_BONES];
    skm_gather_palette(m, si, leaf, pal2);
    CHECK(!memcmp(&pal2[hb], &pal[hb], sizeof(mat4)) && memcmp(&pal2[j], &pal[j], sizeof(mat4)), "leaf override must only move the leaf");
    printf("  mesh-only leaf: %d vertices ride on the head (max %.2g m)\n", moved, worst);
    free(leaf);
    skm_free(m);
    free(m);
    free(rt);
    free(si);
}

static void test_aliases_and_hierarchy(void) {
    Skeleton *s = malloc(sizeof(Skeleton));
    RigReport *r = malloc(sizeof(RigReport));
    // Renamed bone: rejected as-is, accepted through an alias table.
    *s = L->skel;
    int fa = L->skel.role[BR_L_FOREARM];
    dg_strcpy(s->names[fa], ANIM_NAME_MAX, "mixamorig1:LeftLowerArm");
    skel_finalize(s);
    CHECK(!rig_validate(&L->skel, s, 0.05f, r) && strstr(r->text, "LeftLowerArm"), "renamed bone accepted without alias");
    BoneAliasTable t;
    memset(&t, 0, sizeof(t));
    dg_strcpy(t.a[0].from, 48, "mixamorig1:LeftLowerArm");
    dg_strcpy(t.a[0].to, 48, L->skel.names[fa]);
    t.count = 1;
    skm_alias_apply_skeleton(&t, s);
    CHECK(rig_validate(&L->skel, s, 0.05f, r) && r->identical, "alias did not resolve the renamed bone:\n%s", r->text);
    CHECK(!strcmp(skm_alias_resolve(&t, "otherns:LeftLowerArm"), L->skel.names[fa]), "alias namespace-insensitive match");
    // Humanoid name mapping (suggested aliases / tools/retarget).
    const Skeleton *c = &L->skel;
    CHECK(retarget_match_name(c, "upperarm_l") == c->role[BR_L_ARM], "UE upperarm_l");
    CHECK(retarget_match_name(c, "thigh_r") == c->role[BR_R_UPLEG], "UE thigh_r");
    CHECK(retarget_match_name(c, "thumb_01_r") == c->role[BR_R_THUMB1], "UE thumb_01_r");
    CHECK(retarget_match_name(c, "LeftUpperArm") == c->role[BR_L_ARM], "Unity LeftUpperArm");
    CHECK(retarget_match_name(c, "mixamorig7:HeadTop_End") == c->role[BR_HEAD_END], "Mixamo HeadTop_End");
    int t3 = skel_find(c, "LeftHandThumb3_end");
    CHECK(t3 >= 0 && retarget_match_name(c, "mixamorig:LeftHandThumb4") == t3, "Mixamo Thumb4 -> Thumb3_end");
    // A whole UE-named rig maps and validates after aliasing.
    *s = L->skel;
    static const struct { int role; const char *ue; } ue[] = { { BR_HIPS, "pelvis" }, { BR_SPINE, "spine_01" }, { BR_L_ARM, "upperarm_l" },
                                                               { BR_L_FOREARM, "lowerarm_l" }, { BR_L_HAND, "hand_l" }, { BR_R_UPLEG, "thigh_r" } };
    for (int i = 0; i < (int)ARRAY_COUNT(ue); i++) dg_strcpy(s->names[c->role[ue[i].role]], ANIM_NAME_MAX, ue[i].ue);
    skel_finalize(s);
    RetargetMap *map = calloc(1, sizeof(RetargetMap));
    BoneAliasTable sug;
    memset(&sug, 0, sizeof(sug));
    retarget_map_skeleton(&L->skeleton, s, map, &sug, NULL, 0);
    CHECK(sug.count == (int)ARRAY_COUNT(ue) && map->mapped == c->count, "UE rig: %d aliases, %d mapped", sug.count, map->mapped);
    skm_alias_apply_skeleton(&sug, s);
    CHECK(rig_validate(&L->skel, s, 0.05f, r) && r->identical, "UE rig after aliasing:\n%s", r->text);
    // Base-pose correction: identical rigs need none; a source pose maps back onto itself.
    float worst = 0;
    for (int b = 0; b < c->count; b++) worst = fmaxf(worst, quat_angle_between(map->correction[b], quat_identity()));
    CHECK(worst < 1e-3f, "base-pose correction on an identical rig %.4f rad", worst);
    Pose src, out;
    clip_sample(&L->clips[0], 0.3f, &src);
    retarget_pose(&L->skeleton, s, map, &src, &out);
    Xform ma[ANIM_MAX_BONES], mb[ANIM_MAX_BONES];
    pose_to_model(c, &src, ma);
    pose_to_model(c, &out, mb);
    float perr = 0;
    for (int b = 1; b < c->count; b++) perr = fmaxf(perr, quat_angle_between(ma[b].r, mb[b].r));
    CHECK(perr < 1e-3f, "retarget_pose on an identical rig changes rotations by %.4f rad", perr);
    // Different hierarchy: rejected with a clear message.
    *s = L->skel;
    s->parent[L->skel.role[BR_L_HAND]] = L->skel.role[BR_SPINE2];
    skel_finalize(s);
    CHECK(!rig_validate(&L->skel, s, 0.05f, r) && strstr(r->text, "parent differs") && strstr(r->text, "REJECTED"), "re-parented hand accepted:\n%s", r->text);
    free(map);
    free(s);
    free(r);
}

static void test_skm_files(void) {
    int body = find_mesh("assets/human/Dummy.fbx", "");
    if (body < 0) return;
    u8 *a, *b;
    size_t na, nb;
    SkeletalMesh *m = calloc(1, sizeof(SkeletalMesh));
    skm_serialize(&L->meshes[body], &a, &na);
    CHECK(skm_deserialize(m, a, na, NULL, 0), ".skm deserialize");
    skm_serialize(m, &b, &nb);
    CHECK(na == nb && !memcmp(a, b, na), ".skm round trip is not byte-identical");
    CHECK(m->skeleton_hash == L->skel.hash && m->bone_count == L->meshes[body].bone_count, ".skm header");
    char err[128] = { 0 };
    CHECK(!skm_deserialize(m, a, na / 2, err, sizeof(err)) && err[0], "truncated .skm accepted");
    free(a);
    free(b);
    skm_free(m);
    free(m);
}

// ------------------------------------------------------------------ modular characters

static void test_modular_parts(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "zombie_placeholder");
    if (!def) return;
    CHECK(def->slots[CSLOT_HEAD].option_count >= 3 && def->slots[CSLOT_TORSO].option_count >= 3 && def->slots[CSLOT_LEGS].option_count >= 3,
          "placeholder variety: head %d torso %d legs %d", def->slots[CSLOT_HEAD].option_count, def->slots[CSLOT_TORSO].option_count, def->slots[CSLOT_LEGS].option_count);
    // The region cut tiles the body: parts add up to the full mesh.
    int body = find_mesh("assets/human/Dummy.fbx", "");
    u32 tris = 0;
    static const char *const regions[] = { "part_head", "part_torso", "part_hands", "part_legs" };
    for (int i = 0; i < 4; i++) {
        int k = find_mesh("assets/human/Dummy.fbx", regions[i]);
        CHECK(k >= 0, "region %s not loaded", regions[i]);
        if (k >= 0) tris += L->meshes[k].data.lods[0].icount / 3;
    }
    CHECK(body >= 0 && tris == L->meshes[body].data.lods[0].icount / 3, "parts %u tris vs body %u", tris, body >= 0 ? L->meshes[body].data.lods[0].icount / 3 : 0);
    // Two parts on one SkeletonInstance: one animation evaluation and one palette per tick.
    CharRuntime *rt = calloc(1, sizeof(CharRuntime)), *ref = calloc(1, sizeof(CharRuntime));
    SkeletonInstance *si = calloc(1, sizeof(SkeletonInstance)), *si_ref = calloc(1, sizeof(SkeletonInstance));
    charrt_init(rt, L, def);
    charrt_init(ref, L, def);
    skel_inst_init(si, &L->skeleton);
    skel_inst_init(si_ref, &L->skeleton);
    u8 sel[CSLOT_COUNT];
    character_pick_parts(def, 7, sel);
    SkinnedMeshInstance parts[CSLOT_COUNT];
    int np = 0;
    for (int s = 0; s < CSLOT_COUNT; s++) {
        const CharPartOption *o = character_part(def, sel, s);
        if (o) parts[np++] = (SkinnedMeshInstance){ &L->meshes[o->mesh], si, 0, NULL };
    }
    CHECK(np >= 2, "placeholder picked %d parts", np);
    CharInputs in;
    memset(&in, 0, sizeof(in));
    in.grounded = true;
    in.health = 100;
    Xform world[ANIM_MAX_BONES], world_ref[ANIM_MAX_BONES];
    mat4 pal[SKM_MAX_BONES];
    const int ticks = 60;
    bool swapped = false;
    for (int t = 0; t < ticks; t++) {
        in.speed = t < 20 ? 0.5f : 1.6f;
        in.move_local = v2(sinf((float)t * 0.1f), 1.0f);
        CharRuntime *both[2] = { rt, ref };
        for (int k = 0; k < 2; k++) {
            charrt_set_inputs(both[k], &in);
            charrt_tick(both[k], 1.0f / 60.0f);
            charrt_eval_base(both[k]);
        }
        charrt_to_world(rt, rt->base_model, xf_identity(), def->scale, world);
        charrt_to_world(ref, ref->base_model, xf_identity(), def->scale, world_ref);
        skel_inst_set_pose(si, world);
        skel_inst_set_pose(si_ref, world_ref);
        // Mid-animation swap: other head / torso options (only the mesh list changes).
        if (t == ticks / 2) {
            for (int s = CSLOT_HEAD; s <= CSLOT_LEGS; s++)
                if (def->slots[s].option_count > 1) sel[s] = (u8)(sel[s] % def->slots[s].option_count + 1);
            np = 0;
            for (int s = 0; s < CSLOT_COUNT; s++) {
                const CharPartOption *o = character_part(def, sel, s);
                if (o) parts[np++] = (SkinnedMeshInstance){ &L->meshes[o->mesh], si, 0, NULL };
            }
            swapped = true;
        }
        for (int p = 0; p < np; p++) skm_gather_palette(parts[p].mesh, parts[p].skel, NULL, pal);
    }
    CHECK(rt->evals == (u32)ticks, "%d parts: %u animation evaluations for %d ticks", np, rt->evals, ticks);
    CHECK(si->palette_builds == (u32)ticks, "%d parts: %u palettes for %d ticks", np, si->palette_builds, ticks);
    CHECK(swapped && !memcmp(rt->base_model, ref->base_model, sizeof(Xform) * (size_t)L->skel.count) && !memcmp(si->world, si_ref->world, sizeof(Xform) * (size_t)L->skel.count),
          "swapping parts mid-animation changed the pose");
    printf("  modular: %d parts, %u evaluations + %u palettes in %d ticks, pose bit-identical across a swap\n", np, rt->evals, si->palette_builds, ticks);
    // Hide masks: a part hiding the head region removes those triangles from the other parts.
    CharacterDef *d2 = malloc(sizeof(CharacterDef));
    *d2 = *def;
    CharPartOption *helmet = &d2->slots[CSLOT_TORSO].options[0];
    dg_strcpy(helmet->hide_names[0], 24, "part_head");
    helmet->hide_count = 1;
    character_resolve_parts(d2, &L->skeleton);
    u8 s2[CSLOT_COUNT] = { 0 };
    s2[CSLOT_TORSO] = 1;
    s2[CSLOT_HEAD] = 1;
    BoneSet hidden;
    character_hidden_bones(d2, s2, CSLOT_HEAD, &hidden);
    const CharPartOption *ho = character_part(d2, s2, CSLOT_HEAD);
    u32 *idx = malloc(sizeof(u32) * L->meshes[ho->mesh].data.lods[0].icount);
    CHECK(!boneset_empty(&hidden) && skm_indices_without(&L->meshes[ho->mesh], 0, &hidden, idx) == 0, "hidden head part still has triangles");
    character_hidden_bones(d2, s2, CSLOT_TORSO, &hidden);
    CHECK(boneset_empty(&hidden), "a part must not hide its own triangles");
    free(idx);
    free(d2);
    free(rt);
    free(ref);
    free(si);
    free(si_ref);
}

// 200 modular zombies built from shared parts: the renderer's instanced batches (same key and batch
// builder as src/gameplay/characters.c + src/render/render.c) stay within parts x LODs.
static void test_horde_batching(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "zombie_placeholder");
    if (!def) return;
    enum { N = 200 };
    u32 gpu_keys[64];
    int gpu_count = 0, used[CSLOT_COUNT][CHAR_MAX_PART_OPTIONS] = { { 0 } };
    u64 *keys = malloc(sizeof(u64) * N * CSLOT_COUNT);
    u32 *order = malloc(sizeof(u32) * N * CSLOT_COUNT);
    SkinBatchRun *runs = malloc(sizeof(SkinBatchRun) * N * CSLOT_COUNT);
    u32 n = 0;
    for (int z = 0; z < N; z++) {
        u8 sel[CSLOT_COUNT];
        character_pick_parts(def, (u32)z * 2654435761u + 17u, sel);
        int lod = z % CHAR_LOD_COUNT;
        for (int s = 0; s < CSLOT_COUNT; s++) {
            const CharPartOption *o = character_part(def, sel, s);
            if (!o) continue;
            used[s][sel[s] - 1]++;
            vec4 tints[SKIN_MAX_MATERIALS];
            const SkinMeshData *md = &L->meshes[o->mesh].data;
            character_part_tints(o, md, tints);
            u32 key = character_part_key(o->mesh, tints, md->material_count, 0, NULL);
            int g = -1;
            for (int k = 0; k < gpu_count; k++) if (gpu_keys[k] == key) g = k;
            if (g < 0 && gpu_count < 64) { g = gpu_count; gpu_keys[gpu_count++] = key; }
            keys[n++] = (u64)(g * CHAR_LOD_COUNT + lod + 1);   // texture 0 << 32 | mesh id of (GPU part, LOD)
        }
    }
    u32 batches = skin_batch_build(keys, n, order, runs);
    int options = 0;
    for (int s = 0; s < CSLOT_COUNT; s++) options += def->slots[s].option_count;
    CHECK(batches <= (u32)(gpu_count * CHAR_LOD_COUNT), "%u batches > %d parts x %d LODs", batches, gpu_count, CHAR_LOD_COUNT);
    CHECK(gpu_count <= options, "%d GPU parts for %d options", gpu_count, options);
    u32 inst = 0;
    for (u32 b = 0; b < batches; b++) inst += runs[b].count;
    CHECK(inst == n, "batches cover %u of %u draws", inst, n);
    // Every option of every slot shows up in a crowd this size (weighted variety works).
    for (int s = 0; s < CSLOT_COUNT; s++)
        for (int o = 0; o < def->slots[s].option_count; o++)
            CHECK(used[s][o] > 0, "slot %s option %s never picked in %d zombies", char_slot_name(s), def->slots[s].options[o].id, N);
    printf("  horde: %d zombies, %u part draws -> %u instanced batches (%d GPU parts x %d LODs)\n", N, n, batches, gpu_count, CHAR_LOD_COUNT);
    free(keys);
    free(order);
    free(runs);
}

// ------------------------------------------------------------------ net + sockets

static void test_part_replication(void) {
    AnimNetState s0, s1, out;
    memset(&s0, 0, sizeof(s0));
    s1 = s0;
    s1.parts[CSLOT_HEAD] = 2;
    s1.parts[CSLOT_TORSO] = 3;
    s1.parts[CSLOT_LEGS] = 1;
    s1.parts[CSLOT_HANDS] = 1;
    u8 buf[64];
    Writer w = writer_make(buf, sizeof(buf));
    u16 f = anim_net_diff(&s0, &s1);
    CHECK(f == ANS_PARTS, "part change mask %04x", f);
    anim_net_write(&w, &s1, f);
    out = s0;
    Reader r = reader_make(buf, w.len);
    anim_net_read(&r, &out, NULL);
    CHECK(!memcmp(&out, &s1, sizeof(out)) && !r.overflow, "part selection round trip");
    CHECK((int)w.len == 2 + anim_net_size(&s1, f) && w.len == 2 + 2 + 4, "part selection %u bytes", w.len);
    CHECK(anim_net_diff(&s1, &s1) == 0, "unchanged parts must not be resent");
    // The worn gear rides in the same block (docs/CHARACTERS.md "Worn gear"): two of three worn.
    AnimNetState s2 = s1;
    s2.worn[0] = 41;
    s2.worn[2] = 43;
    w = writer_make(buf, sizeof(buf));
    f = anim_net_diff(&s1, &s2);
    CHECK(f == ANS_PARTS, "worn gear change mask %04x", f);
    anim_net_write(&w, &s2, f);
    out = s1;
    r = reader_make(buf, w.len);
    anim_net_read(&r, &out, NULL);
    CHECK(!memcmp(&out, &s2, sizeof(out)) && !r.overflow && w.len == 2 + 2 + 4 + 2, "worn gear round trip (%u bytes)", w.len);
    // Every machine builds the same parts from the same seed.
    const CharacterDef *def = anim_lib_char_by_id(L, "zombie_placeholder");
    u8 a[CSLOT_COUNT], b[CSLOT_COUNT];
    character_pick_parts(def, 99, a);
    character_pick_parts(def, 99, b);
    CHECK(!memcmp(a, b, sizeof(a)), "part pick not deterministic");
    printf("  part selection: %u bytes on the wire (4 slots)\n", w.len);
}

static void test_sockets(void) {
    const CharacterDef *player = anim_lib_char_by_id(L, "player");
    CHECK(player && character_socket(player, "back") && character_socket(player, "camera"), "skeleton sockets not merged into the character");
    // Mesh-local socket (a backpack on one jacket) resolved through the part.
    int body = find_mesh("assets/human/Dummy.fbx", "");
    if (body < 0 || !player) return;
    SkeletalMesh *m = calloc(1, sizeof(SkeletalMesh));
    skm_copy(m, &L->meshes[body]);
    SkelSocket *k = &m->sockets[m->socket_count++];
    memset(k, 0, sizeof(*k));
    dg_strcpy(k->name, sizeof(k->name), "backpack");
    dg_strcpy(k->bone_name, sizeof(k->bone_name), "Spine2");
    k->offset = v3(0, 0.05f, 0.18f);
    k->bind_space = true;
    skm_finalize(m, &L->skeleton);
    CharRuntime *rt = calloc(1, sizeof(CharRuntime));
    SkeletonInstance *si = calloc(1, sizeof(SkeletonInstance));
    Xform world[ANIM_MAX_BONES];
    walking_pose(player, 11, rt, world);
    skel_inst_init(si, &L->skeleton);
    skel_inst_set_pose(si, world);
    Xform pack, back;
    CHECK(skm_socket_world(m, si, "backpack", &pack), "mesh socket");
    // Same as a skeleton socket with the same definition.
    SkelSocket ref = *k;
    skel_socket_resolve(&ref, &L->skel);
    back = skel_socket_world(&ref, si->world);
    CHECK(v3_dist(pack.t, back.t) < 1e-5f, "mesh socket %.6f m off the skeleton socket", v3_dist(pack.t, back.t));
    Xform grip;
    CHECK(skel_inst_socket_world(si, "hand_r_grip", &grip) && v3_dist(grip.t, world[L->skel.role[BR_R_HAND]].t) < 0.2f, "hand grip socket");
    skm_free(m);
    free(m);
    free(rt);
    free(si);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    L = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    if (!anim_lib_load(L, 30.0f)) { printf("FAIL: library load\n"); return 1; }
    printf("character pipeline tests\n");
    test_skeleton_bake();
    test_mesh_subset_palette();
    test_mesh_only_leaf();
    test_aliases_and_hierarchy();
    test_skm_files();
    test_modular_parts();
    test_horde_batching();
    test_part_replication();
    test_sockets();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
