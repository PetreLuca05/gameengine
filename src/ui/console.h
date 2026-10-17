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

// Shared with the pause menu's Commands tab (src/ui/pause/commands_tab.c): one history, the same
// completion and hints, the same submit.
int console_history_count(void);
const char *console_history_at(int i);              // 0 = oldest
void console_submit(const char *line);               // logs "] line", history, runs it
// Completes the first word of buf in place (common prefix; a single match gets a space). Returns
// the number of matches; hint gets the first few of them.
int console_complete(char *buf, int size, char *hint, int hint_size);
// One-line help for what is typed: the command's description, or a cvar's value / default / range.
void console_hint(const char *input, char *out, int size);

#endif
