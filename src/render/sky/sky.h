// Physically based sky (docs/DAYNIGHT.md, shaders/sky/*): Hillaire atmosphere LUTs
// (transmittance + multiple scattering when the atmosphere changes, sky view + aerial
// perspective every frame), the sky itself (sun / moon / stars / milky way / clouds), the cloud
// shadow map and the time-varying environment (octahedral map + SH9 irradiance, refiltered in
// quarters over 4 frames, eased so it never pops) that the lit shaders use for ambient and
// specular. Driven by RenderEnvironment.atmosphere (game_daynight.c fills it).
//
// The persistent images stay in VK_IMAGE_LAYOUT_GENERAL; the passes synchronise them with
// global memory barriers (they are not frame graph resources).
#ifndef DG_SKY_H
#define DG_SKY_H

#include "render/graph/fg.h"
#include "render/vk_internal.h"

struct PostBind;

bool sky_init(void);
void sky_shutdown(void);
bool sky_active(void);                       // atmosphere on this frame
void sky_build(void);                        // frame graph passes, after post_begin_frame (before the prepass)
int sky_last_pass(void);                     // this frame's last sky pass (-1 none): the passes reading the sky images follow it (fg_after)
// The sky into a colour target where depth = 1 (main view or the scope PiP).
void sky_render_exec(VkCommandBuffer cmd, FgRes color, FgRes depth, mat4 inv_vp, vec3 cam, bool pip);
void sky_bind_aerial(struct PostBind *b, u32 binding);
VkImageView sky_aerial_view(void);
// For the lit shaders (frame set bindings 5..7).
VkImageView sky_env_view(void);
VkImageView sky_cloud_shadow_view(void);
VkBuffer sky_sh_buffer(void);
VkDeviceSize sky_sh_size(void);
int sky_env_mips(void);
// Diagnostics / tests: luminance of the SH DC term in use and of the latest refilter (host
// readback, a frame or two late), the refilter cycle count.
float sky_sh_luminance(float *latest);
u32 sky_refilter_count(void);
vec3 sky_irradiance_cpu(vec3 dir);           // the SH in use (host copy, a frame late)
void sky_cloud_shadow_region(vec2 *origin, float *size);

#endif
