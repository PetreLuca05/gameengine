"""Image diff for fidelity-neutral proofs (docs/PERF_AUDIT.md "Verification"). Pure Python (zlib only):
8-bit RGB / RGBA PNGs as written by the engine's screenshot (stb_image_write).

Metrics per pair:
  max_diff      largest channel difference (0..255)
  over          fraction of pixels whose largest channel difference exceeds `tol` (TAA / dither noise)
  psnr          dB over RGB
  ssim, ssim_min  luma SSIM over 8x8 blocks (mean, worst block): the perceptual metric
A pair passes when over <= max_over and ssim >= min_ssim; perf_verify also accepts a B-vs-A difference
that stays inside the A-vs-A' envelope (the same build run twice: animation timing, TAA history)."""
import math
import struct
import zlib

TOL = 4            # per channel, 0..255: TAA / dithering noise on a converged frame
MAX_OVER = 1e-4    # fraction of pixels allowed above TOL
MIN_SSIM = 0.999


class Image:
    def __init__(self, w, h, px):
        self.w, self.h = w, h
        self.px = px     # bytearray RGB, row major


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat, w = 8, [], None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError("%s: only 8-bit RGB / RGBA non-interlaced PNGs" % path)
            bpp = 3 if ctype == 2 else 4
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    i = 0
    for y in range(h):
        ft = raw[i]
        row = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if ft == 1:
            for x in range(bpp, stride):
                row[x] = (row[x] + row[x - bpp]) & 255
        elif ft == 2:
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 255
        elif ft == 3:
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + ((left + prev[x]) >> 1)) & 255
        elif ft == 4:
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                ul = prev[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + _paeth(left, prev[x], ul)) & 255
        if bpp == 3:
            out[y * w * 3:(y + 1) * w * 3] = row
        else:
            o = y * w * 3
            out[o:o + w * 3:3] = row[0::4]
            out[o + 1:o + w * 3:3] = row[1::4]
            out[o + 2:o + w * 3:3] = row[2::4]
        prev = row
    return Image(w, h, out)


def write_png(path, img):
    """RGB, filter 0 (tests and diff heat maps)."""
    raw = bytearray()
    for y in range(img.h):
        raw.append(0)
        raw += img.px[y * img.w * 3:(y + 1) * img.w * 3]

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def _luma(img):
    p = img.px
    return [(299 * p[i] + 587 * p[i + 1] + 114 * p[i + 2]) / 1000.0 for i in range(0, len(p), 3)]


def compare(a, b, tol=TOL, heat_path=None):
    if a.w != b.w or a.h != b.h:
        return {"same_size": False, "pass": False}
    pa, pb = a.px, b.px
    n = a.w * a.h
    over = 0
    maxd = 0
    se = 0
    heat = bytearray(n * 3) if heat_path else None
    for i in range(n):
        o = i * 3
        d0, d1, d2 = abs(pa[o] - pb[o]), abs(pa[o + 1] - pb[o + 1]), abs(pa[o + 2] - pb[o + 2])
        m = d0 if d0 > d1 else d1
        if d2 > m:
            m = d2
        if m:
            se += d0 * d0 + d1 * d1 + d2 * d2
            if m > maxd:
                maxd = m
            if m > tol:
                over += 1
            if heat is not None:
                v = min(255, m * 8)
                heat[o], heat[o + 1], heat[o + 2] = v, 0, 255 - v if m > tol else 0
    mse = se / (3.0 * n)
    psnr = 99.0 if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    ssim, ssim_min = block_ssim(a, b) if maxd else (1.0, 1.0)
    if heat is not None:
        write_png(heat_path, Image(a.w, a.h, heat))
    r = {"same_size": True, "max_diff": maxd, "over": over / float(n), "psnr": round(psnr, 2), "ssim": round(ssim, 6), "ssim_min": round(ssim_min, 6)}
    r["pass"] = r["over"] <= MAX_OVER and r["ssim"] >= MIN_SSIM
    return r


def block_ssim(a, b, k=8):
    la, lb = _luma(a), _luma(b)
    w, h = a.w, a.h
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    total, count, worst = 0.0, 0, 1.0
    for by in range(0, h - k + 1, k):
        for bx in range(0, w - k + 1, k):
            sa = sb = saa = sbb = sab = 0.0
            for y in range(by, by + k):
                row = y * w
                for x in range(bx, bx + k):
                    va, vb = la[row + x], lb[row + x]
                    sa += va
                    sb += vb
                    saa += va * va
                    sbb += vb * vb
                    sab += va * vb
            m = float(k * k)
            ma, mb = sa / m, sb / m
            va_, vb_ = saa / m - ma * ma, sbb / m - mb * mb
            cov = sab / m - ma * mb
            s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va_ + vb_ + c2))
            total += s
            count += 1
            if s < worst:
                worst = s
    return (total / count if count else 1.0), worst


def within_envelope(ab, aa, slack=1.5):
    """B vs A no further apart than A vs A' (same build twice), with slack: the fraction of differing
    pixels and the mean SSIM within 1.5 x the A / A' figures. The largest single difference is a noisy
    statistic (one twinkling star): it only has to stay within 2 x A / A' + 4."""
    if not ab.get("same_size") or not aa.get("same_size"):
        return False
    return (ab["over"] <= max(MAX_OVER, aa["over"] * slack + 1e-5) and ab["ssim"] >= min(MIN_SSIM, 1.0 - (1.0 - aa["ssim"]) * slack - 1e-5)
            and ab["max_diff"] <= max(TOL, aa["max_diff"] * 2 + 4))
