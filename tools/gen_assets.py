#!/usr/bin/env python3
"""Generates the placeholder assets in assets/ (no third-party Python packages needed).

  assets/sounds/*.wav      synthesized effects + an ambient loop
  assets/textures/crate.png  plank texture (PNG, via zlib)
  assets/textures/hazard.ktx2  RGBA8 sRGB KTX2 with a full mip chain
  assets/models/barrel.gltf   glTF 2.0 barrel (embedded buffer)

Run from the repo root:  python tools/gen_assets.py
"""
import base64
import math
import os
import random
import struct
import wave
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
RATE = 22050
random.seed(1234)


def path(*p):
    full = os.path.join(ROOT, *p)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    return full


# ----------------------------------------------------------------------------- sounds

def write_wav(name, samples):
    with wave.open(path("sounds", name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 30000)) for s in samples))


def env(t, attack, decay):
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


def synth(duration, fn):
    n = int(duration * RATE)
    return [fn(i / RATE, i) for i in range(n)]


class LowPass:
    def __init__(self, a):
        self.a, self.y = a, 0.0

    def __call__(self, x):
        self.y += self.a * (x - self.y)
        return self.y


def gunshot(length, body_freq, noise_mix, decay, lp=0.35):
    f = LowPass(lp)
    def s(t, i):
        e = env(t, 0.002, decay)
        noise = random.uniform(-1, 1)
        thump = math.sin(2 * math.pi * body_freq * t * (1.0 - t * 2)) * math.exp(-t / (decay * 0.6))
        return (f(noise) * noise_mix + thump * (1 - noise_mix)) * e * 1.4
    return synth(length, s)


def click(freq, length=0.05, level=0.6):
    return synth(length, lambda t, i: math.sin(2 * math.pi * freq * t) * env(t, 0.001, 0.01) * level)


def sweep(f0, f1, length, level=0.5, noise=0.0, decay=0.15):
    phase = [0.0]
    def s(t, i):
        f = f0 + (f1 - f0) * (t / length)
        phase[0] += 2 * math.pi * f / RATE
        return (math.sin(phase[0]) * (1 - noise) + random.uniform(-1, 1) * noise) * env(t, 0.005, decay) * level
    return synth(length, s)


def thud(freq, length, level=0.8, noise=0.3):
    f = LowPass(0.08)
    return synth(length, lambda t, i: (math.sin(2 * math.pi * freq * t) * (1 - noise) + f(random.uniform(-1, 1)) * noise * 3)
                 * env(t, 0.003, length * 0.3) * level)


def concat(*parts, gap=0.0):
    out = []
    for p in parts:
        out += p + [0.0] * int(gap * RATE)
    return out


def creak(length):
    f = LowPass(0.2)
    def s(t, i):
        freq = 180 + 60 * math.sin(t * 9) + 30 * math.sin(t * 23)
        return (math.sin(2 * math.pi * freq * t) * 0.4 + f(random.uniform(-1, 1)) * 0.5) * env(t, 0.05, length * 0.5) * 0.6
    return synth(length, s)


def ambient(length):
    lp1, lp2 = LowPass(0.01), LowPass(0.003)
    def s(t, i):
        wind = lp1(random.uniform(-1, 1)) * 4.0 + lp2(random.uniform(-1, 1)) * 6.0
        hum = math.sin(2 * math.pi * 55 * t) * 0.05 + math.sin(2 * math.pi * 110.5 * t) * 0.02
        fade = min(1.0, t / 0.5, (length - t) / 0.5)  # seamless-ish loop edges
        return (wind * 0.6 + hum) * fade * 0.5
    return synth(length, s)


