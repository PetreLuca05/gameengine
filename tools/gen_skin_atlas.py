"""Builds the runtime texture atlases of the character skins (assets/human/skins.json): each skin's
material maps (extracted from the FBX by build/bin/fbx_textures, which also writes
<model>.maps.json: material -> FBX property -> file) downscaled to TILE x TILE and placed side by
side, registered in data/assets.json. The game remaps each vertex's UV into its material's tile.

  python tools/gen_skin_atlas.py [--tile 1024] [--only <skin id>]

Per skin (docs/CHARACTERS.md "PBR maps"):
  <atlas>.png          albedo (sRGB): DiffuseColor
  <atlas>_normal.png   tangent-space normal (linear, OpenGL convention: +y = image up): NormalMap, or
                       Bump when that image is a normal map. DirectX-convention maps (green down,
                       detected by the integrability test below) are flipped; a map of another UDIM
                       tile than the material's diffuse (Ch42_Hair's Bump is the body's map) is not used.
  <atlas>_orm.png      linear: r ambient occlusion (255 without an AO map), g roughness, b metallic.
                       Spec / gloss models (Mixamo nonPBR): roughness = 1 - glossiness (the
                       ShininessExponent map), metallic and the albedo from the specular colour with the
                       KHR_materials_pbrSpecularGlossiness conversion (F0 = 0.04 dielectric -> metallic 0,
                       albedo unchanged).
  <atlas>_emissive.png sRGB, only when a material has an EmissiveColor map.
A tile entry in skins.json may name any map itself ("normal", "gloss", "roughness", "specular",
"metallic", "occlusion", "emissive"), over what <model>.maps.json says.
"""
import json
import os
import re
import sys

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
TILE = int(sys.argv[sys.argv.index('--tile') + 1]) if '--tile' in sys.argv else 1024
ONLY = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
DIELECTRIC = 0.04


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def load(path, mode, size=TILE):
    im = Image.open(path)
    if mode == 'L' and im.mode not in ('L', 'I', 'I;16'):
        im = im.convert('RGB').convert('L')
    im = im.convert(mode)
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    return np.asarray(im).astype(np.float32) / 255.0


def is_normal_map(path):
    """An RGB image whose blue channel is mostly near 1 (tangent space), not a height map."""
    im = Image.open(path)
    if im.mode not in ('RGB', 'RGBA'):
        return False
    a = np.asarray(im.convert('RGB').resize((256, 256), Image.BOX)).astype(np.float32) / 255.0
    return a[..., 2].mean() > 0.85 and abs(a[..., 0].mean() - 0.5) < 0.1 and abs(a[..., 1].mean() - 0.5) < 0.1


def green_down(path):
    """True for a DirectX-convention normal map (green = image down). A normal map is the gradient
    of a height field, so its curl vanishes for the right convention: with p = -dh/dcol and
    q = +dh/drow (OpenGL, image rows go down) dp/drow + dq/dcol = 0, for DirectX dp/drow - dq/dcol = 0.
    Counted over 32 x 32 blocks (seams and non-integrable fabric detail average out)."""
    im = Image.open(path).convert('RGB')
    im = im.resize((min(im.width, 2048), min(im.height, 2048)), Image.BOX)
    a = np.asarray(im).astype(np.float32) / 127.5 - 1.0
    nz = np.maximum(a[..., 2], 0.3)
    p, q = a[..., 0] / nz, a[..., 1] / nz
    pr = p[2:, 1:-1] - p[:-2, 1:-1]
    qc = q[1:-1, 2:] - q[1:-1, :-2]
    egl, edx = (pr + qc) ** 2, (pr - qc) ** 2
    n = egl.shape[0] // 32
    gl = dx = 0
    for i in range(32):
        for j in range(32):
            g = egl[i * n:(i + 1) * n, j * n:(j + 1) * n].mean()
            d = edx[i * n:(i + 1) * n, j * n:(j + 1) * n].mean()
            if g + d < 1e-4:
                continue
            gl += g < 0.8 * d
            dx += d < 0.8 * g
    return dx > gl, gl, dx


