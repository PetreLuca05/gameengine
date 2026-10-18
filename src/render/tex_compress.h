// Block-compressed textures (docs/TEXTURE_MIPS.md "Block compression", PF-0035): the RGBA8 levels a
// loader built (mip chains, array layers) encoded to BC7 on the job system and cached on disk by
// content, so the GPU keeps 1 byte per texel instead of 4. Every loader keeps its own mip chain (the
// GPU blit chain's filter for world textures, the alpha-weighted / coverage chains of the vgeo
// module, the box chains of the terrain and surface arrays): each level is encoded as it is.
//
//   colour (sRGB albedo + alpha / coverage)          BC7 SRGB
//   data (normal maps, roughness, height, AO, ...)   BC7 UNORM, the rotated modes too (BC_DATA)
//
// r_texture_compress 0 (or perf_fix_disable PF-0035, or a device that cannot sample BC7) keeps
// RGBA8: the A / B reference. World textures switch at once (assets_reload_textures), everything
// else at its next load (the next map). The cache: <writable>/assets/cache/tex/<key>.bct, key = a
// 64-bit hash of the texels, the sizes, the kind and BC_CODEC_VERSION, so only an identical input
// reads a file (tex_cache_clear deletes them; nothing else ever does).
#ifndef DG_TEX_COMPRESS_H
#define DG_TEX_COMPRESS_H

#include "core/base.h"
#include "render/bc_codec.h"

typedef enum TexcKind { TEXC_COLOR, TEXC_DATA } TexcKind;
typedef struct TexcImage { const u8 *rgba; u32 w, h; } TexcImage;

void texc_init(void);                        // cvar, commands, the device check (render_init, after the device)
bool texc_enabled(void);
u32 texc_vk_format(TexcKind k);              // VK_FORMAT_BC7_SRGB_BLOCK / VK_FORMAT_BC7_UNORM_BLOCK
// Encodes n images in order into one malloc'd buffer: image i at offsets[i] (bc_image_bytes of its
// size), *total bytes; from the cache when it holds them. NULL when compression is off. On the main
// thread the block rows run on the jobs; inside a job it encodes serially.
u8 *texc_encode(const TexcImage *im, int n, TexcKind k, size_t *offsets, size_t *total);
// A square texture array's chain as vkr_upload_image takes it (mip-major: mip m at mip_offs[m] holds
// every layer contiguously; size x size halving to 1 x 1) encoded in the same layout: bc_offs[m] is
// mip m's offset. NULL when compression is off.
u8 *texc_encode_array(const u8 *data, const size_t *mip_offs, u32 mips, u32 layers, u32 size, TexcKind k, size_t *bc_offs, size_t *total);

// One 2 x 2 box level of an RGBA8 image (floor sizes, as the mip chain): sRGB colour averaged in
// linear light (the GPU blit filter of an sRGB image), alpha and UNORM data as they are.
u8 *texc_halve(const u8 *src, int w, int h, bool srgb, int *ow, int *oh);
// The whole chain from w x h to 1 x 1 by texc_halve, packed: im[] gets each level (16 at most),
// *levels the count. The first level is a copy of rgba.
u8 *texc_mip_chain(const u8 *rgba, u32 w, u32 h, bool srgb, TexcImage im[16], u32 *levels);

#endif
