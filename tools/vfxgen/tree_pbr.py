"""Tree bark PBR maps (docs/VGEO.md "Tree models", docs/PBR.md): a tangent-space normal map and an ORM
map for every opaque (bark / stump / stem) texture of assets/treefbx, derived locally from the Tree It
images (no API calls, deterministic):

    py tools/vfxgen/tree_pbr.py                 # all opaque tree textures
    py tools/vfxgen/tree_pbr.py --sheet out.png # + a contact sheet (albedo, normal, ORM) to check

Outputs in assets/treepbr/ (vgeo_tree.c loads them for the opaque materials):
  <tex>_n.png    tangent normal, glTF / OpenGL convention (+y = image up), the size the game uses
                 for the albedo (box-halved to <= 512 px, like vgeo_tree_texture_load)
  <tex>_orm.png  half that size: R ambient occlusion, G roughness, B metallic (0)

Sources per texture, in order:
  normal     Tree It's own <tex>_normal.png when it decodes (its y convention checked against the
             albedo's luminance relief and flipped when it is DirectX-style); else derived from the
             albedo luminance (band-passed height, like tools/vfxgen/texgen.py)
  roughness  Tree It's <tex>_roughness.png, else 0.75 + cavity
  AO         horizon AO (surface_pbr.horizon_ao) of the height integrated from the normal map
             (Frankot-Chellappa, wrap-around), relief scaled to ~8 texels
Masked textures (leaf / needle / branch cards: > 2 % of the texels below half alpha, the import's
test) are skipped: cards stay albedo-only.
"""
import argparse
import glob
import io
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from surface_pbr import horizon_ao   # noqa: E402
from texgen import blur_wrap, srgb_to_lin   # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "assets", "treefbx")
OUT = os.path.join(ROOT, "assets", "treepbr")
MAX_SIDE = 512                 # VG_TREE_TEX_MAX
MAP_SUFFIXES = ("_normal", "_roughness", "_transmission", "_translucency")


def open_png(path):
    """PIL image; PNGs PIL rejects (Stump03_normal.png: two iCCP chunks) are retried without their
    ancillary chunks."""
    try:
        im = Image.open(path)
        im.load()
        return im
    except Exception:
        d = open(path, "rb").read()
        if d[:8] != b"\x89PNG\r\n\x1a\n":
            raise
        out, i = [d[:8]], 8
        while i < len(d):
            n = struct.unpack(">I", d[i:i + 4])[0]
            t = d[i + 4:i + 8]
            if t in (b"IHDR", b"PLTE", b"IDAT", b"IEND", b"tRNS"):
                out.append(d[i:i + 12 + n])
            i += 12 + n
            if t == b"IEND":
                break
        im = Image.open(io.BytesIO(b"".join(out)))
        im.load()
        return im


def find_map(stem, suffix):
    """Tree It names maps inconsistently (bark01_normal, Stump03_normal, oak leaf_Normal)."""
    want = (stem + suffix + ".png").lower()
    for f in os.listdir(SRC):
        if f.lower() == want:
            return os.path.join(SRC, f)
    return None


def game_size(w, h):
    while (w > MAX_SIDE or h > MAX_SIDE) and w > 1 and h > 1:
        w, h = w // 2, h // 2
    return w, h


def resize(arr, w, h):
    """float array (h, w[, c]) resampled with a box / Lanczos filter, values kept in range."""
    if arr.ndim == 2:
        im = Image.fromarray(arr.astype(np.float32))
        return np.asarray(im.resize((w, h), Image.BOX if w < arr.shape[1] else Image.LANCZOS)).astype(np.float64)
    return np.stack([resize(arr[..., c], w, h) for c in range(arr.shape[2])], axis=2)


def height_from_lum(lin):
    lum = lin @ np.array([0.2126, 0.7152, 0.0722])
    h = lum - blur_wrap(lum, max(lum.shape) // 20)
    h = blur_wrap(h, 1)
    return h / (h.std() + 1e-6)


def normal_from_height(h, k):
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5 * k
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5 * k
    nz = 1.0 / np.sqrt(dx * dx + dy * dy + 1.0)
    return np.stack([-dx * nz, dy * nz, nz], axis=2)   # +y up (image rows go down)


def integrate(n):
    """Height (texels) from a unit normal field (+y up), Frankot-Chellappa with wrap-around."""
    nz = np.maximum(n[..., 2], 0.2)
    p = -n[..., 0] / nz          # dh / dx
    q = n[..., 1] / nz           # dh / drow (rows go down, +y is up)
    hgt, wid = p.shape
    u = np.fft.fftfreq(wid)[None, :] * 2 * np.pi
    v = np.fft.fftfreq(hgt)[:, None] * 2 * np.pi
    den = u * u + v * v
    den[0, 0] = 1.0
    H = (-1j * u * np.fft.fft2(p) - 1j * v * np.fft.fft2(q)) / den
    H[0, 0] = 0.0
    return np.real(np.fft.ifft2(H))


def corr(a, b):
    a, b = a - a.mean(), b - b.mean()
    return float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-12))