def udim(name):
    m = re.search(r'_(1\d\d\d)_', os.path.basename(name))
    return m.group(1) if m else None


def tile_maps(tile, maps_json):
    """The material's map files (absolute paths or None) by role."""
    tex_dir = os.path.dirname(os.path.join(ROOT, tile['texture']))
    mat = maps_json.get('materials', {}).get(tile['material'], {}) if maps_json else {}
    def fbx(prop):
        f = mat.get(prop)
        return os.path.join(tex_dir, f) if f and os.path.exists(os.path.join(tex_dir, f)) else None
    out = {
        'albedo': os.path.join(ROOT, tile['texture']),
        'normal': fbx('NormalMap'),
        'gloss': fbx('ShininessExponent'),
        'specular': fbx('SpecularColor') or fbx('SpecularFactor'),
        'roughness': None, 'metallic': None, 'occlusion': None,
        'emissive': fbx('EmissiveColor'),
    }
    if not out['normal'] and fbx('Bump') and is_normal_map(fbx('Bump')):
        out['normal'] = fbx('Bump')
    for k in ('normal', 'gloss', 'roughness', 'specular', 'metallic', 'occlusion', 'emissive'):
        if k in tile:
            out[k] = os.path.join(ROOT, tile[k])
    # A map of another UDIM tile than the albedo belongs to another material (Ch42_Hair's Bump).
    ua = udim(out['albedo'])
    for k in ('normal', 'gloss', 'specular'):
        if out[k] and ua and udim(out[k]) and udim(out[k]) != ua:
            print('  %s: %s is UDIM %s, the albedo %s: not used' % (tile['material'], os.path.basename(out[k]), udim(out[k]), ua))
            out[k] = None
    return out


def build_tile(tile, maps_json, report):
    m = tile_maps(tile, maps_json)
    alb = load(m['albedo'], 'RGB')
    lin = srgb_to_linear(alb)
    # normal (linear, OpenGL convention)
    if m['normal']:
        flip, gl, dx = green_down(m['normal'])
        n = load(m['normal'], 'RGB') * 2.0 - 1.0
        if flip:
            n[..., 1] = -n[..., 1]
        n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)   # renormalise after the filter
        nrm = (n * 0.5 + 0.5)
        report.append('%s normal %s (%s: blocks GL %d / DX %d)' % (tile['material'], os.path.basename(m['normal']),
                                                                 'DirectX, green flipped' if flip else 'OpenGL', gl, dx))
    else:
        nrm = np.broadcast_to(np.array([0.5, 0.5, 1.0], np.float32), alb.shape).copy()
        report.append('%s normal: none (flat)' % tile['material'])
    # roughness
    if m['roughness']:
        rough = load(m['roughness'], 'L')
    elif m['gloss']:
        rough = 1.0 - load(m['gloss'], 'L')
    else:
        rough = np.full(alb.shape[:2], 0.75, np.float32)   # the character default (characters.c)
    # metallic: explicit, else from the specular colour (spec / gloss -> metal / rough)
    if m['metallic']:
        metal = load(m['metallic'], 'L')
    elif m['specular']:
        spec = srgb_to_linear(load(m['specular'], 'RGB'))
        smax = spec.max(axis=-1)
        one_minus = 1.0 - smax
        def bright(c):
            return np.sqrt(0.299 * c[..., 0] ** 2 + 0.587 * c[..., 1] ** 2 + 0.114 * c[..., 2] ** 2)
        d, s = bright(lin), bright(spec)
        a = DIELECTRIC
        b = d * one_minus / (1.0 - a) + s - 2.0 * a
        c = a - s
        disc = np.maximum(b * b - 4.0 * a * c, 0.0)
        metal = np.where(s < a, 0.0, np.clip((-b + np.sqrt(disc)) / (2.0 * a), 0.0, 1.0)).astype(np.float32)
        # albedo where it turned metallic (elsewhere the diffuse is the albedo: F0 = 0.04)
        mt = metal[..., None]
        from_d = lin * one_minus[..., None] / (1.0 - a) / np.maximum(1.0 - mt, 1e-4)
        from_s = (spec - a * (1.0 - mt)) / np.maximum(mt, 1e-4)
        base = np.clip(from_d + (from_s - from_d) * mt * mt, 0.0, 1.0)
        sel = metal > 0.0
        if sel.any():
            alb = np.where(sel[..., None], linear_to_srgb(base), alb)
        report.append('%s metallic from %s: %.2f %% of texels > 0, max %.2f; roughness mean %.2f' % (
            tile['material'], os.path.basename(m['specular']), 100.0 * sel.mean(), float(metal.max()), float(rough.mean())))
    else:
        metal = np.zeros(alb.shape[:2], np.float32)
    occ = load(m['occlusion'], 'L') if m['occlusion'] else np.ones(alb.shape[:2], np.float32)
    orm = np.stack([occ, rough, metal], axis=-1)
    emi = load(m['emissive'], 'RGB') if m['emissive'] else None
    return alb, nrm, orm, emi


