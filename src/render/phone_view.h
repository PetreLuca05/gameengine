// Phone camera views (docs/PHONE.md): offscreen cameras at a phone's lens, rendered into
// persistent targets through a light post stack (exposure + tonemap, no DoF / motion blur):
//   screen   display-linear image + the phone UI overlay: sampled by the phone's screen quad
//   scene    the clean sRGB camera image, optionally copied to the CPU (stream, photo, video)
//   panel    the screen image box-filtered down to the HUD panel's pixel size, sRGB-encoded,
//            with a rounded-corner alpha mask (the HUD phone panel: crisp at any panel size)
// A UI-only view (the phone's home screen, portal, gallery) skips the scene pass: the overlay is
// composited over black.
// View 0 is the local player's phone, view 1 a nearby remote phone (rendered at a low rate by the
// game), view 2 the scene of the second window (the hand pose editor's own window, docs/HANDPOSE.md:
// lines on, its scene image drawn by that window's UI in the same frame). A view rendered this frame
// is sampled from the next frame on (its passes run after the viewmodel); CPU copies complete two
// frames later and never stall the render thread.
#ifndef DG_PHONE_VIEW_H
#define DG_PHONE_VIEW_H

#include "render/render.h"

#define RENDER_PHONE_VIEWS 3
#define RENDER_PHONE_VIEW_WINDOW2 2   // the second window's scene

typedef struct RenderPhoneView {
    int view;                 // 0 .. RENDER_PHONE_VIEWS - 1
    vec3 pos;                 // lens position (world)
    quat rot;                 // lens orientation: looks down -Z, +Y up (the top of the screen)
    float fov_deg;            // vertical field of view (zoom = narrower)
    int width, height;        // target size (re-created when it changes)
    TextureId overlay;        // optional sRGB RGBA8 UI composited onto the screen image (0 = none)
    float exposure_bias;      // EV on top of the main view's auto exposure
    float flash;              // shutter flash on the screen image 0..1
    float vignette;           // camera vignette 0..1 (the scene image too)
    bool readback;            // copy the scene image to the CPU this frame
    bool mirror;              // front (selfie) camera: the screen preview is mirrored, the image is not
    bool ui_only;             // no lens view: the screen is the overlay alone (opaque UI apps)
    int panel_w, panel_h;     // the HUD panel image size in pixels (0 = no panel image)
    float panel_radius;       // its corner radius in pixels
    float znear;              // near plane (m, 0 = 0.05)
    bool lines;               // draw the second window's 3D lines / text after the sky (render_set_3d_view)
} RenderPhoneView;

// The view's projection x view (the lens camera with its target's aspect): picking against it.
mat4 render_phone_view_proj(const RenderPhoneView *v);

void render_phone_view(const RenderPhoneView *v);   // this frame (call between begin / end frame)
TextureId render_phone_screen(int view);            // 0 until the view has been rendered once
TextureId render_phone_scene(int view);             // the clean sRGB image as a texture (UI: the second window), 0 until rendered
TextureId render_phone_panel(int view);             // the panel image (sRGB, rounded alpha), 0 until rendered
// The newest completed CPU copy (RGBA8 sRGB, top row first, stride w * 4). The pointer stays
// valid for one frame. *serial increases with every new copy; returns false if there is none.
bool render_phone_readback(int view, const u8 **rgba, int *w, int *h, u32 *serial);
float render_phone_gpu_ms(int view);                // scene + post GPU time (last completed frame)

// Renderer internals (render.c).
struct FrameData;
void phone_view_build(struct FrameData *fd);        // build_frame_graph: after the viewmodel / PiP
void phone_view_frame_begin(int frame_slot);        // after the slot's fence: harvest readbacks
void phone_view_shutdown(void);
bool phone_view_pending(void);                      // a view was requested this frame
int phone_view_scene_res(int view);                 // this frame's scene image (FgRes; FG_NONE: not rendered this frame)

#endif
