// Animation debug UI (anim_debug 1): controller states / weights / parameters / transition
// history, per-layer solo / mute, host vs local state for remote characters; and the
// animation test stand (anim_stand): character / clip scrub, hold-style cycling, per-part
// hits, ragdoll drop + push.
#ifndef DG_ANIM_DEBUG_H
#define DG_ANIM_DEBUG_H

void anim_debug_init(void);
void anim_debug_draw(void);    // Nuklear pass (call from the UI frame)

#endif
