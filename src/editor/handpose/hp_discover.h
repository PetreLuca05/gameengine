// Hand pose discovery / auto-registration (docs/HANDPOSE.md). Pure (no world, no renderer): the
// game, tools/handpose_report and the tests run the same scan.
//
// Every holdable / grippable thing is a "source": weapons and items with a "hold" block, attachments
// that add a grip, vehicle definitions with steering controls, registered interactables. A scan
// creates the hand pose entry of every source that has none - a stub that inherits its style (or the
// interactable / vehicle grip default), lists the hands and states the source uses and overrides
// nothing - and writes it to assets/handposes/<id>.json when a write directory is given (never over
// an existing file). Entries whose source is gone are flagged orphan (listed, never removed silently).
#ifndef DG_HP_DISCOVER_H
#define DG_HP_DISCOVER_H

#include "anim/handpose.h"

struct ItemDB;
struct ProcParams;
struct VehicleDef;

typedef struct HandPoseSource {
    u8 category;               // HandPoseCategory (not HPC_STYLE)
    char name[48];             // id without the category prefix ("rifle", "vertical_grip", "car", "door")
    char label[48];
    char source[128];          // the asset ("data/weapons/02_rifle.json", "interactable:door")
    char inherits[48];         // "style/rifle", "style/grip_grab"
    u8 hands;                  // bit 0 left, bit 1 right
    u32 states;                // HandPoseState bits
    bool relative;             // socket = offset from a gameplay target (vehicles, interactables)
    bool auto_place;           // no gameplay target: the stub gets a default socket from the type (needs review)
    Xform auto_socket[2];
} HandPoseSource;

typedef struct HandPoseSourceList {
    HandPoseSource *v;
    int count, cap;
    u32 categories;            // bit per HandPoseCategory this list covers (others are never orphaned)
} HandPoseSourceList;

void hp_sources_free(HandPoseSourceList *l);
HandPoseSource *hp_sources_add(HandPoseSourceList *l, int category, const char *name);

// Collectors. States come from the hold styles' hold point states (the ones a style sets).
u32 hp_style_states(const struct ProcParams *p, const char *style);
void hp_sources_from_items(HandPoseSourceList *l, const struct ItemDB *db, const struct ProcParams *p);
void hp_source_from_vehicle(HandPoseSourceList *l, const struct VehicleDef *d, const char *source);
int hp_sources_from_vehicle_dir(HandPoseSourceList *l, const char *dir_rel);   // every *.json (new files included)
// Interactables: hand mode (0 right / free hand, 1 both, 2 point: world.h HandMode), hold time,
// whether the gameplay code computes the hand target (else the stub is auto-placed).
void hp_source_from_interactable(HandPoseSourceList *l, const char *name, int hand_mode, float hold_time, bool has_hand_target, const char *source);

typedef struct HandPoseScanOpts {
    const char *write_dir;     // absolute assets/handposes to write new stubs into; NULL = memory only
    const char *copy_dir;      // the runtime copy (next to the executable) written too; NULL = none
    bool log;
} HandPoseScanOpts;

typedef struct HandPoseScanReport {
    int sources, created, written, existing, orphans, unreviewed, auto_placed;
    char created_ids[32][48];  // the first 32 created this scan (dev log / editor badge)
} HandPoseScanReport;

// Creates missing entries, refreshes the sources' hands / states / labels in memory, flags orphans.
// Relinks and rehashes the DB.
void hp_scan(HandPoseDB *db, const HandPoseSourceList *src, const HandPoseScanOpts *o, HandPoseScanReport *r);
void hp_count(const HandPoseDB *db, HandPoseScanReport *r);   // unreviewed / auto-placed / orphan totals
// Deletes an orphan's file (dir = absolute assets/handposes) and its entry. False if not an orphan.
bool hp_remove_orphan(HandPoseDB *db, int entry, const char *dir, const char *copy_dir);
// Writes an entry to dir (+ copy_dir), creating the directories. Never used for stubs over files.
bool hp_write_entry(const HandPoseEntry *e, const char *dir, const char *copy_dir);
// Human-readable report (unreviewed, auto-placed, orphans) into buf.
int hp_report_text(const HandPoseDB *db, char *buf, int size);

#endif