def process(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    im = open_png(path).convert("RGBA")
    rgba = np.asarray(im).astype(np.float64) / 255
    if (rgba[..., 3] < 0.5).mean() > 0.02:
        return None                                     # a cut-out card: albedo only
    w, h = game_size(im.width, im.height)
    alb = resize(rgba[..., :3], w, h)
    lin = srgb_to_lin(np.clip(alb, 0, 1))
    h_lum = height_from_lum(lin)
    note = []
    npath = find_map(stem, "_normal")
    n = None
    if npath:
        try:
            nm = np.asarray(open_png(npath).convert("RGB")).astype(np.float64) / 255
            n = resize(nm, w, h) * 2 - 1
            n /= np.linalg.norm(n, axis=2, keepdims=True) + 1e-6
            # convention: the x slope must agree with the luminance relief (bright = raised) for the
            # check to mean anything; then y up (OpenGL) also agrees, y down (DirectX) disagrees
            gx = (np.roll(h_lum, -1, axis=1) - np.roll(h_lum, 1, axis=1))
            gy = (np.roll(h_lum, -1, axis=0) - np.roll(h_lum, 1, axis=0))
            cx, cy = corr(-n[..., 0], gx), corr(n[..., 1], gy)
            decided = abs(cx) > 0.25 and abs(cy) > 0.25
            if (cx * cy < 0) if decided else True:   # undecided: Tree It's maps are DirectX-style
                n[..., 1] = -n[..., 1]
                note.append(f"normal y flipped (x {cx:+.2f}, y {cy:+.2f})")
            else:
                note.append(f"Tree It normal (x {cx:+.2f}, y {cy:+.2f})")
        except Exception as e:
            note.append(f"normal unreadable ({e.__class__.__name__})")
            n = None
    if n is None:
        n = normal_from_height(h_lum, 1.2)
        note.append("normal from luminance")
    rpath = find_map(stem, "_roughness")
    if rpath:
        rough = resize(np.asarray(open_png(rpath).convert("L")).astype(np.float64) / 255, w // 2, h // 2)
        note.append("Tree It roughness")
    else:
        hs = resize(h_lum, w // 2, h // 2)
        rough = np.clip(0.75 + (0.0 - hs) * 0.08, 0.3, 1.0)
        note.append("roughness from luminance")
    # AO of the relief the normal map describes, at the ORM size (its slopes in ORM texels)
    hgt = integrate(resize(n, w // 2, h // 2))
    hgt *= 8.0 / (np.percentile(hgt, 98) - np.percentile(hgt, 2) + 1e-6)   # ~8 texels (2-3 cm furrows) peak to valley
    ao = horizon_ao(hgt, radii=[1, 2, 3, 4, 6, 8, 12, 16])
    ao = np.clip(ao / max(np.median(ao), 1e-3), 0, 1) ** 1.5
    os.makedirs(OUT, exist_ok=True)
    nimg = np.clip((n * 0.5 + 0.5) * 255 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(nimg).save(os.path.join(OUT, f"{stem}_n.png"), optimize=True)
    orm = np.stack([ao, np.clip(rough, 0, 1), np.zeros_like(ao)], axis=2)
    Image.fromarray(np.clip(orm * 255 + 0.5, 0, 255).astype(np.uint8)).save(os.path.join(OUT, f"{stem}_orm.png"), optimize=True)
    print(f"  {stem}: {w}x{h}, ORM {w // 2}x{h // 2}, AO mean {ao.mean():.2f}, roughness mean {rough.mean():.2f}; " + ", ".join(note))
    return stem, alb, nimg, orm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet", default="")
    a = ap.parse_args()
    rows = []
    for path in sorted(glob.glob(os.path.join(SRC, "*.png"))):
        stem = os.path.splitext(os.path.basename(path))[0].lower()
        if stem.endswith(MAP_SUFFIXES) or "imposter" in stem:
            continue
        r = process(path)
        if r:
            rows.append(r)
    print(f"{len(rows)} opaque tree textures -> {os.path.relpath(OUT, ROOT)}")
    if a.sheet and rows:
        s = 192
        sheet = Image.new("RGB", (s * 4, s * len(rows)))
        for i, (stem, alb, nimg, orm) in enumerate(rows):
            sheet.paste(Image.fromarray((np.clip(alb, 0, 1) * 255).astype(np.uint8)).resize((s, s)), (0, i * s))
            sheet.paste(Image.fromarray(nimg).resize((s, s)), (s, i * s))
            o = (np.clip(orm, 0, 1) * 255).astype(np.uint8)
            sheet.paste(Image.fromarray(o[..., 0]).convert("RGB").resize((s, s)), (2 * s, i * s))
            sheet.paste(Image.fromarray(o[..., 1]).convert("RGB").resize((s, s)), (3 * s, i * s))
        sheet.save(a.sheet)


if __name__ == "__main__":
    main()