def to_img(a):
    return Image.fromarray((np.clip(a, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8))


def main():
    skins = json.load(open(os.path.join(ROOT, 'assets', 'human', 'skins.json')))
    manifest_path = os.path.join(ROOT, 'data', 'assets.json')
    manifest = json.load(open(manifest_path))
    names = {t['name'] for t in manifest['textures']}

    def register(name, rel, srgb):
        if name in names:
            return
        e = {'name': name, 'path': rel}
        if not srgb:
            e['srgb'] = False
        manifest['textures'].append(e)
        names.add(name)

    for s in skins['skins']:
        tiles = s.get('tiles')
        if not tiles or (ONLY and s['id'] != ONLY):
            continue
        model = os.path.splitext(os.path.basename(s['mesh']))[0]
        mj_path = os.path.join(ROOT, os.path.dirname(tiles[0]['texture']), model + '.maps.json')
        maps_json = json.load(open(mj_path)) if os.path.exists(mj_path) else None
        if not maps_json:
            print('%s: %s missing (run fbx_textures): albedo only' % (s['id'], os.path.relpath(mj_path, ROOT)))
        atl = {k: Image.new('RGB', (TILE * len(tiles), TILE)) for k in ('albedo', 'normal', 'orm', 'emissive')}
        any_emi = False
        report = []
        for i, t in enumerate(tiles):
            alb, nrm, orm, emi = build_tile(t, maps_json, report)
            atl['albedo'].paste(to_img(alb), (i * TILE, 0))
            atl['normal'].paste(to_img(nrm), (i * TILE, 0))
            atl['orm'].paste(to_img(orm), (i * TILE, 0))
            if emi is not None:
                atl['emissive'].paste(to_img(emi), (i * TILE, 0))
                any_emi = True
        out = [('albedo', '', True), ('normal', '_normal', False), ('orm', '_orm', False)]
        if any_emi:
            out.append(('emissive', '_emissive', True))
        for key, suffix, srgb in out:
            if key != 'albedo' and not maps_json and not any(k in t for t in tiles for k in ('normal', 'gloss', 'roughness', 'specular', 'metallic', 'occlusion')):
                continue
            rel = 'assets/human/textures/%s%s.png' % (s['atlas'], suffix)
            atl[key].save(os.path.join(ROOT, rel), optimize=True)
            register(s['atlas'] + suffix, rel, srgb)
            print('%s: %d tile(s) -> %s (%d KB)' % (s['id'], len(tiles), rel, os.path.getsize(os.path.join(ROOT, rel)) // 1024))
        for r in report:
            print('  ' + r)
    with open(manifest_path, 'w', newline='\n') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')


if __name__ == '__main__':
    main()
