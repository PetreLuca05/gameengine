"""Writes sample grading LUTs (Adobe .cube, 17^3, display-encoded sRGB in / out) into
assets/post/luts/. Load one with `set r_lut assets/post/luts/<name>.cube`.

  teal_orange   shadows towards teal, skin / highlights towards orange
  warm_film     warm, lifted blacks, gentle highlight roll-off
  bleach        bleach bypass: desaturated, contrasty
  night         cold blue night grade
"""
import os

N = 17
here = os.path.dirname(os.path.abspath(__file__))
out_dir = os.path.join(here, '..', 'assets', 'post', 'luts')
os.makedirs(out_dir, exist_ok=True)


def clamp(x):
    return 0.0 if x < 0 else (1.0 if x > 1 else x)


def luma(r, g, b):
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def smooth(e0, e1, x):
    t = clamp((x - e0) / (e1 - e0))
    return t * t * (3 - 2 * t)


def teal_orange(r, g, b):
    l = luma(r, g, b)
    sh, hi = 1 - smooth(0.0, 0.55, l), smooth(0.35, 1.0, l)
    r2 = r + 0.06 * hi - 0.05 * sh
    g2 = g + 0.015 * hi + 0.01 * sh
    b2 = b - 0.05 * hi + 0.06 * sh
    m = luma(r2, g2, b2)
    s = 1.12
    return [m + (c - m) * s for c in (r2, g2, b2)]


def warm_film(r, g, b):
    out = []
    for c, k in ((r, 1.04), (g, 1.0), (b, 0.92)):
        c = 0.03 + c * 0.95 * k
        c = c - 0.08 * smooth(0.7, 1.0, c) * (c - 0.7)
        out.append(c)
    return out


def bleach(r, g, b):
    l = luma(r, g, b)
    out = []
    for c in (r, g, b):
        c = l + (c - l) * 0.45
        c = 0.5 + (c - 0.5) * 1.25
        out.append(c)
    return out


def night(r, g, b):
    l = luma(r, g, b)
    return [l * 0.55 + r * 0.25, l * 0.7 + g * 0.25, l * 0.95 + b * 0.3]


for name, fn in (('teal_orange', teal_orange), ('warm_film', warm_film), ('bleach', bleach), ('night', night)):
    lines = ['TITLE "%s"' % name, '# Deadground sample grade (tools/gen_luts.py)', 'LUT_3D_SIZE %d' % N]
    for bi in range(N):
        for gi in range(N):
            for ri in range(N):
                r, g, b = ri / (N - 1), gi / (N - 1), bi / (N - 1)
                o = fn(r, g, b)
                lines.append('%.6f %.6f %.6f' % tuple(clamp(c) for c in o))
    with open(os.path.join(out_dir, name + '.cube'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
print('LUTs written to', os.path.normpath(out_dir))
