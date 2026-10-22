// Dev scene registry and the scene command (src/game/scene.h).
#include "scene.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include <string.h>

#define MAX_SCENES 16

static struct {
    const GameScene *list[MAX_SCENES];
    int count;
    const GameScene *active;
} SC;

void scene_register(const GameScene *s) {
    if (!s || !s->name || SC.count >= MAX_SCENES) return;
    for (int i = 0; i < SC.count; i++) if (!strcmp(SC.list[i]->name, s->name)) return;
    SC.list[SC.count++] = s;
}

const GameScene *scene_active(void) { return SC.active; }
static bool own_window(void) { return SC.active && SC.active->own_window && SC.active->own_window(); }
bool scene_wants_mouse(void) { return SC.active && SC.active->free_cursor && !own_window(); }
bool scene_has_panels(void) { return SC.active && SC.active->draw && !own_window(); }
bool scene_hides_hud(void) { return SC.active && SC.active->hide_hud && !own_window(); }

void scene_leave(void) {
    const GameScene *s = SC.active;
    if (!s) return;
    SC.active = NULL;   // first: leave() may run commands that query the scene
    if (s->leave) s->leave();
    log_info("scene %s: left", s->name);
}

bool scene_enter(const char *name) {
    const GameScene *s = NULL;
    for (int i = 0; i < SC.count; i++) if (!strcmp(SC.list[i]->name, name)) s = SC.list[i];
    if (!s) { log_warn("scene: unknown scene '%s' (scene lists them)", name); return false; }
    if (SC.active == s) return true;
    scene_leave();
    if (s->enter && !s->enter()) return false;
    SC.active = s;
    log_info("scene %s: entered (scene off to leave)", s->name);
    return true;
}

bool scene_camera(struct World *w, RenderCamera *cam) { return SC.active && SC.active->camera && SC.active->camera(w, cam); }
void scene_draw(void) { if (SC.active && SC.active->draw) SC.active->draw(); }

static void cmd_scene(int argc, const char **argv) {
    if (argc > 1 && strcmp(argv[1], "off") != 0) { scene_enter(argv[1]); return; }
    if (SC.active) { scene_leave(); return; }
    if (argc > 1) return;   // "scene off" with nothing open
    log_info("scenes (scene <name>, scene / scene off to leave):");
    for (int i = 0; i < SC.count; i++) log_info("  %-12s %s", SC.list[i]->name, SC.list[i]->desc ? SC.list[i]->desc : "");
}

void scenes_init(void) { cmd_register("scene", cmd_scene, "scene <name>: open a dev scene (handpose, ...); scene / scene off: leave; no scene open: list them"); }
