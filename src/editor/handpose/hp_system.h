// Game side of hand poses (docs/HANDPOSE.md): discovery / auto-registration at startup, on map
// load and on every reload; the handpose_* commands; hot reload of external edits to
// assets/handposes; the hand target of interactables whose code has none (auto-placed entries).
#ifndef DG_HP_SYSTEM_H
#define DG_HP_SYSTEM_H

#include "hp_discover.h"
#include "world/world.h"

void handpose_system_init(void);              // commands + cvars (game_init)
void handpose_system_startup(void);           // first scan, once items / vehicles / interactables are registered
void handpose_system_scan(World *w, bool log);// collect every source, create missing entries, update the manifest
void handpose_system_poll(void);              // per frame: external edits -> reload (dev, ~1 Hz)
bool handpose_system_reload(void);            // handpose_reload: re-read files, rescan
const HandPoseScanReport *handpose_system_report(void);
// Where entries are saved: the source tree (dev builds, NULL otherwise) and the runtime copy.
const char *handpose_system_src_dir(void);
const char *handpose_system_rt_dir(void);
// Saves one entry (both dirs) and re-reads nothing (the DB is the source of truth while editing).
bool handpose_system_save(int entry);
void handpose_system_note_own_write(void);    // don't treat our own saves as external edits

// Interactables: the code's hand target, or - for an auto-placed entry a person reviewed - the top
// of the entity's bounds facing the user (the entry's relative socket refines it).
bool handpose_interact_has_target(const InteractDef *def);
bool handpose_interact_target(World *w, EntityId e, int player, int hand, vec3 *pos, quat *rot);

#endif
