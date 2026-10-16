"""Image post-processing for VFX textures (numpy + Pillow, no OpenCV).

All images are float32 arrays in [0, 1], shape (H, W, C). Outputs are premultiplied RGBA unless
noted. Functions:
  alpha_from_luma / chroma_key        background removal (black / white / green screens)
  premultiply / unpremultiply
  fit_cell                             crop to content, pad square, resize to the cell size
  pack_grid                            frames -> flipbook atlas (row-major, top-left first)
  optical_flow                         coarse-to-fine Lucas-Kanade (motion-vector flipbooks)
  encode_flow                          flow (cell pixels) -> RG texture the particle shader decodes
  normal_from_height                   Sobel normal map (tangent space, +Y up, OpenGL convention)
  sixway_from_density                  6-way lightmaps (right/left/top in lm0, bottom/back/front in lm1)
  write_png / write_ktx2               8-bit outputs (KTX2: uncompressed RGBA8 with a full mip chain)
"""
import math
import struct

import numpy as np
from PIL import Image


# ------------------------------------------------------------------ io

def load_rgba(path):
    im = Image.open(path).convert("RGBA")
    return np.asarray(im, dtype=np.float32) / 255.0


def to_u8(a):
    return (np.clip(a, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def write_png(path, a):
    a = np.asarray(a)
    if a.ndim == 2:
        Image.fromarray(to_u8(a), "L").save(path, optimize=True)
    elif a.shape[2] == 3:
        Image.fromarray(to_u8(a), "RGB").save(path, optimize=True)
    else:
        Image.fromarray(to_u8(a), "RGBA").save(path, optimize=True)


def resize(a, w, h):
    """High quality resize of a float image (per channel through Pillow, F mode)."""
    if a.shape[1] == w and a.shape[0] == h:
        return a.copy()
    chans = []
    for c in range(a.shape[2]):
        im = Image.fromarray(a[:, :, c].astype(np.float32), "F")
        chans.append(np.asarray(im.resize((w, h), Image.LANCZOS), dtype=np.float32))
    return np.clip(np.stack(chans, axis=2), 0.0, None)


def box_down(a):
    h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    a = a[:h, :w]
    return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


def blur(a, radius):
    """Separable gaussian blur (radius in pixels)."""
    if radius <= 0:
        return a
    sigma = radius / 2.0
    k = np.arange(-int(math.ceil(radius * 1.5)), int(math.ceil(radius * 1.5)) + 1, dtype=np.float32)
    k = np.exp(-(k * k) / (2 * sigma * sigma))
    k /= k.sum()
    out = a.astype(np.float32)
    pad = len(k) // 2
    for axis in (0, 1):
        p = [(0, 0)] * out.ndim
        p[axis] = (pad, pad)
        src = np.pad(out, p, mode="edge")
        acc = np.zeros_like(out)
        for i, kv in enumerate(k):
            sl = [slice(None)] * out.ndim
            sl[axis] = slice(i, i + out.shape[axis])
            acc += kv * src[tuple(sl)]
        out = acc
    return out


# ------------------------------------------------------------------ alpha

def luma(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def alpha_from_luma(rgb, black=0.03, white=0.6, gamma=1.0, color=None):
    """Emissive / smoke on a black background. Alpha from brightness; colour is either the
    un-darkened source (keep) or a constant albedo (smoke: lighting comes from the 6-way maps)."""
    rgb = rgb[..., :3]
    l = luma(rgb)
    a = np.clip((l - black) / max(white - black, 1e-4), 0.0, 1.0) ** gamma
    if color is None:
        col = rgb / np.maximum(l[..., None], 1e-4) * np.minimum(l[..., None] / max(white, 1e-4), 1.0)
        col = np.clip(col, 0, 1)
    else:
        col = np.broadcast_to(np.asarray(color, dtype=np.float32), rgb.shape).copy()
    return np.concatenate([col * a[..., None], a[..., None]], axis=2)


def chroma_key(rgb, key=(0.0, 1.0, 0.0), tol=0.25, soft=0.15, despill=True):
    """Removes a solid background colour (green screen / white / magenta). Returns premultiplied RGBA."""
    rgb = rgb[..., :3].astype(np.float32)
    key = np.asarray(key, dtype=np.float32)
    d = np.sqrt(((rgb - key) ** 2).sum(axis=2)) / math.sqrt(3.0)
    a = np.clip((d - tol) / max(soft, 1e-4), 0.0, 1.0)
    col = rgb.copy()
    if despill and key[1] > 0.8 and key[0] < 0.2:
        # Green spill: clamp G to the max of R and B.
        col[..., 1] = np.minimum(col[..., 1], np.maximum(col[..., 0], col[..., 2]))
    if despill and key.min() > 0.8:
        # White background: recover the darker foreground (un-mix with white).
        col = np.clip((col - (1 - a[..., None]) * key) / np.maximum(a[..., None], 1e-3), 0, 1)
    return np.concatenate([col * a[..., None], a[..., None]], axis=2)


def fill_enclosed(rgba, color=(0.04, 0.035, 0.03), thresh=0.5):
    """Background regions not connected to the image border (the inside of a bullet hole that the
    keyer made transparent) become opaque dark. Premultiplied in / out."""
    from collections import deque
    a = rgba[..., 3]
    h, w = a.shape
    bg = a < thresh
    seen = np.zeros_like(bg)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            if bg[y, x] and not seen[y, x]:
                seen[y, x] = True
                q.append((y, x))
    for y in range(h):
        for x in (0, w - 1):
            if bg[y, x] and not seen[y, x]:
                seen[y, x] = True
                q.append((y, x))
    while q:
        y, x = q.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and bg[ny, nx] and not seen[ny, nx]:
                seen[ny, nx] = True
                q.append((ny, nx))
    inside = bg & ~seen
    out = rgba.copy()
    k = blur(inside.astype(np.float32), 1.0)[..., None]
    fill = np.concatenate([np.asarray(color, np.float32) * np.ones_like(out[..., :3]), np.ones_like(out[..., 3:4])], axis=2)
    out = out * (1 - k) + fill * k
    return out


def premultiply(rgba):
    out = rgba.copy()
    out[..., :3] *= out[..., 3:4]
    return out


def unpremultiply(rgba):
    out = rgba.copy()
    a = np.maximum(out[..., 3:4], 1e-4)
    out[..., :3] = np.where(out[..., 3:4] > 1e-4, out[..., :3] / a, 0)
    return out


def bleed_edges(rgba, passes=4):
    """Fills colour under transparent texels from their neighbours (mip / bilinear friendliness).
    Operates on straight (unpremultiplied) colour; alpha is untouched."""
    col = unpremultiply(rgba)[..., :3]
    a = rgba[..., 3]
    known = a > 0.02
    for _ in range(passes):
        acc = np.zeros_like(col)
        cnt = np.zeros(a.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                s = np.roll(np.roll(col, dy, 0), dx, 1)
                k = np.roll(np.roll(known, dy, 0), dx, 1)
                acc += s * k[..., None]
                cnt += k
        fill = (~known) & (cnt > 0)
        col[fill] = acc[fill] / cnt[fill][..., None]
        known = known | fill
    out = np.concatenate([col * a[..., None], a[..., None]], axis=2)
    return out


def edge_fade(rgba, margin=0.06):
    """Fades premultiplied content to zero at the cell border (no hard cut-offs on quads)."""
    h, w = rgba.shape[:2]
    y = np.linspace(0, 1, h, dtype=np.float32)[:, None]
    x = np.linspace(0, 1, w, dtype=np.float32)[None, :]
    m = np.minimum(np.minimum(x, 1 - x), np.minimum(y, 1 - y))
    f = np.clip(m / margin, 0, 1)
    f = f * f * (3 - 2 * f)
    return rgba * f[..., None]


def fit_cell(rgba, size, pad=0.06, center=True, bbox=None):
    """Crops to the alpha bounding box (or the given bbox), pads to a square and resizes."""
    a = rgba[..., 3]
    if bbox is None:
        ys, xs = np.nonzero(a > 0.02)
        if len(xs) == 0:
            return np.zeros((size, size, 4), np.float32)
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    else:
        x0, y0, x1, y1 = bbox
    crop = rgba[y0:y1, x0:x1]
    h, w = crop.shape[:2]
    s = int(max(h, w) * (1 + 2 * pad))
    canvas = np.zeros((s, s, 4), np.float32)
    oy, ox = (s - h) // 2, (s - w) // 2
    canvas[oy:oy + h, ox:ox + w] = crop
    return resize(canvas, size, size)


def union_bbox(frames, thresh=0.02):
    x0 = y0 = 10 ** 9
    x1 = y1 = 0
    for f in frames:
        ys, xs = np.nonzero(f[..., 3] > thresh)
        if len(xs):
            x0, x1 = min(x0, xs.min()), max(x1, xs.max() + 1)
            y0, y1 = min(y0, ys.min()), max(y1, ys.max() + 1)
    if x1 <= x0:
        h, w = frames[0].shape[:2]
        return 0, 0, w, h
    return x0, y0, x1, y1


def pack_grid(frames, cols, rows):
    """Row-major flipbook (frame 0 top-left), matching cell_uv() in shaders/vfx_common.glsl."""
    h, w, c = frames[0].shape
    out = np.zeros((rows * h, cols * w, c), np.float32)
    for i, f in enumerate(frames[: cols * rows]):
        r, q = divmod(i, cols)
        out[r * h:(r + 1) * h, q * w:(q + 1) * w] = f
    return out


def unpack_grid(atlas, cols, rows):
    h, w = atlas.shape[0] // rows, atlas.shape[1] // cols
    return [atlas[r * h:(r + 1) * h, q * w:(q + 1) * w] for r in range(rows) for q in range(cols)]


def resample_frames(frames, count):
    """Picks `count` frames evenly over the clip (with blending between source frames)."""
    n = len(frames)
    if n == count:
        return frames
    out = []
    for i in range(count):
        t = i * (n - 1) / max(count - 1, 1)
        a = int(math.floor(t))
        b = min(a + 1, n - 1)
        f = t - a
        out.append(frames[a] * (1 - f) + frames[b] * f)
    return out


# ------------------------------------------------------------------ motion vectors

def _grad(img):
    gx = np.zeros_like(img)
    gy = np.zeros_like(img)
    gx[:, 1:-1] = (img[:, 2:] - img[:, :-2]) * 0.5
    gy[1:-1, :] = (img[2:, :] - img[:-2, :]) * 0.5
    return gx, gy


def _warp(img, fx, fy):
    """Samples img at (x + fx, y + fy) with bilinear filtering (clamped)."""
    h, w = img.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    x = np.clip(xs + fx, 0, w - 1.001)
    y = np.clip(ys + fy, 0, h - 1.001)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    tx, ty = x - x0, y - y0
    a = img[y0, x0] * (1 - tx) + img[y0, x0 + 1] * tx
    b = img[y0 + 1, x0] * (1 - tx) + img[y0 + 1, x0 + 1] * tx
    return a * (1 - ty) + b * ty


def optical_flow(a, b, levels=4, iters=3, window=4):
    """Dense flow a -> b (pixels): for each pixel of a, where its content is in b.
    Coarse-to-fine Lucas-Kanade with a box window, numpy only."""
    pa, pb = [a.astype(np.float32)], [b.astype(np.float32)]
    for _ in range(levels - 1):
        if min(pa[-1].shape) < 16:
            break
        pa.append(box_down(pa[-1][..., None])[..., 0])
        pb.append(box_down(pb[-1][..., None])[..., 0])
    fx = np.zeros_like(pa[-1])
    fy = np.zeros_like(pa[-1])
    for lvl in range(len(pa) - 1, -1, -1):
        A, B = pa[lvl], pb[lvl]
        if fx.shape != A.shape:
            fx = resize(fx[..., None], A.shape[1], A.shape[0])[..., 0] * 2.0
            fy = resize(fy[..., None], A.shape[1], A.shape[0])[..., 0] * 2.0
        for _ in range(iters):
            Bw = _warp(B, fx, fy)
            gx, gy = _grad((A + Bw) * 0.5)
            it = Bw - A
            ixx = blur(gx * gx, window)
            iyy = blur(gy * gy, window)
            ixy = blur(gx * gy, window)
            ixt = blur(gx * it, window)
            iyt = blur(gy * it, window)
            det = ixx * iyy - ixy * ixy
            ok = det > 1e-7
            dx = np.where(ok, (-iyy * ixt + ixy * iyt) / np.where(ok, det, 1), 0)
            dy = np.where(ok, (ixy * ixt - ixx * iyt) / np.where(ok, det, 1), 0)
            fx = fx + np.clip(dx, -2, 2)
            fy = fy + np.clip(dy, -2, 2)
    return fx, fy


def encode_flow(fx, fy, cell_px, strength=0.15):
    """Flow (pixels inside a cell) -> RG texture. The particle shader decodes mv = tex * 2 - 1 and
    offsets uv by mv * cell_uv_size * strength * blend, so mv = flow / cell / strength."""
    mx = np.clip(fx / (cell_px * strength), -1, 1)
    my = np.clip(fy / (cell_px * strength), -1, 1)
    return np.stack([mx * 0.5 + 0.5, my * 0.5 + 0.5, np.full_like(mx, 0.5), np.ones_like(mx)], axis=2)


def flow_flipbook(frames, cell_px, loop=False, strength=0.15):
    """Per-frame flow to the next frame (the last frame: to the first if looping, else zero)."""
    out = []
    lum = [f[..., 3] * 0.6 + luma(f[..., :3]) * 0.4 for f in frames]
    for i in range(len(frames)):
        j = i + 1
        if j >= len(frames):
            if not loop:
                out.append(encode_flow(np.zeros(lum[i].shape, np.float32), np.zeros(lum[i].shape, np.float32), cell_px, strength))
                continue
            j = 0
        fx, fy = optical_flow(lum[i], lum[j])
        # Flow is meaningless where both frames are empty.
        m = blur(np.maximum(lum[i], lum[j]), 2) > 0.01
        out.append(encode_flow(fx * m, fy * m, cell_px, strength))
    return out


# ------------------------------------------------------------------ normals / lighting

def normal_from_height(h, strength=4.0):
    """Tangent-space normal map (RGB, +X right, +Y up in texture space, OpenGL / Y+ convention)."""
    h = h.astype(np.float32)
    gx = np.zeros_like(h)
    gy = np.zeros_like(h)
    gx[:, 1:-1] = h[:, 2:] - h[:, :-2]
    gy[1:-1, :] = h[2:, :] - h[:-2, :]
    nx = -gx * strength
    ny = gy * strength          # image rows grow downwards; +Y up in tangent space
    nz = np.ones_like(h)
    l = np.sqrt(nx * nx + ny * ny + nz * nz)
    return np.stack([nx / l * 0.5 + 0.5, ny / l * 0.5 + 0.5, nz / l * 0.5 + 0.5], axis=2)


def _march(d, dx, dy, steps, k):
    """Transmittance towards a light in screen direction (dx, dy): exp(-k * sum density)."""
    acc = np.zeros_like(d)
    cur = d.copy()
    for s in range(1, steps + 1):
        cur = np.roll(np.roll(d, -dy * s, 0), -dx * s, 1)
        # Do not wrap around: zero what rolled in from the other side.
        if dy > 0:
            cur[-dy * s:, :] = 0
        elif dy < 0:
            cur[:-dy * s, :] = 0
        if dx > 0:
            cur[:, -dx * s:] = 0
        elif dx < 0:
            cur[:, :-dx * s] = 0
        acc += cur
    return np.exp(-k * acc / steps)


def sixway_from_density(density, steps=24, k=6.0, ambient=0.08):
    """Approximate 6-way lightmaps from a 2D density sprite (alpha):
    right / left / top / bottom: transmittance marched towards that screen edge;
    back (light behind the volume, towards the viewer): exp(-k * density) (thin parts glow);
    front (light from the camera side): 1 - occlusion of the surface, a soft wrap term.
    Returns (lm0 RGBA = right, left, top, alpha), (lm1 RGBA = bottom, back, front, alpha)."""
    d = blur(np.clip(density, 0, 1), 1.5)
    h, w = d.shape
    step = max(1, int(round(max(h, w) / 2 / steps)))
    right = _march(d, step, 0, steps, k)
    left = _march(d, -step, 0, steps, k)
    top = _march(d, 0, -step, steps, k)
    bottom = _march(d, 0, step, steps, k)
    back = np.exp(-k * 0.35 * blur(d, 3))
    front = 0.55 + 0.45 * (1 - blur(d, 4))
    f = lambda x: np.clip(ambient + (1 - ambient) * x, 0, 1)
    a = np.clip(density, 0, 1)
    lm0 = np.stack([f(right), f(left), f(top), a], axis=2)
    lm1 = np.stack([f(bottom), f(back), f(front), a], axis=2)
    return lm0, lm1


# ------------------------------------------------------------------ KTX2

def _mips(rgba_u8):
    levels = [rgba_u8]
    cur = rgba_u8.astype(np.float32)
    while cur.shape[0] > 1 or cur.shape[1] > 1:
        h, w = max(1, cur.shape[0] // 2), max(1, cur.shape[1] // 2)
        if cur.shape[0] >= 2 and cur.shape[1] >= 2:
            cur = box_down(cur)
        else:
            cur = resize(cur, w, h)
        levels.append((np.clip(cur, 0, 255) + 0.5).astype(np.uint8))
    return levels


def write_ktx2(path, rgba, srgb=False):
    """Uncompressed KTX2 (VK_FORMAT_R8G8B8A8_UNORM / _SRGB) with a full mip chain."""
    img = to_u8(rgba if rgba.shape[2] == 4 else np.concatenate([rgba, np.ones(rgba.shape[:2] + (1,), np.float32)], 2))
    levels = _mips(img)
    vk_format = 43 if srgb else 37
    h, w = img.shape[:2]
    # Data Format Descriptor (basic, 4 channels RGBA8).
    samples = b""
    for ch in range(4):
        channel_id = [0, 1, 2, 15][ch]
        samples += struct.pack("<IIII", (ch * 8) | (7 << 16) | (channel_id << 24), 0, 0, 255)
    dfd_block_size = 24 + len(samples)
    transfer = 2 if srgb else 1
    dfd = struct.pack("<I", 4 + dfd_block_size)
    dfd += struct.pack("<IHHBBBBBBBBBBBB", 0, 2, dfd_block_size, 1, 1, transfer, 0, 0, 0, 0, 0, 4, 0, 0, 0)
    dfd += samples
    header_size = 12 + 17 * 4 + 3 * 8 + 24 * len(levels)
    dfd_off = header_size
    data_off = dfd_off + len(dfd)
    data_off = (data_off + 15) & ~15
    level_index = []
    blobs = []
    off = data_off
    # KTX2 stores level 0 last? No: the index lists levels 0..n, data may be laid out smallest first.
    offsets = [0] * len(levels)
    for i in range(len(levels) - 1, -1, -1):
        b = levels[i].tobytes()
        off = (off + 3) & ~3
        offsets[i] = off
        blobs.append((off, b))
        off += len(b)
    for i, lv in enumerate(levels):
        b = lv.tobytes()
        level_index.append(struct.pack("<QQQ", offsets[i], len(b), len(b)))
    ident = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    hdr = struct.pack("<IIIIIIIII", vk_format, 1, w, h, 0, 0, 1, len(levels), 0)
    idx = struct.pack("<IIIIQQ", dfd_off, len(dfd), 0, 0, 0, 0)
    out = bytearray(ident + hdr + idx + b"".join(level_index))
    out += dfd
    for o, b in sorted(blobs):
        if len(out) < o:
            out += b"\0" * (o - len(out))
        out += b
    with open(path, "wb") as f:
        f.write(out)
