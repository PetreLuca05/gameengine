#include "retarget.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

// Normalized bone name: no namespace, no rig prefix, lower case, letters and digits only
// ("mixamorig7:LeftHand_Index-1" -> "lefthandindex1", "Bip01 L UpperArm" -> "lupperarm").
static void norm_name(const char *in, char *out, size_t n) {
    const char *c = strrchr(in, ':');
    const char *s = c ? c + 1 : in;
    size_t k = 0;
    for (; *s && k + 1 < n; s++)
        if (isalnum((unsigned char)*s)) out[k++] = (char)tolower((unsigned char)*s);
    out[k] = 0;
    static const char *const prefixes[] = { "bip001", "bip01", "def", "mixamorig" };
    for (int p = 0; p < 4; p++) {
        size_t l = strlen(prefixes[p]);
        if (!strncmp(out, prefixes[p], l) && out[l]) { memmove(out, out + l, strlen(out + l) + 1); break; }
    }
}

// Synonyms (normalized source -> normalized canonical): UE mannequin, Unity humanoid, Bip / rigify-ish.
static const char *const k_syn[][2] = {
    { "pelvis", "hips" }, { "root", "hips" }, { "spine01", "spine" }, { "spine02", "spine1" }, { "spine03", "spine2" },
    { "chest", "spine1" }, { "upperchest", "spine2" }, { "neck01", "neck" }, { "headtopend", "headend" }, { "headend", "headend" },
    { "claviclel", "leftshoulder" }, { "clavicler", "rightshoulder" }, { "lclavicle", "leftshoulder" }, { "rclavicle", "rightshoulder" },
    { "upperarml", "leftarm" }, { "upperarmr", "rightarm" }, { "lupperarm", "leftarm" }, { "rupperarm", "rightarm" },
    { "leftupperarm", "leftarm" }, { "rightupperarm", "rightarm" },
    { "lowerarml", "leftforearm" }, { "lowerarmr", "rightforearm" }, { "lforearm", "leftforearm" }, { "rforearm", "rightforearm" },
    { "leftlowerarm", "leftforearm" }, { "rightlowerarm", "rightforearm" },
    { "handl", "lefthand" }, { "handr", "righthand" }, { "lhand", "lefthand" }, { "rhand", "righthand" },
    { "thighl", "leftupleg" }, { "thighr", "rightupleg" }, { "lthigh", "leftupleg" }, { "rthigh", "rightupleg" },
    { "leftupperleg", "leftupleg" }, { "rightupperleg", "rightupleg" },
    { "calfl", "leftleg" }, { "calfr", "rightleg" }, { "lcalf", "leftleg" }, { "rcalf", "rightleg" },
    { "leftlowerleg", "leftleg" }, { "rightlowerleg", "rightleg" },
    { "footl", "leftfoot" }, { "footr", "rightfoot" }, { "lfoot", "leftfoot" }, { "rfoot", "rightfoot" },
    { "balll", "lefttoebase" }, { "ballr", "righttoebase" }, { "lefttoes", "lefttoebase" }, { "righttoes", "righttoebase" },
    { "lefttoeend", "lefttoebaseend" }, { "righttoeend", "righttoebaseend" },
};

static int find_norm(const Skeleton *canon, const char *norm) {
    char b[64];
    for (int i = 0; i < canon->count; i++) {
        norm_name(canon->names[i], b, sizeof(b));
        if (!strcmp(b, norm)) return i;
    }
    return -1;
}

int retarget_match_name(const Skeleton *canon, const char *src_name) {
    char n[64], t[72];
    norm_name(src_name, n, sizeof(n));
    int b = find_norm(canon, n);
    if (b >= 0) return b;
    for (int i = 0; i < (int)ARRAY_COUNT(k_syn); i++)
        if (!strcmp(n, k_syn[i][0]) && (b = find_norm(canon, k_syn[i][1])) >= 0) return b;
    // UE fingers: thumb_01_l -> lefthandthumb1 (index / middle / ring / pinky alike).
    static const char *const fingers[] = { "thumb", "index", "middle", "ring", "pinky" };
    for (int f = 0; f < 5; f++) {
        size_t l = strlen(fingers[f]);
        if (strncmp(n, fingers[f], l) || strlen(n) != l + 3 || n[l] != '0') continue;
        char side = n[l + 2];
        if (side != 'l' && side != 'r') continue;
        snprintf(t, sizeof(t), "%shand%s%c", side == 'l' ? "left" : "right", fingers[f], n[l + 1]);
        if ((b = find_norm(canon, t)) >= 0) return b;
    }
    // End bones: Mixamo "...Thumb4" = canonical "...Thumb3_end"; "<x>end" = "<x>baseend".
    size_t len = strlen(n);
    if (len > 1 && n[len - 1] == '4') {
        snprintf(t, sizeof(t), "%.*s3end", (int)(len - 1), n);
        if ((b = find_norm(canon, t)) >= 0) return b;
    }
    return -1;
}

