// Block compression codecs (docs/TEXTURE_MIPS.md "Block compression", PF-0035): BCn encoders and
// decoders for the texture compressor (render/tex_compress.c).
//
//   BC1  RGB, 8 bytes per 4 x 4 block (4 bpp): two RGB565 endpoints, 2-bit indices (4-colour mode only)
//   BC3  RGBA, 16 bytes (8 bpp): a BC4 block for alpha + a BC1 colour block
//   BC4  one channel (r), 8 bytes: two 8-bit endpoints, 3-bit indices (8- and 6-value modes)
//   BC5  two channels (r, g), 16 bytes: two BC4 blocks
//   BC7  RGBA, 16 bytes (8 bpp). Encoded modes: 6 (one subset, RGBA, 4-bit indices), 5 and 4 (one
//        subset, a rotated channel with its own indices), 1 and 3 (two partitioned RGB subsets) and
//        7 (two RGBA subsets). The decoder reads every mode but the three-subset ones (0, 2).
//
// A block is 4 x 4 texels, RGBA8, row major (64 bytes); decoders write the same layout. Every
// candidate an encoder tries is scored by the squared error of its exact decoded texels (the BC7
// interpolation and endpoint expansion of the format spec), the best one is kept. The BC4 / BC1
// palettes are rounded to the nearest integer (GPUs decode them at a higher precision: within 1).
// Deterministic (the same bytes on every machine and thread count), no allocation, pure
// (tests/test_bc_codec.c checks the error bounds; tools/tests/bc_crosscheck.py checks the
// bitstreams against Pillow's decoders).
#ifndef DG_BC_CODEC_H
#define DG_BC_CODEC_H

#include "core/base.h"

typedef enum BcFormat { BC_NONE, BC_1, BC_3, BC_4, BC_5, BC_7, BC_FORMAT_COUNT } BcFormat;

// Encoder effort (the `q` argument: a level, optionally | BC_DATA):
//   FAST    BC7: mode 6, mode 5 for blocks with alpha; no partitions
//   NORMAL  + the two-subset modes on the two best-ranked partitions
//   HIGH    every rotation / index selection of modes 4 and 5, four partitions, every p-bit pair,
//           more refinement (offline quality, ~10 x the time)
// BC_DATA: linear packed channels (normal xy, roughness, height ...) that do not move together: the
// rotated modes 4 / 5 are tried for every block (they give one channel its own indices).
typedef enum BcQuality { BC_Q_FAST = 0, BC_Q_NORMAL = 1, BC_Q_HIGH = 2, BC_Q_MASK = 3, BC_DATA = 4 } BcQuality;

#define BC_CODEC_VERSION 1   // bump when an encoder's output changes (tex_compress cache key)

u32 bc_block_bytes(BcFormat f);                       // 8 (BC1 / BC4) or 16
size_t bc_image_bytes(BcFormat f, u32 w, u32 h);      // ceil(w / 4) x ceil(h / 4) blocks
const char *bc_format_name(BcFormat f);

void bc_encode_block(BcFormat f, const u8 rgba[64], u8 *out, int q);
void bc_decode_block(BcFormat f, const u8 *in, u8 rgba[64]);

// Block rows [by0, by1) of a w x h RGBA8 image into `blocks` (the whole image's block array, row
// major). Edge blocks repeat the last column / row (texels outside the image are never sampled).
void bc_encode_rows(BcFormat f, const u8 *rgba, u32 w, u32 h, u32 by0, u32 by1, u8 *blocks, int q);
void bc_decode_image(BcFormat f, const u8 *blocks, u32 w, u32 h, u8 *rgba);

// BC7 with one mode only (tests, the cross-check): mode 1 / 3 / 4 / 5 / 6 / 7; part = the partition
// (modes 1, 3, 7) or the rotation (4, 5; mode 4 adds 4 for the index selection bit). Returns the
// squared error of the encoded block.
u32 bc7_encode_block_mode(const u8 rgba[64], u8 out[16], int mode, int part, int q);
int bc7_block_mode(const u8 in[16]);                  // the mode of an encoded block (8: reserved)
int bc7_partition_subset(int part, int texel);        // two-subset partition table (0 / 1)

#endif