def gen_sounds():
    write_wav("shot_pistol", gunshot(0.35, 140, 0.75, 0.06))
    write_wav("shot_rifle", gunshot(0.3, 110, 0.8, 0.05, 0.45))
    write_wav("shot_shotgun", gunshot(0.6, 80, 0.85, 0.12, 0.25))
    write_wav("shot_crossbow", concat(click(900, 0.03), sweep(400, 120, 0.2, 0.4, 0.4, 0.06)))
    write_wav("swing", sweep(300, 120, 0.25, 0.35, 0.8, 0.1))
    write_wav("melee_hit", thud(90, 0.25, 0.9, 0.5))
    write_wav("melee_hit_wall", concat(click(1400, 0.02, 0.7), thud(160, 0.15, 0.6, 0.6)))
    write_wav("reload", concat(click(700), click(500), click(1100, 0.06), gap=0.18))
    write_wav("dry_fire", click(1600, 0.04, 0.5))
    write_wav("pickup", concat(sweep(500, 900, 0.08, 0.35), sweep(900, 1300, 0.08, 0.3)))
    write_wav("door_open", concat(click(300, 0.04, 0.7), creak(0.7)))
    write_wav("door_close", concat(creak(0.35), thud(70, 0.25, 0.9, 0.4)))
    write_wav("locked", concat(click(250, 0.05, 0.8), click(220, 0.05, 0.8), gap=0.06))
    write_wav("unlock", concat(click(900, 0.03), click(1300, 0.04), gap=0.08))
    write_wav("lever", concat(sweep(200, 90, 0.3, 0.5, 0.3, 0.2), thud(60, 0.2, 0.8)))
    write_wav("gate", concat(thud(45, 0.4, 0.9, 0.6), creak(1.2)))
    write_wav("click", click(2000, 0.03, 0.5))
    write_wav("crate_open", concat(creak(0.4), thud(110, 0.2, 0.6, 0.6)))
    write_wav("heal", concat(sweep(400, 800, 0.25, 0.3), sweep(600, 1000, 0.25, 0.25)))
    write_wav("hit", click(2600, 0.04, 0.45))
    write_wav("hit_kill", concat(click(2600, 0.04, 0.5), click(1800, 0.06, 0.5), gap=0.02))
    write_wav("hurt", thud(70, 0.2, 0.7, 0.2))
    write_wav("bolt_hit", concat(click(1000, 0.02, 0.6), thud(130, 0.12, 0.6, 0.6)))
    write_wav("ambient", ambient(8.0))
    # recoil / weapons pass
    write_wav("hit_head", concat(click(3400, 0.03, 0.55), click(2900, 0.05, 0.5), gap=0.015))
    write_wav("switch", concat(click(1200, 0.02, 0.45), click(800, 0.03, 0.4), gap=0.04))
    write_wav("bolt_cycle", concat(click(600, 0.04, 0.6), click(420, 0.05, 0.6), click(900, 0.04, 0.55), click(1200, 0.04, 0.5), gap=0.09))
    write_wav("shot_smg", gunshot(0.22, 150, 0.8, 0.035, 0.5))
    write_wav("shot_lmg", gunshot(0.4, 95, 0.8, 0.07, 0.4))
    write_wav("shot_dmr", gunshot(0.5, 100, 0.8, 0.08, 0.35))
    write_wav("shot_sniper", gunshot(0.8, 70, 0.85, 0.14, 0.3))


# ----------------------------------------------------------------------------- textures

