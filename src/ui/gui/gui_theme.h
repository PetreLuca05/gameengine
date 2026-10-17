// UI theme (assets/ui/theme.json, hot reloaded with ui_reload): colours, type sizes, metrics,
// animation timings and UI sounds. Sizes are design pixels at 1080p (scaled by gui_s()).
#ifndef DG_GUI_THEME_H
#define DG_GUI_THEME_H

#include "core/base.h"
#include "gui_layout.h"

#define GUI_THEME_MAX_TOKENS 160

typedef struct GuiTheme {
    // colours (RGBA8, r in the low byte)
    u32 accent, accent_dim, text, text_dim, text_faint, text_dark;
    u32 panel, panel_hi, panel_border, panel_header;
    u32 hover, press, focus, scrim, shadow;
    u32 danger, warning, ok, info;
    u32 slot_bg, slot_border, slot_hi;
    u32 type_weapon, type_ammo, type_consumable, type_attachment, type_key, type_misc;
    u32 hud_panel, hud_text;
    u32 select;                   // radial wheel / hotbar selection (white)
    u32 ring;                     // timed-action ring (white)
    u32 ping[8];                  // ping types (wheel order: generic, enemy, loot, danger, go, help, vehicle, defend)
    u32 rarity[5];                // cosmetics: common, uncommon, rare, epic, legendary
    u32 chat_name[8];             // lobby chat: name colour per member id (cycled)
    // type sizes (px at 1080p)
    float t_tiny, t_small, t_body, t_label, t_title, t_large, t_huge;
    float tracking_caps;          // small-caps letter spacing (em)
    // metrics
    float button_h, row_h, radius, radius_panel, border, accent_bar, pad, gap;
    float slot, slot_gap, modal_max_w_pct, scroll_speed;
    float safe_frac, max_aspect;
    // animation (seconds)
    float menu_in, menu_stagger, hover_t, modal_in, shake, tooltip_delay;
    // backdrop
    float blur, dim;
    // sounds
    char snd_hover[32], snd_click[32], snd_back[32], snd_open[32], snd_error[32], snd_drop[32], snd_equip[32], snd_tab[32];
    float snd_volume;
    char font_regular[96], font_medium[96], font_semibold[96];
    // "layout": named design-px sizes for screen JSON ("@name") and the screens' code (gui_tok)
    LayoutToken layout[GUI_THEME_MAX_TOKENS];
    int layout_count;
} GuiTheme;

const GuiTheme *gui_theme(void);
float gui_tok(const char *name);         // a "layout" token (design px); logs once and returns 0 if missing
bool gui_theme_load(const char *path);   // missing keys keep their defaults
u32 gui_rgba_mul(u32 c, float a);         // alpha multiplied
u32 gui_rgba_lerp(u32 a, u32 b, float t);

#endif
