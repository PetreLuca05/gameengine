#!/usr/bin/env python3
"""Blue-noise textures for the stochastic post effects (void-and-cluster, Ulichney 1993).

Writes assets/textures/bluenoise64.png: 64x64 RGBA, every channel an independent blue-noise
rank map (0..255, uniform histogram, toroidal). Shaders rotate / offset it per frame (golden
ratio sequence) for temporal stability.

  python tools/gen_bluenoise.py [size] [seed]
"""
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def gaussian_kernel(n, sigma=1.9):
    x = np.arange(n)
    x = np.minimum(x, n - x)
    g = np.exp(-(x * x) / (2 * sigma * sigma))
    return np.outer(g, g)


def energy(binary, kf):
    return np.real(np.fft.ifft2(np.fft.fft2(binary.astype(np.float64)) * kf))


def void_and_cluster(n, rng):
    kf = np.fft.fft2(gaussian_kernel(n))
    total = n * n
    ones = total // 10
    binary = np.zeros((n, n), bool)
    binary.flat[rng.choice(total, ones, replace=False)] = True
    # 1. Relax the initial pattern: move the tightest cluster into the largest void until stable.
    for _ in range(total):
        e = energy(binary, kf)
        cl = np.argmax(np.where(binary, e, -np.inf))
        binary.flat[cl] = False
        e = energy(binary, kf)
        vo = np.argmin(np.where(binary, np.inf, e))
        if vo == cl:
            binary.flat[cl] = True
            break
        binary.flat[vo] = True
    rank = np.zeros((n, n), np.int64)
    proto = binary.copy()
    # 2. Ranks below the initial count: remove clusters.
    b = proto.copy()
    for r in range(ones - 1, -1, -1):
        e = energy(b, kf)
        cl = np.argmax(np.where(b, e, -np.inf))
        b.flat[cl] = False
        rank.flat[cl] = r
    # 3. Ranks above: fill voids.
    b = proto.copy()
    for r in range(ones, total):
        e = energy(b, kf)
        vo = np.argmin(np.where(b, np.inf, e))
        b.flat[vo] = True
        rank.flat[vo] = r
    return rank


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 64
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    rng = np.random.default_rng(seed)
    chans = []
    for c in range(4):
        r = void_and_cluster(n, rng)
        chans.append((r * 256 // (n * n)).astype(np.uint8))
        print(f"channel {c} done")
    img = np.stack(chans, axis=2)
    out = os.path.join(ROOT, "assets", "textures", f"bluenoise{n}.png")
    Image.fromarray(img, "RGBA").save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