def png_bytes(w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def crate_texture(size=256):
    px = bytearray(size * size * 4)
    planks = 4
    for y in range(size):
        for x in range(size):
            plank = y * planks // size
            grain = math.sin(x * 0.09 + plank * 13 + math.sin(y * 0.05 + plank) * 2.0) * 0.5 + 0.5
            base = (150 + plank * 7, 108 + plank * 4, 62)
            v = 0.78 + grain * 0.22 + random.uniform(-0.04, 0.04)
            edge = (y % (size // planks)) < 3 or (y % (size // planks)) > size // planks - 3
            frame = x < 10 or x > size - 10
            if edge:
                v *= 0.45
            if frame:
                v *= 0.7
            nail = any(abs(x - nx) < 3 and abs((y % (size // planks)) - size // planks // 2) < 3 for nx in (16, size - 17))
            c = (60, 60, 64) if nail else tuple(min(255, int(ch * v)) for ch in base)
            i = (y * size + x) * 4
            px[i:i + 4] = bytes((c[0], c[1], c[2], 255))
    with open(path("textures", "crate.png"), "wb") as f:
        f.write(png_bytes(size, size, px))


def hazard_level(size):
    px = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            stripe = ((x + y) * 8 // max(size, 1)) % 2
            c = (230, 180, 30) if stripe else (30, 30, 32)
            px[(y * size + x) * 4:(y * size + x) * 4 + 4] = bytes((c[0], c[1], c[2], 255))
    return px


def downsample(px, size):
    half = size // 2
    out = bytearray(half * half * 4)
    for y in range(half):
        for x in range(half):
            for c in range(4):
                s = sum(px[((y * 2 + dy) * size + (x * 2 + dx)) * 4 + c] for dy in (0, 1) for dx in (0, 1))
                out[(y * half + x) * 4 + c] = s // 4
    return out


def ktx2_texture(size=128):
    """Writes a KTX2 container: VK_FORMAT_R8G8B8A8_SRGB, full mip chain, no supercompression."""
    levels = []
    px, s = hazard_level(size), size
    while True:
        levels.append((s, px))
        if s == 1:
            break
        px, s = downsample(px, s), s // 2
    level_count = len(levels)
    header_size = 12 + 13 * 4 + 2 * 8      # identifier + 13 u32 + 2 u64
    index_size = level_count * 24
    # Basic data format descriptor (Khronos Data Format 1.3): RGBA8, sRGB transfer, 4 samples.
    block = struct.pack("<IHH", 0, 2, 24 + 16 * 4)
    block += struct.pack("<BBBB", 1, 1, 2, 0)          # RGBSDA model, BT709 primaries, sRGB, straight alpha
    block += struct.pack("<4B", 0, 0, 0, 0)            # texel block dimensions (1x1x1x1, stored minus one)
    block += struct.pack("<8B", 4, 0, 0, 0, 0, 0, 0, 0)  # bytes per plane
    for ch in range(4):
        channel = 0x1F if ch == 3 else ch              # alpha is linear even for sRGB formats
        block += struct.pack("<HBB4BII", ch * 8, 7, channel, 0, 0, 0, 0, 0, 255)
    dfd = struct.pack("<I", 4 + len(block)) + block
    dfd_offset = header_size + index_size
    data_offset = (dfd_offset + len(dfd) + 15) & ~15
    # Mip data is stored smallest level first; the index lists level 0 first.
    blobs, offsets = b"", {}
    cursor = data_offset
    for lvl in reversed(range(level_count)):
        s, px = levels[lvl]
        offsets[lvl] = cursor
        blobs += bytes(px)
        cursor += len(px)
    out = bytearray()
    out += b"\xabKTX 20\xbb\r\n\x1a\n"
    out += struct.pack("<IIIIIIIII", 43, 1, size, size, 0, 0, 1, level_count, 0)
    out += struct.pack("<IIII", dfd_offset, len(dfd), 0, 0)
    out += struct.pack("<QQ", 0, 0)
    for lvl in range(level_count):
        n = len(levels[lvl][1])
        out += struct.pack("<QQQ", offsets[lvl], n, n)
    out += dfd
    out += b"\x00" * (data_offset - len(out))
    out += blobs
    with open(path("textures", "hazard.ktx2"), "wb") as f:
        f.write(out)


# ----------------------------------------------------------------------------- glTF

def barrel_gltf(radius=0.3, half_h=0.45, seg=24):
    pos, nrm, uv, idx = [], [], [], []
    def v(p, n, t):
        pos.append(p); nrm.append(n); uv.append(t)
        return len(pos) - 1
    rings = [(-half_h, 1.0), (-half_h * 0.4, 1.06), (half_h * 0.4, 1.06), (half_h, 1.0)]
    for r in range(len(rings) - 1):
        (y0, s0), (y1, s1) = rings[r], rings[r + 1]
        for i in range(seg):
            a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
            n0, n1 = (math.cos(a0), 0, math.sin(a0)), (math.cos(a1), 0, math.sin(a1))
            p00 = (n0[0] * radius * s0, y0, n0[2] * radius * s0)
            p10 = (n1[0] * radius * s0, y0, n1[2] * radius * s0)
            p01 = (n0[0] * radius * s1, y1, n0[2] * radius * s1)
            p11 = (n1[0] * radius * s1, y1, n1[2] * radius * s1)
            a = v(p00, n0, (i / seg, 0)); b = v(p10, n1, ((i + 1) / seg, 0))
            c = v(p11, n1, ((i + 1) / seg, 1)); d = v(p01, n0, (i / seg, 1))
            idx += [a, c, b, a, d, c]
    for sign in (-1, 1):
        y = half_h * sign
        center = v((0, y, 0), (0, sign, 0), (0.5, 0.5))
        for i in range(seg):
            a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
            p0 = v((math.cos(a0) * radius, y, math.sin(a0) * radius), (0, sign, 0), (0, 0))
            p1 = v((math.cos(a1) * radius, y, math.sin(a1) * radius), (0, sign, 0), (1, 0))
            idx += [center, p1, p0] if sign > 0 else [center, p0, p1]
    pbin = b"".join(struct.pack("<3f", *p) for p in pos)
    nbin = b"".join(struct.pack("<3f", *n) for n in nrm)
    tbin = b"".join(struct.pack("<2f", *t) for t in uv)
    ibin = b"".join(struct.pack("<H", i) for i in idx)
    while len(ibin) % 4:
        ibin += b"\x00"
    buf = pbin + nbin + tbin + ibin
    mins = [min(p[k] for p in pos) for k in range(3)]
    maxs = [max(p[k] for p in pos) for k in range(3)]
    import json
    gltf = {
        "asset": {"version": "2.0", "generator": "deadground gen_assets.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "barrel"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": "barrel_paint", "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.32, 0.55, 1.0], "metallicFactor": 0.6, "roughnessFactor": 0.45}}],
        "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(buf).decode()}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pbin), "target": 34962},
            {"buffer": 0, "byteOffset": len(pbin), "byteLength": len(nbin), "target": 34962},
            {"buffer": 0, "byteOffset": len(pbin) + len(nbin), "byteLength": len(tbin), "target": 34962},
            {"buffer": 0, "byteOffset": len(pbin) + len(nbin) + len(tbin), "byteLength": len(idx) * 2, "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uv), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ],
    }
    with open(path("models", "barrel.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)


if __name__ == "__main__":
    gen_sounds()
    crate_texture()
    ktx2_texture()
    barrel_gltf()
    print("assets generated in", os.path.normpath(ROOT))
