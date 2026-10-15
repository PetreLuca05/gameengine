"""Builds the runtime texture atlases of the character skins (assets/human/skins.json):
each skin's material diffuse maps (extracted from the FBX by build/bin/fbx_textures) downscaled to
TILE x TILE and placed side by side -> assets/human/textures/<atlas>.png, registered in
data/assets.json under the atlas name. The game remaps each vertex's UV into its material's tile.

  python tools/gen_skin_atlas.py [--tile 1024]
"""
import json
import os
import sys

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
TILE = int(sys.argv[sys.argv.index('--tile') + 1]) if '--tile' in sys.argv else 1024

skins = json.load(open(os.path.join(ROOT, 'assets', 'human', 'skins.json')))
manifest_path = os.path.join(ROOT, 'data', 'assets.json')
manifest = json.load(open(manifest_path))
names = {t['name'] for t in manifest['textures']}
for s in skins['skins']:
    tiles = s.get('tiles')
    if not tiles:
        continue
    atlas = Image.new('RGB', (TILE * len(tiles), TILE))
    for i, t in enumerate(tiles):
        im = Image.open(os.path.join(ROOT, t['texture'])).convert('RGB').resize((TILE, TILE), Image.LANCZOS)
        atlas.paste(im, (i * TILE, 0))
    rel = 'assets/human/textures/%s.png' % s['atlas']
    atlas.save(os.path.join(ROOT, rel), optimize=True)
    if s['atlas'] not in names:
        manifest['textures'].append({'name': s['atlas'], 'path': rel})
        names.add(s['atlas'])
    print('%s: %d tile(s) -> %s (%d KB)' % (s['id'], len(tiles), rel, os.path.getsize(os.path.join(ROOT, rel)) // 1024))
with open(manifest_path, 'w', newline='\n') as f:
    json.dump(manifest, f, indent=2)
    f.write('\n')
