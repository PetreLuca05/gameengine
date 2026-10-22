// Main menu internals (src/ui/mainmenu): shared state, layouts and drawing helpers of the tabs.
#ifndef DG_MM_INTERNAL_H
#define DG_MM_INTERNAL_H

#include "mainmenu.h"
#include "net/lobby_net.h"
#include "render/render.h"

#define TH (gui_theme())
#define TOK(n) gui_s(gui_tok(n))

typedef enum MmTab { MT_PLAY, MT_PLAYER, MT_COSMETICS, MT_SETTINGS, MT_NEWS, MT_CREDITS, MT_COUNT } MmTab;
typedef enum MmPlayPage { PP_MAIN, PP_BROWSER, PP_CHAPTERS, PP_TRIALS } MmPlayPage;   // the maps view: Story > Chapter select, Trials > Enroll
typedef enum MmPlayMode { PM_TRIALS, PM_STORY } MmPlayMode;

// A screen layout (assets/ui/screens/<name>.json) solved into a rect each frame.
#define MM_LAYOUT_NODES 48
typedef struct MmLayout {
    char file[48];
    LayoutNode nodes[MM_LAYOUT_NODES];
    int count;
    bool loaded;
} MmLayout;
bool mm_layout_load(MmLayout *l, const char *name);
// Solves inside r (safe roots: against the window's safe area) and returns the root rect.
GuiRect mm_layout_solve(MmLayout *l, GuiRect r);
GuiRect mm_node(const MmLayout *l, const char *id);
const LayoutNode *mm_node_info(const MmLayout *l, const char *id);

typedef struct MainMenu {
    bool inited, enabled, active;
    int tab;
    int play_page;
    int play_mode;                 // MmPlayMode
    char trials_map[64];           // the last development map (Trials comes back to it)
    double open_time, boot_time;
    bool splash;
    char notice[192];              // shown once on the Play tab (Disconnected: ..., Kicked ...)
    double notice_time;
    // selection
    char map[32];                  // the map selected in the Maps tab (host: the lobby's map)
    int season, hour;
    char weather[24];              // "" = the season's pick
    bool dynamic_weather;
    bool show_dev_maps;
    bool cos_items;                // Cosmetics: the Items sub-tab (else Player)
    float items_t;                 // 0..1 towards the Items close-up (the background's camera)
    bool bg_played;
    bool party_auto;               // the menu is opening our own party (a failure is quiet: retried)
    double party_retry;            // gui_time of the next attempt
    char party_err[128];                // a session ran since the menu world was made (the next menu loads a fresh one)
    bool env_dirty;                // the background must take the selection's look
    // player preview (scene camera)
    float orbit_yaw, orbit_pitch, orbit_zoom;
    bool preview_tod;              // Player tab: "Preview in map lighting" (else neutral studio-ish noon)
    GuiRect preview_rect;          // where the character should be framed this frame (0 = background drift)
    // cosmetics
    int cos_slot;
    int cos_selected;              // item index, -1 none
    // layouts
    MmLayout shell, play, browser, player, cosmetics, maps, settings, credits, loading, chat, phone_card, phone_portal;
    // Steam avatar
    TextureId avatar;
    u64 avatar_id;
    double avatar_try;
} MainMenu;

MainMenu *mm(void);

// tabs (mm_*.c)
void mm_play_draw(GuiRect content);
void mm_player_draw(GuiRect content);
void mm_cosmetics_draw(GuiRect content);
void mm_maps_draw(GuiRect content, bool trials);   // the maps view: Trials > Enroll / Story > Chapter select
void mm_maps_background(GuiRect content, float alpha);   // the globe, before the shading and the panels
void mm_credits_draw(GuiRect content);
void mm_play_start(void);
void mm_play_story(const char *chapter, const char *slot);   // a story chapter for the party (host)
void mm_set_play_mode(int mode);       // Trials / Story (the selection follows: a development map / a chapter)
void mm_party_sync(void);              // the host's party takes the menu's map / season / time / weather (mm_scene.c)              // the party's game (host): ready, then start / the countdown
void mm_lobby_players(GuiRect r);      // the party's player rows (mm_lobby.c)
const char *mm_lobby_offline_text(void);   // why there is no party right now (mainmenu.c)
void mm_news_draw(GuiRect content);     // Patch notes tab (assets/ui/news.txt)
void mm_loading_draw(void);

// scene (mm_scene.c)
void mm_scene_register(void);
void mm_scene_enter(void);
void mm_background_draw(bool globe);   // the pale blue backdrop + the character (globe: the backdrop only)
void mm_scene_apply_environment(void); // the selection's season / time / weather on the background world
void mm_select_map(const char *id);    // Maps tab / lobby: selection, defaults from the map, background

// phone (mm_phone.c, docs/PHONE.md "Phone flow"): the glyph, the footer chip (returns its width)
void mm_phone_glyph(GuiRect r, u32 col, bool lit);
float mm_phone_status_chip(GuiRect r);
const char *mm_phone_status_text(char *b, size_t n);

// shared drawing
void mm_panel(GuiRect r);              // the pause menu's panel (blurred, semi-transparent, hairline)
void mm_title(GuiRect r, const char *text);           // small-caps section title like the pause stack's
void mm_kv(GuiCursor *c, const char *key, const char *value);   // "Key ........ value" row
void mm_thumb(GuiRect r, const char *path);           // a map thumbnail (cached texture), letterboxed
TextureId mm_texture(const char *path);               // cached PNG texture (0 = missing)
// Chat log + input (lobby screen, pause > Lobby, in-game chat). Returns true when a line was sent.
bool mm_chat_panel(GuiRect r, bool input, bool keep_editing, float fade_after);
u32 mm_name_color(int member_id);
void mm_footer_hint(GuiRect r, const char *text);
// Season / start time / start weather / dynamic rows on a settings cursor (Maps tab, lobby centre).
// Returns true when something changed; enabled false: read-only.
bool mm_environment_rows(GuiCursor *c, LobbySettings *s, bool enabled);

#endif
