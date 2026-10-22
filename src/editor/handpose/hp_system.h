// Game side of hand poses (docs/HANDPOSE.md): discovery / auto-registration at startup, on map
// load and on every reload; the handpose_* commands; hot reload of external edits to
// assets/handposes; which case an interaction / seat / ledge uses at runtime; the hand target of
// interactables whose code has none (auto-placed cases).
#ifndef DG_HP_SYSTEM_H
#define DG_HP_SYSTEM_H

#include "hp_discover.h"
#include "world/world.h"

void handpose_system_init(void);              // commands + cvars (game_init)
void handpose_system_startup(void);           // first scan, once items / vehicles / interactables are registered
void handpose_system_scan(World *w, bool log);// collect every source, create missing grips / cases, update the manifest
void handpose_system_poll(void);              // per frame: external edits -> reload (dev, ~1 Hz)
bool handpose_system_reload(void);            // handpose_reload: re-read files, rescan
const HandPoseScanReport *handpose_system_report(void);
// Where grips / cases are saved: the source tree (dev builds, NULL otherwise) and the runtime copy.
const char *handpose_system_src_dir(void);
const char *handpose_system_rt_dir(void);
// Saves one grip / case (both dirs) and re-reads nothing (the DB is the source of truth while editing).
bool handpose_system_save(int entry);
bool handpose_system_save_case(int ci);
void handpose_system_note_own_write(void);    // don't treat our own saves as external edits

// Runtime case lookup (hash of the case, 0 = none: the grip style alone).
const HandPoseCase *handpose_case_by_hash(u32 hash);
u32 handpose_interact_case(World *w, EntityId e);              // "interactable:<name>", its "/open" variant while EF_OPEN
u32 handpose_interact_case_name(const char *name, bool open);
u32 handpose_seat_case(World *w, int slot);                    // "vehicle:<def>:driver" / "vehicle:passenger"
float handpose_seat_case_time(u32 case_hash);                  // the "use" marker (the editor's timeline time while previewed)
float handpose_seat_case_weight(u32 case_hash);                // 1 (editor: the reach / return blend at the timeline time)
u32 handpose_mantle_case(void);                                // "mantle" when it has keys (0: the ledge grab as before)
u32 handpose_item_case(const char *bind);                      // "item:<id>:<action>" (night vision flip: "item:nvg:flip"), 0 = none
float handpose_case_duration(u32 case_hash);                   // its timeline length (s; 0 = none)
float handpose_mantle_case_time(float progress);               // traversal progress 0..1 (< 0 = hanging: the "use" marker)
// Reloads (reload_<weapon>, once reviewed): the hand IK weight at `elapsed` of a `total` s reload
// from the case's hand-off markers; < 0 = no authored case (the controller's transitions).
float handpose_reload_ik_weight(const char *weapon, float elapsed, float total);
// Editor: a case previewed at timeline time t (seats, ledges); hash 0 = off.
void handpose_case_preview(u32 case_hash, float t);
// Vehicles: the anchor of a seat's case (the steering wheel / handlebar offset from the def's
// steering frame, the passenger hands from the seat), vehicle space. Identity when none.
Xform handpose_seat_anchor(const char *vehicle, bool driver);

// Interactables: the code's hand target, or - for an auto-placed case a person reviewed - the top
// of the entity's bounds facing the user (the case anchor / keys refine it).
bool handpose_interact_has_target(const InteractDef *def);
bool handpose_interact_target(World *w, EntityId e, int player, int hand, vec3 *pos, quat *rot);

#endif
