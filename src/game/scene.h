// Dev scenes (console: scene <name>, scene / scene off to leave): a mode that takes over the view
// and the HUD of the running game (a camera, a panel) without loading another map. One scene is
// active at a time; each registers itself at init (scene_register). docs/HANDPOSE.md "Editor".
#ifndef DG_SCENE_H
#define DG_SCENE_H

#include "core/base.h"
#include "render/render.h"

struct World;

typedef struct GameScene {
    const char *name;          // "handpose"
    const char *desc;          // one line for the scene list
    bool (*enter)(void);       // false: can't open now (logs why), the scene stays closed
    void (*leave)(void);
    // Per render frame, after the game's camera: return true to own the camera (third person: the
    // local body is drawn whole, no first-person pass). NULL / false = the normal view.
    bool (*camera)(struct World *w, RenderCamera *cam);
    void (*draw)(void);        // Nuklear panels (after the world, before menus / console)
    bool free_cursor;          // the panels need the mouse (the game loses look / move input)
    bool hide_hud;
    // Optional: true while the scene's panels are in their own OS window (ui_window2_*): the game
    // window keeps its HUD, mouse and input (free_cursor / hide_hud / the Nuklear events don't apply).
    bool (*own_window)(void);
} GameScene;

void scenes_init(void);                       // the scene command (game_init)
void scene_register(const GameScene *s);      // s must outlive the game (static)
bool scene_enter(const char *name);           // leaves the current one first
void scene_leave(void);
const GameScene *scene_active(void);
bool scene_wants_mouse(void);
bool scene_has_panels(void);                  // the scene draws Nuklear panels (dev tool UI gets the events; not the main menu)
bool scene_hides_hud(void);
bool scene_camera(struct World *w, RenderCamera *cam);   // game_render: true = the scene's camera (third person)
void scene_draw(void);

#endif
