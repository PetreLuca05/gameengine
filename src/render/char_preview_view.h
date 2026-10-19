// Inventory character preview view (docs/CHARACTERS.md "Character preview"): one offscreen camera
// that draws only the skinned draws with SkinnedDraw.view = RENDER_VIEW_CHAR_PREVIEW and the static
// draws captured for it (the held item, the worn gear) into its own persistent RGBA8 target:
//   * lit by a fixed studio rig (key + fill directional, two rim point lights, hemispheric ambient)
//     in place of the world's sun / moon / sky / shadows / fog / weather / interiors / screen-space
//     inputs: the same at noon, at midnight and in the rain;
//   * rendered at `supersample` x the size into a transient HDR + depth target, then resolved by
//     post/char_preview.comp: fixed exposure, the curve, a box filter with the depth's coverage as
//     alpha -> straight alpha, sRGB-encoded colour (the UI's convention: shaders/ui.frag blends
//     src alpha in display space, like the ui3d item views), transparent background, the empty
//     pixels next to the silhouette carrying its colour (no dark halo when sampled bilinearly).
// The UI pass of the same frame samples it. Nothing is recorded on frames without a request.
#ifndef DG_CHAR_PREVIEW_VIEW_H
#define DG_CHAR_PREVIEW_VIEW_H

#include "render/render.h"

#define CHAR_PREVIEW_MAX_SIZE 4096
#define CHAR_PREVIEW_MAX_SS 4

typedef struct RenderCharPreview {
    int width, height;        // the target (re-created when it changes)
    int supersample;          // 1 .. CHAR_PREVIEW_MAX_SS samples per side and output pixel
    vec3 cam_pos;
    mat4 view, proj;          // the camera (mat4_look_at / mat4_perspective, as the phone views)
    // Light rig, world space: directions point towards the light; colours are linear radiance
    // (colour x intensity).
    vec3 key_dir, key_color;
    vec3 fill_dir, fill_color;
    vec3 rim_pos[2], rim_color[2];
    float rim_radius;         // the rim lights' range (m)
    vec3 sky_ambient, ground_ambient;   // hemispheric ambient (up / down), also the reflections
    float exposure;           // linear multiplier before the curve
    int tonemap;              // 0 Khronos PBR Neutral, 1 ACES (fitted)
    float saturation;         // 1 = as lit
} RenderCharPreview;

void render_char_preview(const RenderCharPreview *v);   // this frame (between begin / end frame)
TextureId render_char_preview_texture(void);             // the target (0 before the first request)
// render_draw calls between begin / end go to the preview's own static draw list (drawn only by
// the preview pass); render_camera_info answers with the preview camera meanwhile (item mesh LODs).
void render_char_preview_capture_begin(vec3 cam_pos, float fov_deg);
void render_char_preview_capture_end(void);

// Renderer internals (render.c).
struct FrameData;
void char_preview_view_build(struct FrameData *fd);   // build_frame_graph: after the phone views
void char_preview_final_reads(int pass);              // the final (UI) pass samples the target
bool char_preview_pending(void);                      // a preview was requested this frame
void char_preview_view_shutdown(void);
int render_char_preview_pass(const RenderCharPreview *v, int color, int depth, u32 w, u32 h, struct FrameData *fd);

#endif
