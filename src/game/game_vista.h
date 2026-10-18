// The city outside in a story level (docs/STORY.md "Vista"): the level's "city" block (window lights,
// roads) and the vista sequences the chapter started (src/story/vista_seq.c evaluates them; this draws
// them: district power, helicopters, smoke columns, fires, flashes, traffic, birds). Every machine.
#ifndef DG_GAME_VISTA_H
#define DG_GAME_VISTA_H

#include "core/dgmath.h"

void vista_register(void);
void vista_level_loaded(const char *map);
// A sequence started `age` story seconds before the host's story clock read `now` (EV_STORY
// SEVT_SEQUENCE; the vista clock adopts it); "" forgets every sequence.
void vista_sequence(const char *name, float age, float now);
// Per frame. host_now: the host's story clock (NULL on clients: they advance by dt).
void vista_frame(float dt, const float *host_now);
// Draw items and sprites for this frame (game_render.c, after the world). night: 0 day .. 1 night.
void vista_render(vec3 cam, float night);

#endif
