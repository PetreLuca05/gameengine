// Game UI core (docs/UI.md): input (mouse, keyboard, gamepad), ids, hover / press / focus,
// spatial keyboard + gamepad navigation, tweens, input blocking for modals, UI sounds, tooltips,
// DPI scale and the layout parameters. Widgets are in gui_widgets.c, screens in src/ui/screens.
#ifndef DG_GUI_H
#define DG_GUI_H

#include "core/base.h"
#include "core/dgmath.h"
#include "gui_layout.h"
#include "gui_draw.h"
#include "gui_theme.h"

union SDL_Event;
typedef u32 GuiId;

typedef enum GuiNav {
    NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT,
    NAV_ACCEPT,        // Enter / Space / pad A
    NAV_BACK,          // Esc / pad B
    NAV_TAB_NEXT,      // pad RB / Ctrl+Tab / E (inventory tabs)
    NAV_TAB_PREV,      // pad LB / Ctrl+Shift+Tab / Q
    NAV_ALT,           // pad X: context action (inventory: context menu)
    NAV_ALT2,          // pad Y: secondary (inventory: quick move)
    NAV_MENU,          // pad Start
    NAV_VIEW,          // pad Back / Tab / I (inventory toggle)
    NAV_COUNT
} GuiNav;

enum GuiFlags {
    GUI_FOCUSABLE = 1u << 0,   // reachable with the keyboard / gamepad
    GUI_NO_HOVER_SOUND = 1u << 1,
    GUI_DEFAULT_FOCUS = 1u << 2, // focused first when nothing is
    GUI_DISABLED = 1u << 3,
};

typedef struct GuiInteract {
    bool hovered;              // mouse over (and input enabled)
    bool focused;              // keyboard / gamepad focus
    bool pressed;              // mouse pressed on it this frame
    bool held;                 // mouse button held since a press on it
    bool clicked;              // released over it / accepted with focus
    bool right_clicked;
    bool double_clicked;
    bool hot;                  // hovered or focused (draw state)
} GuiInteract;

void gui_init(void);
void gui_shutdown(void);
bool gui_handle_event(const union SDL_Event *ev);   // feeds every event; true = the UI used it
void gui_frame_begin(float dt);
void gui_frame_end(void);       // navigation, tooltips, flush
void gui_reload(void);          // theme + screen layouts + fonts (ui_reload)

GuiId gui_id(const char *s);
GuiId gui_idi(const char *s, int i);
float gui_s(float design_px);   // design px (1080p) -> pixels
float gui_scale(void);
float gui_w(void);
float gui_h(void);
float gui_dt(void);
double gui_time(void);
LayoutParams gui_layout_params(void);

// Input
vec2 gui_mouse(void);
bool gui_mouse_down(int button);       // 0 left, 1 right, 2 middle
bool gui_mouse_pressed(int button);
bool gui_mouse_released(int button);
bool gui_mouse_moved(void);
bool gui_double_clicked(void);        // left double click pressed this frame
float gui_wheel(void);                 // consumed by the first scroll region under the mouse
void gui_wheel_consume(void);
bool gui_shift(void);
bool gui_ctrl(void);
void gui_set_mods(bool shift, bool ctrl);   // scripted tests
bool gui_nav(int nav);                 // pressed this frame and not consumed
void gui_nav_consume(int nav);
bool gui_nav_mode(void);               // the last input was keyboard / gamepad: draw focus
vec2 gui_pad_stick(void);              // left stick (deadzoned), for the gamepad cursor
bool gui_text_input_char(int *cp);     // typed characters (text fields)
int gui_backspaces(void);
void gui_set_text_mode(bool on);       // a text field is being edited (letters are text, not shortcuts)

// Interaction (registers focusables while input is enabled)
GuiInteract gui_interact(GuiId id, GuiRect r, u32 flags);
void gui_push_input(bool enabled);     // modals: only the top screen receives input
void gui_pop_input(void);
bool gui_input_enabled(void);
GuiId gui_focus(void);
bool gui_focus_rect(GuiRect *out);     // the focused widget's rect (last seen)
void gui_set_focus(GuiId id);
GuiId gui_active(void);                // mouse capture (sliders, drags)
void gui_set_active(GuiId id);
void gui_focus_scope(GuiRect r);       // navigation stays inside r (modals); call each frame
void gui_block_rect(GuiRect r);        // the mouse over r hits nothing under it (opaque panels drawn later)

// Animation
float gui_tween(GuiId id, float target, float duration);   // exponential approach (99 % in duration)
float gui_anim_t(GuiId id);            // seconds since first used (or restarted)
void gui_anim_restart(GuiId id);
float gui_ease_out(float t);           // cubic

// Feedback
void gui_sound(const char *name);
void gui_tooltip(GuiId id, const char *text);   // call while the widget is hovered

// Debug (ui_debug 1): widget rects, focus, layout nodes
bool gui_debug(void);
void gui_debug_rect(GuiRect r, u32 color, const char *label);

#endif
