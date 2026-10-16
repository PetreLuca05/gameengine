// Widgets (docs/UI.md "Widgets"): every widget takes its rect (from a screen layout or a cursor),
// draws itself with the theme and returns what happened. Focusable with the keyboard / gamepad;
// sliders and dropdowns take left / right while focused.
#ifndef DG_GUI_WIDGETS_H
#define DG_GUI_WIDGETS_H

#include "gui.h"

typedef enum GuiButtonKind { GB_SECONDARY, GB_PRIMARY, GB_DANGER, GB_GHOST } GuiButtonKind;

// Vertical flow inside a rect (settings rows, lists).
typedef struct GuiCursor { GuiRect area; float y, gap; } GuiCursor;
GuiCursor gc_make(GuiRect area, float gap);
GuiRect gc_row(GuiCursor *c, float h);
float gc_used(const GuiCursor *c);
void gui_split_h(GuiRect r, float left_frac, float gap, GuiRect *a, GuiRect *b);

// Scroll region: draw content at r.y - offset inside the clip, then end with the content height.
typedef struct GuiScroll { GuiId id; GuiRect view; float offset, target, content_h; bool dragging; float drag_from, drag_off; } GuiScroll;
void gw_scroll_begin(GuiScroll *s, GuiId id, GuiRect view);
float gw_scroll_y(const GuiScroll *s);        // top of the content
void gw_scroll_end(GuiScroll *s, float content_h);
void gw_scroll_to(GuiScroll *s, float y, float h);   // make [y, y + h) (content space) visible

void gw_panel(GuiRect r, bool backdrop);       // modal / window: shadow, blurred scene, tint, border
void gw_hud_panel(GuiRect r);                  // HUD: lighter, no blur
void gw_section(GuiRect r, const char *title); // small-caps label + hairline
void gw_label(GuiRect r, const char *text, float size, int weight, u32 color, int align);
void gw_label_caps(GuiRect r, const char *text, float size, u32 color, int align);
void gw_focus_ring(GuiRect r, float radius, float amount);

bool gw_menu_item(GuiId id, GuiRect r, const char *label, bool selected, float appear, bool enabled);
bool gw_button(GuiId id, GuiRect r, const char *label, int kind, bool enabled);
int gw_tabs(GuiId id, GuiRect r, const char *const *labels, int n, int cur);
bool gw_slider(GuiId id, GuiRect r, float *v, float lo, float hi, float step, const char *fmt);
bool gw_toggle(GuiId id, GuiRect r, bool *v);
bool gw_dropdown(GuiId id, GuiRect r, const char *const *items, int n, int *sel);
bool gw_keybind(GuiId id, GuiRect r, const char *text, bool capturing);
bool gw_textfield(GuiId id, GuiRect r, char *buf, int size, const char *placeholder);   // true = Enter
// A settings row: label left, the control's rect returned (right part). hint: tooltip.
GuiRect gw_setting_row(GuiCursor *c, const char *label, const char *hint, GuiId id);

// Inventory / hotbar slot (shared by the inventory grid and the HUD).
typedef struct GuiSlotView {
    TextureId icon;           // 0: draw the fallback (type tint + initials)
    vec4 uv;
    u32 tint;                 // type colour
    const char *name;         // fallback label / initials
    int count;                // > 1: stack count
    float ammo_frac;          // < 0: none
    int ammo;
    int hotkey;               // 1..9, 0 = none
    bool selected, hot, focus, dim, empty;
    int drop_state;           // 0 none, 1 accept, 2 reject
    float shake;              // 0..1 rejection shake
} GuiSlotView;
void gw_slot(GuiRect r, const GuiSlotView *v);

#endif
