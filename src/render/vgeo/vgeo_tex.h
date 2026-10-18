// vgeo textures (docs/VGEO.md "Foliage support"): one index space for the textures a VgMaterial names:
//   [0, VG_TEX_GAME)            the renderer's texture ids (the classic renderer uses them directly)
//   [VG_TEX_GAME, VG_TEX_COUNT) textures the vgeo module creates: impostor atlases, masks, tree maps,
//                                the foliage displacement buffer (the classic renderer wraps them as
//                                renderer textures)
// Every entry starts as a 1x1 white texture. vg_tex_set changes an entry (ver / gen bump: users that
// cache an entry re-read it); vg_tex_write_set writes the changed entries into an array binding.
#ifndef DG_VGEO_TEX_H
#define DG_VGEO_TEX_H

#include "vgeo_ctx.h"

#define VG_TEX_GAME 1024
#define VG_TEX_OWN 128
#define VG_TEX_COUNT (VG_TEX_GAME + VG_TEX_OWN)
#define VG_TEX_BINDING 23

typedef struct VgTexEntry { VkImageView view; VkSampler sampler; VkImageLayout layout; } VgTexEntry;
typedef struct VgOwnTex { VkImage image; VmaAllocation alloc; VkImageView view; u32 w, h, mips; bool used; } VgOwnTex;

typedef struct VgTextures {
    const VgContext *ctx;
    VgTexEntry e[VG_TEX_COUNT];  // current contents (view NULL = white)
    u32 ver[VG_TEX_COUNT];       // bumps on every change of the entry
    u32 gen;                     // bumps on every change of any entry
    VgOwnTex own[VG_TEX_OWN];
    VkImage white;
    VmaAllocation white_alloc;
    VkImageView white_view;
    VkSampler clamp, repeat;     // trilinear (+ anisotropic: vg_tex_set_anisotropy), clamp to edge / repeat
    float anisotropy;            // 0 = off
    float mip_bias;              // mipLodBias of both samplers (the renderer's, PF-0029)
} VgTextures;

bool vg_tex_init(VgTextures *t, const VgContext *ctx);
void vg_tex_shutdown(VgTextures *t);
// Anisotropic filtering on clamp / repeat (0: off; the creator enabled samplerAnisotropy): both are
// recreated and the entries holding the old ones switch over. The caller waited for the device (the
// old samplers are destroyed; whoever copied t->clamp / t->repeat elsewhere rewrites them).
void vg_tex_set_anisotropy(VgTextures *t, float anisotropy, float mip_bias);
// Entry `index` samples `view` (NULL: white again) with `sampler` (NULL: t->repeat) in `layout`.
void vg_tex_set(VgTextures *t, u32 index, VkImageView view, VkSampler sampler, VkImageLayout layout);
// An RGBA8 texture of the module (index >= VG_TEX_GAME, 0 when full): `levels` mips packed one after
// the other (w x h, then w/2 x h/2, ...), uploaded now (load time: waits). srgb: albedo; repeat: wrap.
u32 vg_tex_create(VgTextures *t, u32 w, u32 h, u32 levels, const u8 *rgba, bool srgb, bool repeat);
// A module entry for an image created elsewhere (vg_tex_set it); vg_tex_release frees an entry and
// the image vg_tex_create made for it.
u32 vg_tex_alloc(VgTextures *t);
void vg_tex_release(VgTextures *t, u32 index);
// Full mip chain of an RGBA8 image (box filter; colour weighted by alpha so masked texels do not bleed
// dark), returns the level count and writes every level packed into out (vg_tex_mip_bytes).
u32 vg_tex_build_mips(const u8 *rgba, u32 w, u32 h, u8 *out);
size_t vg_tex_mip_bytes(u32 w, u32 h, u32 *levels);

// Writes the entries changed since this set last saw them (cull sets; the caller owns `seen`, one
// version per entry, all ~0 at the start).
void vg_tex_write_set(const VgTextures *t, VkDescriptorSet set, u32 *seen);

#endif
