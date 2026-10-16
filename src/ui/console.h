// In-game console overlay (toggle with ~). Executes commands / cvars through core/cvar.
#ifndef DG_CONSOLE_H
#define DG_CONSOLE_H

#include "core/base.h"

union SDL_Event;

void console_init(void);
void console_shutdown(void);
void console_toggle(void);
void console_set_open(bool open);
bool console_is_open(void);
// Returns true if the event was consumed (history navigation / completion).
bool console_handle_event(const union SDL_Event *ev);
void console_draw(void);

#endif
