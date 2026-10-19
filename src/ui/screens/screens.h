// Game screens (docs/UI.md "Screen map"): the screen stack lives in src/ui/menus.c; each screen
// draws itself into the rect the pause layout gives it and returns true when it wants to close.
#ifndef DG_SCREENS_H
#define DG_SCREENS_H

#include "ui/gui/gui_widgets.h"

typedef enum ScreenId {
    SCR_NONE, SCR_PAUSE, SCR_SETTINGS, SCR_MULTIPLAYER, SCR_INVENTORY, SCR_BENCH, SCR_CONFIRM,
    SCR_MAINMENU,   // the main menu (src/ui/mainmenu): stack bottom, never popped by Back
    SCR_CHAT,       // in-game chat line (Enter)
    SCR_LOBBY,      // pause > Lobby: players, chat, invite, kick
    SCR_COMMANDS,   // pause > Commands: the console registry + embedded console (src/ui/pause)
    SCR_PHONE,      // the Connect portal (src/ui/mainmenu/mm_phone.c): main menu, pause menu, the in-game phone
    SCR_COUNT
} ScreenId;

typedef enum ConfirmKind { CONFIRM_QUIT, CONFIRM_LEAVE, CONFIRM_DROP, CONFIRM_DISPLAY, CONFIRM_RESET_BINDS, CONFIRM_GRAPHICS_DEFAULTS, CONFIRM_FIDELITY, CONFIRM_MAINMENU, CONFIRM_COMMAND } ConfirmKind;

// Shared by the screens (menus.c)
const char *screen_name(int id);
void screens_push(int id);
void screens_pop(void);
int screens_top(void);
bool screens_has(int id);
void screens_confirm(int kind, const char *title, const char *text, int arg);
GuiRect screens_modal_rect(void);          // the centred sub-menu rect (pause layout "modal")
GuiRect screens_dialog_rect(void);
// A modal frame: panel with a title and an optional tab bar; returns the content rect.
GuiRect screens_modal_frame(GuiRect r, const char *title, const char *subtitle);
void screens_footer_hint(GuiRect r, const char *text);   // bottom-right key hints

// Settings (scr_settings.c)
void settings_open(int tab);
bool settings_draw(GuiRect r);             // true = close
void settings_tick(float dt);              // display "keep?" countdown
void settings_confirm_result(bool keep);
// Settings > Graphics "Default": every graphics cvar back to its registered (coded) default, the
// Engine Fidelity level's own defaults for its features (custom tweaks cleared). The display
// (resolution, window mode, vsync) is kept: it has its own Apply / Revert. Console: r_defaults.
void settings_graphics_defaults(void);
float settings_confirm_left(void);
void screens_confirm_expired(void);        // the display countdown ran out with the dialog open

// Multiplayer (scr_multiplayer.c)
bool multiplayer_draw(GuiRect r);

// Inventory (ui/inventory/inv_screen.c, docs/INVENTORY.md)
void inventory_open(void);
void inventory_register_commands(void);
void inventory_close(void);
bool inventory_draw(GuiRect area, bool as_modal);
void inventory_confirm_drop(bool yes);
bool inventory_wants_back(void);           // false while a drag / context menu takes the back press

// Weapon bench (scr_bench.c)
bool bench_draw(GuiRect r);

#endif
