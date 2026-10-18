// Vista sequences (docs/STORY.md "Vista"): what the city outside does once a chapter's `sequence` actions
// start them: district power (window lights, the flickering district, blackouts), helicopters (a pass, a
// crash), smoke columns, fires, flashes, traffic density / stalls, bird flocks scattering. Pure: the
// state is a function of the started sequences and their ages (story seconds), so `story_time` scrubbing,
// saves and late joiners see the same city; src/game/game_vista.c draws it. tests/test_story.c tests it.
//
// assets/story/vista/sequences.json, one object per sequence name:
//   "power":   [{"t": s, "sector": 0..7, "to": v, "fade": s}]   v: 0 dark .. 1 normal, > 1 flickering
//   "traffic": {"density": 0..1, "stall": 0..1, "dark": 0..1, "ramp": s}
//   "heli":    {"path": [point, point], "duration": s}  or  {"from": point, "to": point, "approach": m,
//              "duration": s, "crash": true}  (spirals down onto "to")
//   "smoke":   [{"at": point, "t": s, "grow": s, "rise": m/s, "life": s, "size": [m, m], "rate": /s,
//              "shade": [base, top], "spread": m, "drift": [x, z] m/s}]
//   "fire":    [{"at": point, "t": s, "size": m}]
//   "flash":   [{"at": point, "t": s, "time": s, "size": m}]
//   "birds":   {"scatter": true}   (the idle flocks over the park and the river scatter and leave)
#ifndef DG_VISTA_SEQ_H
#define DG_VISTA_SEQ_H

#include <stdbool.h>

#define VISTA_MAX_SEQ 32
#define VISTA_MAX_ITEMS 8
#define VISTA_MAX_POWER 16
#define VISTA_NAME 32

typedef struct VistaPowerKey { float t, to, fade; int sector; } VistaPowerKey;
typedef struct VistaSmokeDef {
    char at[VISTA_NAME];
    float t, grow, rise, life, size0, size1, rate, shade0, shade1, spread, drift[2];
} VistaSmokeDef;
typedef struct VistaSpotDef { char at[VISTA_NAME]; float t, time, size; } VistaSpotDef;   // fires, flashes

typedef struct VistaSeqDef {
    char name[VISTA_NAME];
    VistaPowerKey power[VISTA_MAX_POWER];
    int power_n;
    bool traffic;
    float density, stall, dark, ramp;      // density / stall / dark < 0: unchanged
    bool heli, crash;
    char heli_from[VISTA_NAME], heli_to[VISTA_NAME];
    float heli_time, approach;
    VistaSmokeDef smoke[VISTA_MAX_ITEMS];
    int smoke_n;
    VistaSpotDef fire[VISTA_MAX_ITEMS], flash[VISTA_MAX_ITEMS];
    int fire_n, flash_n;
    bool birds_scatter;
} VistaSeqDef;

typedef struct VistaSeqLib {
    VistaSeqDef seq[VISTA_MAX_SEQ];
    int n;
} VistaSeqLib;

// A started sequence and its age (story seconds since its `sequence` action).
typedef struct VistaActive { char name[VISTA_NAME]; float age; } VistaActive;

// Named points (the level's story points); false if unknown.
typedef bool (*VistaPointFn)(void *user, const char *name, float out[3]);

typedef struct VistaHeli { float pos[3], yaw, rotor, roll; bool crashing; } VistaHeli;
typedef struct VistaSmoke { float pos[3], age; const VistaSmokeDef *def; } VistaSmoke;   // age since the column began
typedef struct VistaSpot { float pos[3], age, size, time; } VistaSpot;

typedef struct VistaFrame {
    float power[8];                        // per 45 degree sector (render_set_city)
    float traffic_density, traffic_stall, traffic_dark;
    float stall_age;                       // seconds since the stall began (< 0: none)
    VistaHeli heli[4];
    int heli_n;
    VistaSmoke smoke[16];
    int smoke_n;
    VistaSpot fire[16], flash[8];
    int fire_n, flash_n;
    float birds_scatter_age;               // < 0: the flocks circle
} VistaFrame;

// JSON text -> lib (false on a parse error; err gets a message).
bool vista_seq_parse(VistaSeqLib *lib, const char *json, char *err, int err_size);
const VistaSeqDef *vista_seq_find(const VistaSeqLib *lib, const char *name);
// The city now. act: started sequences in start order (later ones override earlier power / traffic).
void vista_eval(const VistaSeqLib *lib, const VistaActive *act, int n, VistaPointFn point, void *user, VistaFrame *out);
// Smoke column particle k (deterministic): false if not alive at this age. pos, size (m), shade 0..1, alpha.
bool vista_smoke_particle(const VistaSmoke *s, int k, float out_pos[3], float *size, float *shade, float *alpha);
int vista_smoke_count(const VistaSmoke *s);   // particles born so far (upper bound for k)

#endif
