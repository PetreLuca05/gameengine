// First-person arms layer (docs/CAMERA_HOLDPOINT.md "Rule 3"): the owner's arms (upper arms down)
// and the held item are drawn a second time from the same pose - the same bones everyone else sees -
// with their own camera and FOV. This module is that camera, pure math: it never touches a bone.
//
// The arms camera is the pose eye (head bone x eye socket, the view's rotation: where the hold point
// is defined). The layer moves rigidly in that camera's space, applied to the drawn arms + item:
//   present  the rigid move from the drawn item onto the presented one (the presentation hold point:
//            ADS happens here, never in the model; at the hip too, so the item and the hands keep
//            their place on screen at any view pitch whatever the body's reach)
//   tuned    offset / rotation / scale per hold point state and per item (assets/player/arms_layer.json),
//            about the hold point; in ADS only the push along the view axis, the roll and the scale
//            about the sight (the sight stays on the view axis)
//   preset   Weapon Field of View: the layer moves sideways with the preset's FOV so the sight keeps
//            its place on screen (the item looks larger with Narrow, smaller with Wide)
//   sway     the player's cosmetic "Weapon sway" (the pose's own sway shown smaller)
//   kick     the player's cosmetic "Weapon kick" (the pose's own kick shown smaller) + the first-person
//            screen kick towards the eye
//   camera   the arms camera's own position against the pose eye, per state / item (the hip framing:
//            the camera moves, never turns; the arms and item stay where the body holds them); back on
//            the eye aimed
//   equip    a newly selected item comes up from below the view (out of the arms frustum) to its hold
//   aim      a rotation about the eye that puts the barrel where the world camera shows the shot
//            (sway, the ADS counter of the camera kick, the Hands FOV against the world FOV)
//   screen   the layer's picture scaled about a point on the screen (the bottom edge): bigger or
//            smaller on screen, the same place; back to 1 aimed (arms_layer_screen_clip / _view, the renderer)
#ifndef DG_ARMS_LAYER_H
#define DG_ARMS_LAYER_H

#include "core/base.h"
#include "core/dgmath.h"
#include "player/holdpoint.h"
#include "player/fov.h"

struct cJSON;

// A rigid placement of the layer in camera space: offset (right, up, forward) m, rotation (pitch,
// yaw, roll) deg as the hold point's, uniform scale, and degrees added to the arms FOV (hip). The
// arms camera's own position against the pose eye: cam_pos (right, up, forward m; it moves, never
// turns) - it frames the arms + item without moving them.
typedef struct ArmsPose { vec3 offset, rotation; float scale, fov_add; vec3 cam_pos; } ArmsPose;

#define ARMS_MAX_ITEMS 64
typedef struct ArmsItem {
    char id[32];
    bool set[HP_STATE_COUNT];
    ArmsPose state[HP_STATE_COUNT];
    float screen_scale;               // the item's own screen size at the hip (0: the layer's screen.scale)
} ArmsItem;

typedef struct ArmsLayerDef {
    ArmsPose state[HP_STATE_COUNT];
    float blend[HP_STATE_COUNT];      // s into each state (state / item changes ease over it)
    ArmsItem items[ARMS_MAX_ITEMS];
    int item_count;
    float equip_time;                 // s from below the view to the hold point (the gun is ready at 0.3 s)
    float equip_pitch;                // deg muzzle down at the start of the raise
    float equip_margin;               // m below the frustum's bottom edge
    bool seam;                        // shoulder seam on: arm vertices within r0 of the upper-arm joint are drawn with
    float seam_r0, seam_r1;           // the world camera, past r1 with the arms camera (m); off: the arms camera draws
                                      // the arms + item only, the body is the world camera's
    float znear;                      // the arms projection's near plane (m)
    float arm_cut;                    // m: the arms (upper arm down) are cut closer than this to the arms camera
    float weapon_fov[WEAPON_FOV_COUNT];   // Weapon Field of View presets: narrow, default, wide (hip, horizontal deg at 16:9)
    float screen_scale;               // the layer's size on screen at the hip (0.25 .. 2; 1 aimed) ...
    vec2 screen_anchor;               // ... about this screen point (fractions: x from the left, y from the top; 1 = the bottom edge)
} ArmsLayerDef;

