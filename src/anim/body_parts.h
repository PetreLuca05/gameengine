// Body parts (shared skeleton): hitbox tags, damage multipliers, reactions, ragdoll bodies.
#ifndef DG_BODY_PARTS_H
#define DG_BODY_PARTS_H

#include "core/base.h"

typedef enum BodyPart {
    BP_HEAD, BP_NECK, BP_CHEST, BP_STOMACH, BP_PELVIS,
    BP_UPPER_ARM_L, BP_UPPER_ARM_R, BP_LOWER_ARM_L, BP_LOWER_ARM_R, BP_HAND_L, BP_HAND_R,
    BP_UPPER_LEG_L, BP_UPPER_LEG_R, BP_LOWER_LEG_L, BP_LOWER_LEG_R, BP_FOOT_L, BP_FOOT_R,
    BP_COUNT,
    BP_NONE = 0xFF
} BodyPart;

DG_INLINE const char *body_part_name(int p) {
    static const char *const names[BP_COUNT] = {
        "head", "neck", "chest", "stomach", "pelvis",
        "upper_arm_l", "upper_arm_r", "lower_arm_l", "lower_arm_r", "hand_l", "hand_r",
        "upper_leg_l", "upper_leg_r", "lower_leg_l", "lower_leg_r", "foot_l", "foot_r",
    };
    return (p >= 0 && p < BP_COUNT) ? names[p] : "none";
}
DG_INLINE int body_part_from_name(const char *s) {
    for (int i = 0; i < BP_COUNT; i++)
        if (!strcmp(body_part_name(i), s)) return i;
    return -1;
}
DG_INLINE bool body_part_is_leg(int p) { return p >= BP_UPPER_LEG_L && p <= BP_FOOT_R; }
DG_INLINE bool body_part_is_arm(int p) { return p >= BP_UPPER_ARM_L && p <= BP_HAND_R; }

#endif
