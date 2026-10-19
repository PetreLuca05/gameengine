// First-person camera (docs/CAMERA_HOLDPOINT.md "Rule 2"): a gliding camera that lives on the head
// but ignores the animation's motion.
//
// Position = the nominal eye: the capsule's feet + a per-stance eye offset (stand / crouch / swim /
// hang / downed / mantle x weapon hold style x speed x view pitch, the head eye's mean over each clip
// cycle: assets/player/eye_nominal.json, measured by cam_calibrate from the shared animation), never
// the animated head. Stance, speed and style changes glide through a critically damped spring on the
// table's weights (cam_glide_stiffness / _damping); grounded step-ups / -downs are absorbed by the same
// spring; jumps and falls follow the capsule; a landing adds no dip (the optional movement shake does).
// The animated head eye only validates it (cam_anchor_radius, cam_anchor_report): the body is never
// moved for the camera. Rotation is the view angles (+ the optional shake layers), never a bone's.
// A sphere cast keeps the eye out of walls and ledges; death: the ragdoll head camera (the exception).
#ifndef DG_CAMERA_FP_H
#define DG_CAMERA_FP_H

#include "core/dgmath.h"

struct cJSON;

typedef struct CamHeadStab {
    // death camera
    bool dead;
    vec3 death_pos, death_vel;
    float death_t;
    // collision
    float col_push;
} CamHeadStab;

typedef struct CamFpCvars {
    struct Cvar *glide_stiffness, *glide_damping, *anchor_radius, *anim_influence, *recon_smooth, *third, *debug;
    struct Cvar *radius, *death_follow, *death_spectate, *kick_scale, *move_shake;
} CamFpCvars;

// ------------------------------------------------------------------ nominal eye (pure, camera_fp_math.c)

enum { EYE_STAND, EYE_CROUCH, EYE_SWIM, EYE_HANG, EYE_DOWNED, EYE_MANTLE, EYE_DIVE, EYE_POSTURES };
#define EYE_PITCH_N 7       // view pitch samples (deg): -85 -60 -30 0 30 60 85
#define EYE_SPEED_N 4       // speed samples per posture (mantle: traversal progress 0..1)
#define EYE_STYLES 16       // hold styles (the weapon type)

typedef struct EyeNominalTable {
    bool valid;
    u32 anim_hash;                                   // the animation library it was measured from (anim_manifest_combined)
    float pitch[EYE_PITCH_N];                        // deg
    int speed_n[EYE_POSTURES];
    float speed[EYE_POSTURES][EYE_SPEED_N];          // m/s (mantle: progress)
    bool style_valid[EYE_STYLES];
    // (right, up, forward) m from the capsule's feet in the view's yaw frame
    vec3 off[EYE_POSTURES][EYE_STYLES][EYE_SPEED_N][EYE_PITCH_N];
    float residual[EYE_POSTURES];                    // the head eye's largest distance from the mean while measured (m)
} EyeNominalTable;

const char *eye_posture_name(int p);
void eye_table_defaults(EyeNominalTable *t);     // the sample grid, nothing measured
void eye_table_parse(EyeNominalTable *t, const struct cJSON *root);
char *eye_table_to_json(const EyeNominalTable *t);   // caller frees
// The table at fractional coordinates (missing styles fall back to style 0).
vec3 eye_table_sample(const EyeNominalTable *t, int posture, int style, float speed, float pitch_rad);
// The table's content (offsets quantized to 0.1 mm): part of the join manifest, so every machine looks
// through the same eye.
u32 eye_table_hash(const EyeNominalTable *t);

// What the nominal eye depends on (all replicated / simulated: spectators reproduce it).
typedef struct CamNominalIn {
    int posture;            // EYE_SWIM / EYE_DIVE / EYE_HANG / EYE_DOWNED / EYE_MANTLE, or -1: on foot (stand / crouch by `crouch`)
    float crouch;           // 0..1
    float speed;            // horizontal m/s
    float progress;         // mantle / vault progress 0..1
    int style;              // the held item's hold style (0: none)
    float pitch;            // view pitch (rad): not smoothed (the head turns with the view)
} CamNominalIn;

typedef struct CamGlideParams { float stiffness, damping; } CamGlideParams;
CamGlideParams cam_glide_params_default(void);

// The glide: springs on the table's weights (postures, speed, style) and on the vertical step offset.
typedef struct CamGlide {
    bool init;
    float w[EYE_POSTURES], wv[EYE_POSTURES];
    float speed, speed_v;
    float sw[EYE_STYLES], swv[EYE_STYLES];   // hold style weights (an interrupted change never pops)
    float step, step_v;
    float last_y;
    bool last_grounded, step_init;
} CamGlide;

void cam_glide_reset(CamGlide *g);
// The eye offset (right, up, forward m, view yaw frame) for this frame.
vec3 cam_glide_offset(CamGlide *g, const EyeNominalTable *t, const CamNominalIn *in, float dt, const CamGlideParams *p);
// The vertical offset added to the capsule this frame: grounded snaps (steps: faster vertically than
// any slope or lift goes) glide out, bounded to cam_step_max so stairs never sink the eye into the body;
// everything else (jumps, falls, slopes, lifts) follows the capsule.
float cam_glide_step(CamGlide *g, float capsule_y, float speed_h, bool grounded, float dt, const CamGlideParams *p);
bool cam_glide_settled(const CamGlide *g, const CamNominalIn *in);   // the weights reached this input's targets

// Anchor statistics: the nominal eye against the animated head eye (never corrects anything).
// Eligible frames: settled on-foot motion without a sprint, hit reaction, landing or traversal - the
// anchor radius holds there; elsewhere the animation moves the head away by design and it's reported.
typedef struct CamAnchorStats {
    u32 frames, over, elig, elig_over;
    float max_d, sum_d, elig_max;
    float max_posture[EYE_POSTURES];
} CamAnchorStats;
void cam_anchor_add(CamAnchorStats *s, float d, float radius, int posture, bool eligible);

// ------------------------------------------------------------------ engine side (camera_fp.c)

void camera_fp_register_cvars(void);
const CamFpCvars *camera_fp_cvars(void);
CamGlideParams camera_fp_glide_params(void);
void camera_fp_reset(CamHeadStab *s);
// Damped follow of a (ragdoll) head; after cam_death_spectate seconds the camera lifts away.
vec3 camera_fp_death(CamHeadStab *s, vec3 head, float dt);
typedef bool (*CamSphereCastFn)(void *ctx, vec3 from, vec3 delta, float radius, float *fraction);
// Keeps the eye at least `radius` from geometry along the line from a safe point (capsule center).
vec3 camera_fp_collide(CamHeadStab *s, vec3 safe, vec3 eye, CamSphereCastFn cast, void *ctx, float dt);
// The nominal eye table (assets/player/eye_nominal.json); cam_calibrate rewrites it.
const EyeNominalTable *camera_fp_eye_table(void);
// The nominal eye's inputs from the movement state (posture, crouch, speed, traversal progress).
struct PlayerMove;
void camera_fp_nominal_in(CamNominalIn *ni, const struct PlayerMove *m, bool downed, int style, float pitch);
// The tick's nominal eye (no glide): the camera ray's origin every machine computes alike (docs/AIM.md).
bool camera_fp_nominal_eye(const struct PlayerMove *m, bool downed, int style, float yaw, float pitch, vec3 *out);
EyeNominalTable *camera_fp_eye_table_mut(void);
bool camera_fp_eye_table_load(void);
bool camera_fp_eye_table_save(const char *path_override);
bool camera_fp_eye_table_save_runtime(void);   // the build's copy only (an automatic recalibration)

#endif