typedef struct ArmsLayerState {
    bool init;
    int state;
    u32 item_hash;
    float t, dur;
    ArmsPose from, cur;
    bool hip_sight_ok, ads_sight_ok;
    u32 sight_item;                   // the item they were taken with
    vec3 hip_sight, ads_sight;        // the presented sight's last hip / aimed place (pose-eye space): the camera's ADS weight
} ArmsLayerState;

typedef struct ArmsLayerIn {
    Xform pose_eye;                   // world
    Xform present;                    // pose-eye space: drawn item -> presented item (identity: none)
    bool has_item;
    Xform item_cam;                   // the drawn item, pose-eye space
    vec3 pivot, sight;                // item space
    float length;                     // m
    int hp_state;                     // the presentation hold point's state
    float ads_w;                      // 0..1 ADS progress (the sight's on the axis at 1)
    const char *item_id;
    float equip_t;                    // s since the item was selected (< 0: no raise)
    vec2 aim_deg;                     // the shot against the render view: deg right / up
    float aim_pitch;                  // the render view's pitch the offset is measured at (rad)
    float fov_world, fov_hands;       // vertical deg (fov_hands: the arms projection, ADS zoom included)
    vec3 kick_pos, kick_rot;          // the pose's gun kick: camera space m (+z back) / deg (pitch up, yaw right, roll)
    float kick_scale;                 // the player's cosmetic Weapon kick (1 = the pose's kick as it is)
    vec3 sway_hip_rot, sway_hip_pos;  // the hold point's sway this frame (deg about the pivot / camera space m) ...
    vec3 sway_ads_rot;                // ... and aimed (deg about the sight)
    float sway_scale;                 // the player's cosmetic Weapon sway (1 = the pose's sway as it is)
    float screen_kick;                // first-person screen kick (m towards the eye)
    float lateral_scale;              // Weapon Field of View: the sight's x / y scale by this at the hip so it keeps its
                                      // place on screen (fov_lateral_scale against the Default preset; 0 / 1 = none)
    float dt;
} ArmsLayerIn;

typedef struct ArmsLayerOut {
    mat4 view;                        // world -> arms camera space (the arms layer's view matrix)
    mat4 layer;                       // pose-eye space -> arms camera space (the rigid layer, scale included)
    float fov_add;                    // deg to add to the Hands FOV this frame
    float raise;                      // 0..1 how far the equip raise still has to go (1 = out of view)
    float cam_w;                      // the arms camera's weight this frame (1 at the hip, 0 aimed)
    float screen_scale;               // the layer's size on screen this frame (screen_scale at the hip, 1 aimed: by cam_w)
    vec3 sight;                       // the presented sight (pose-eye space)
} ArmsLayerOut;

void arms_layer_defaults(ArmsLayerDef *d);
void arms_layer_parse(ArmsLayerDef *d, const struct cJSON *root);
char *arms_layer_to_json(const ArmsLayerDef *d);   // caller frees
ArmsPose arms_layer_target(const ArmsLayerDef *d, int state, const char *item_id);
ArmsPose *arms_layer_edit(ArmsLayerDef *d, int state, const char *item_id);   // NULL item: the state default; creates the override
void arms_layer_reset(ArmsLayerState *st);
void arms_layer_eval(const ArmsLayerDef *d, ArmsLayerState *st, const ArmsLayerIn *in, ArmsLayerOut *out);
// The rotation about the eye that turns the arms camera's forward axis to where the world camera
// shows a shot dir(yaw - right, pitch + up) (pitch: the view's), with the arms projection's FOV.
quat arms_layer_aim_rot(vec2 aim_deg, float view_pitch, float fov_world, float fov_hands);
// The screen size: the arms picture scaled by s about anchor (screen fractions, y from the top). As a
// clip-space matrix after the arms projection (Vulkan NDC, y down; it leaves depth alone) ...
mat4 arms_layer_screen_clip(float s, vec2 anchor);
// ... and as the linear map from arms camera space to the world camera's view space that puts a point
// at the same pixel and view depth (k = tan(world / 2) / tan(arms / 2), tw = tan(world / 2), aspect w / h).
mat4 arms_layer_screen_view(float s, vec2 anchor, float k, float tw, float aspect);

#endif