static void rep(char *r, int n, const char *fmt, ...) {
    if (!r || n <= 0) return;
    size_t l = strlen(r);
    if ((int)l >= n - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r + l, (size_t)n - l, fmt, ap);
    va_end(ap);
}

int retarget_map_skeleton(const SkeletonAsset *ca, const Skeleton *src, RetargetMap *map, BoneAliasTable *aliases, char *report, int report_size) {
    const Skeleton *canon = &ca->skel;
    memset(map, 0, sizeof(*map));
    map->src_count = src->count;
    for (int i = 0; i < ANIM_MAX_BONES; i++) { map->canon_of[i] = -1; map->src_of[i] = -1; map->correction[i] = quat_identity(); }
    if (report && report_size) report[0] = 0;
    for (int i = 0; i < src->count; i++) {
        int c = skel_find(canon, src->names[i]);
        bool renamed = false;
        if (c < 0) { c = retarget_match_name(canon, src->names[i]); renamed = c >= 0; }
        if (c < 0 || map->src_of[c] >= 0) continue;   // unmapped, or the canonical bone is taken
        // The parent must map onto the canonical parent chain (a different hierarchy stays unmapped).
        int sp = src->parent[i];
        while (sp >= 0 && map->canon_of[sp] < 0) sp = src->parent[sp];
        if (sp >= 0 && !skel_is_descendant(canon, c, map->canon_of[sp])) {
            rep(report, report_size, "  '%s' -> '%s' rejected: parent '%s' maps elsewhere\n", src->names[i], canon->names[c], src->names[sp]);
            continue;
        }
        map->canon_of[i] = (i16)c;
        map->src_of[c] = (i16)i;
        map->mapped++;
        if (renamed) {
            map->renamed++;
            rep(report, report_size, "  alias '%s' -> '%s'\n", src->names[i], canon->names[c]);
            if (aliases && aliases->count < SKM_MAX_ALIASES) {
                BoneAlias *a = &aliases->a[aliases->count++];
                dg_strcpy(a->from, sizeof(a->from), src->names[i]);
                dg_strcpy(a->to, sizeof(a->to), canon->names[c]);
            }
        }
    }
    int unmapped_canon = 0;
    for (int c = 0; c < canon->count; c++) unmapped_canon += map->src_of[c] < 0;
    rep(report, report_size, "mapped %d / %d source bones (%d by alias), %d canonical bones without a source bone\n", map->mapped, src->count, map->renamed,
        unmapped_canon);
    retarget_base_pose(ca, src, map);
    return map->mapped;
}

void retarget_base_pose(const SkeletonAsset *ca, const Skeleton *src, RetargetMap *map) {
    const Skeleton *canon = &ca->skel;
    // Canonical base pose in model space.
    Xform base[ANIM_MAX_BONES];
    for (int i = 0; i < canon->count; i++) {
        int p = canon->parent[i];
        base[i] = p >= 0 ? xf_mul(base[p], ca->base_pose[i]) : ca->base_pose[i];
    }
    for (int c = 0; c < canon->count; c++) {
        int s = map->src_of[c];
        // src_model * correction = canonical base (model rotations): correction = src^-1 * base.
        map->correction[c] = s >= 0 ? quat_norm(quat_mul(quat_conj(src->bind_model[s].r), base[c].r)) : quat_identity();
    }
    map->height_ratio = src->height > 0.01f ? canon->height / src->height : 1.0f;
}

void retarget_pose(const SkeletonAsset *ca, const Skeleton *src, const RetargetMap *map, const Pose *src_pose, Pose *out) {
    const Skeleton *canon = &ca->skel;
    Xform smodel[ANIM_MAX_BONES], cmodel[ANIM_MAX_BONES];
    pose_to_model(src, src_pose, smodel);
    out->count = canon->count;
    for (int c = 0; c < canon->count; c++) {
        int p = canon->parent[c], s = map->src_of[c];
        Xform local = ca->base_pose[c];
        if (s >= 0) {
            // Model rotation from the source, corrected onto the base pose; bone lengths stay canonical.
            quat want = quat_norm(quat_mul(smodel[s].r, map->correction[c]));
            quat parent_r = p >= 0 ? cmodel[p].r : quat_identity();
            local.r = quat_norm(quat_mul(quat_conj(parent_r), want));
            if (p < 0) local.t = v3_scale(smodel[s].t, map->height_ratio);   // root travel scaled by height
        }
        local.s = v3(1, 1, 1);
        out->local[c] = local;
        cmodel[c] = p >= 0 ? xf_mul(cmodel[p], local) : local;
    }
}

bool retarget_clip(const SkeletonAsset *canon, const Skeleton *src, const RetargetMap *map, const Clip *src_clip, Clip *out) {
    // Stub: foreign-rig clips go through retarget_pose per frame once a store asset needs it
    // (root motion extraction / markers then re-run as in fbx_bake_clips).
    (void)canon; (void)src; (void)map; (void)src_clip; (void)out;
    return false;
}
