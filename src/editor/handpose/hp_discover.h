// Hand pose discovery / auto-registration (docs/HANDPOSE.md). Pure (no world, no renderer): the
// game, tools/handpose_report and the tests run the same scan.
//
// Every holdable / grippable thing is a "source". Grip sources: weapons and items with a "hold"
// block, attachments that add a grip. Case sources: registered interactables, vehicle driver seats
// (and one passenger case), every gun's reload, the ledge grab, the phone in portrait. A scan
// creates the missing grip of every grip source - a stub that inherits its style and overrides
// nothing - and a stub case for every case source no case is bound to, written to
// assets/handposes/{items,cases}/<id>.json when a write directory is given (never over an existing
// file). Grips / cases whose source is gone are flagged orphan (listed, never removed silently).
#ifndef DG_HP_DISCOVER_H
#define DG_HP_DISCOVER_H

#include "anim/handpose.h"

struct ItemDB;
struct ProcParams;
struct VehicleDef;

typedef struct HandPoseSource {
    bool is_case;
    u8 category;               // grips: HandPoseCategory (weapon / item / attachment)
    u8 kind;                   // cases: HandCaseKind
    char name[48];             // grips: id without the category prefix ("rifle", "vertical_grip"); cases: the stub's id ("use_switch")
    char bind[64];             // cases: what binds it ("interactable:switch")
    char label[48];
    char source[128];          // the asset ("data/weapons/02_rifle.json", "interactable:door")
    char inherits[48];         // "style/rifle", "style/grip_grab"
    u8 hands;                  // bit 0 left, bit 1 right
    bool auto_place;           // interactables without a hand target in code: the stub's anchor comes from the type (needs review)
    Xform auto_socket[2];
    // case setup
    char prop[24], vehicle[24], item[32], hold_state[24];
    i8 seat;
    float hold_time;           // interactables: > 0 = a held interaction (the hold segment of the timeline)
} HandPoseSource;

typedef struct HandPoseSourceList {
    HandPoseSource *v;
    int count, cap;
    u32 categories;            // bit per HandPoseCategory this list covers (others are never orphaned)
    u32 case_kinds;            // bit per HandCaseKind this list covers
} HandPoseSourceList;

void hp_sources_free(HandPoseSourceList *l);
HandPoseSource *hp_sources_add(HandPoseSourceList *l, int category, const char *name);
HandPoseSource *hp_sources_add_case(HandPoseSourceList *l, int kind, const char *id, const char *bind);

// Collectors.
void hp_sources_from_items(HandPoseSourceList *l, const struct ItemDB *db, const struct ProcParams *p);   // grips + reload / phone cases
void hp_source_from_vehicle(HandPoseSourceList *l, const struct VehicleDef *d, const char *source);
int hp_sources_from_vehicle_dir(HandPoseSourceList *l, const char *dir_rel);   // every *.json (new files included) + the passenger case
// Interactables: hand mode (0 right / free hand, 1 both, 2 point: world.h HandMode), hold time,
// whether the gameplay code computes the hand target (else the stub is auto-placed).
void hp_source_from_interactable(HandPoseSourceList *l, const char *name, int hand_mode, float hold_time, bool has_hand_target, const char *source);
void hp_sources_builtin(HandPoseSourceList *l);   // the ledge grab (mantle_ledge), the ladder stub

typedef struct HandPoseScanOpts {
    const char *write_dir;     // absolute assets/handposes to write new stubs into; NULL = memory only
    const char *copy_dir;      // the runtime copy (next to the executable) written too; NULL = none
    bool log;
} HandPoseScanOpts;

typedef struct HandPoseScanReport {
    int sources, created, written, existing, orphans, unreviewed, auto_placed;
    int cases, cases_unreviewed;
    char created_ids[32][48];  // the first 32 created this scan (dev log / editor badge)
} HandPoseScanReport;

// Creates missing grips / case stubs, refreshes the sources' hands / labels in memory, flags
// orphans. Relinks and rehashes the DB.
void hp_scan(HandPoseDB *db, const HandPoseSourceList *src, const HandPoseScanOpts *o, HandPoseScanReport *r);
void hp_count(const HandPoseDB *db, HandPoseScanReport *r);   // unreviewed / auto-placed / orphan totals
// Deletes an orphan's file (dir = absolute assets/handposes) and its entry / case. False if not an orphan.
bool hp_remove_orphan(HandPoseDB *db, int entry, const char *dir, const char *copy_dir);
bool hp_remove_orphan_case(HandPoseDB *db, int ci, const char *dir, const char *copy_dir);
// Writes a grip / case to dir (+ copy_dir), creating the directories.
bool hp_write_entry(const HandPoseEntry *e, const char *dir, const char *copy_dir);
bool hp_write_case(const HandPoseCase *c, const char *dir, const char *copy_dir);
// Human-readable report (unreviewed, auto-placed, orphans) into buf.
int hp_report_text(const HandPoseDB *db, char *buf, int size);

#endif
