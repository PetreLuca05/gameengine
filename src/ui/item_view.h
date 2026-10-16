// Item models for the UI (docs/UI.md "Icons and preview"): the parts of an item as the hands
// draw it (weapon boxes with mounted attachments, held-item boxes, attachment parts), framed by
// an orbit camera. Used by the inventory preview (live, rotating) and the icon bakes.
#ifndef DG_ITEM_VIEW_H
#define DG_ITEM_VIEW_H

#include "core/base.h"
#include "core/dgmath.h"
#include "render/render.h"
#include "gameplay/items.h"

#define ITEM_VIEW_MAX_PARTS 96

// highlight_slot: an attachment slot to highlight (its socket marker + mounted parts), -1 none.
// Returns the part count; mn / mx: the model bounds (item space).
int item_view_parts(const ItemDB *db, u16 item, const u8 *att, int highlight_slot, RenderUi3dPart *out, int max, vec3 *mn, vec3 *mx);

typedef struct ItemViewCam {
    float yaw, pitch;         // radians, orbit around the bounds centre
    float zoom;               // 1 = fit
    bool ortho;               // icons: orthographic (clean silhouettes)
} ItemViewCam;
void item_view_default_cam(const ItemDB *db, u16 item, ItemViewCam *cam);
void item_view_matrices(const ItemViewCam *cam, vec3 mn, vec3 mx, float aspect, mat4 *view, mat4 *proj);
// Projects an item-space point into the view (0..1, y down); false when behind the camera.
bool item_view_project(const mat4 *view, const mat4 *proj, vec3 p, vec2 *out);

#endif
