"""Generate the 16 x 16 blue-noise dither of the LOD cross-fades (shaders/lod_fade.glsl, docs/LOD.md).

Void-and-cluster (Ulichney 1993) on a torus, Gaussian sigma 1.5: a rank per texel, printed as the
GLSL constant array (ranks 0..255, the shader maps them to (rank + 0.5) / 256). Deterministic:
    python tools/gen_lod_bluenoise.py > /tmp/bn.txt
"""
import math

N = 16
SIGMA = 1.5


def kernel():
    k = [[0.0] * N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            dx = min(x, N - x)
            dy = min(y, N - y)
            k[y][x] = math.exp(-(dx * dx + dy * dy) / (2 * SIGMA * SIGMA))
    return k


K = kernel()


def energy(bits):
    e = [[0.0] * N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            if not bits[y][x]:
                continue
            for v in range(N):
                for u in range(N):
                    e[v][u] += K[(v - y) % N][(u - x) % N]
    return e


def extreme(bits, e, want, pick_max):
    best, at = None, None
    for y in range(N):
        for x in range(N):
            if bits[y][x] != want:
                continue
            v = e[y][x]
            if best is None or (v > best if pick_max else v < best):
                best, at = v, (x, y)
    return at


def add(e, x, y, s):
    for v in range(N):
        for u in range(N):
            e[v][u] += s * K[(v - y) % N][(u - x) % N]


def main():
    # initial binary pattern: 10 % of the texels from a fixed LCG, then relaxed
    seed = 12345
    bits = [[False] * N for _ in range(N)]
    count = 0
    while count < N * N // 10:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        x, y = seed % N, (seed >> 8) % N
        if not bits[y][x]:
            bits[y][x] = True
            count += 1
    e = energy(bits)
    while True:  # move the tightest cluster into the largest void until stable
        cx, cy = extreme(bits, e, True, True)
        bits[cy][cx] = False
        add(e, cx, cy, -1)
        vx, vy = extreme(bits, e, False, False)
        if (vx, vy) == (cx, cy):
            bits[cy][cx] = True
            add(e, cx, cy, 1)
            break
        bits[vy][vx] = True
        add(e, vx, vy, 1)
    rank = [[0] * N for _ in range(N)]
    proto = [row[:] for row in bits]
    ones = sum(sum(1 for b in row if b) for row in bits)
    # phase 1: ranks below the prototype (remove tightest clusters)
    b = [row[:] for row in proto]
    eb = energy(b)
    for r in range(ones - 1, -1, -1):
        x, y = extreme(b, eb, True, True)
        b[y][x] = False
        add(eb, x, y, -1)
        rank[y][x] = r
    # phase 2: ranks above (fill the largest voids)
    b = [row[:] for row in proto]
    eb = energy(b)
    for r in range(ones, N * N):
        x, y = extreme(b, eb, False, False)
        b[y][x] = True
        add(eb, x, y, 1)
        rank[y][x] = r
    flat = [rank[y][x] for y in range(N) for x in range(N)]
    assert sorted(flat) == list(range(N * N))
    print("const uint LOD_BN[256] = uint[](")
    for y in range(N):
        row = ", ".join("%3du" % rank[y][x] for x in range(N))
        print("    " + row + ("," if y < N - 1 else ""))
    print(");")


if __name__ == "__main__":
    main()
