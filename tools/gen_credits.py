#!/usr/bin/env python3
"""Credits for the main menu (docs/MENU_LOBBY.md "Credits"): assets/ui/credits.txt.

    python tools/gen_credits.py

The team section is kept from tools/credits_team.txt; the licences come from third_party/: every
library listed in third_party/VERSIONS.txt plus the FetchContent dependencies, each with its
licence (read from the licence file / header in third_party/ when present). Re-run after adding a
library. Format: "# " title, "## " section, plain lines (the menu wraps them).
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, "third_party")
OUT = os.path.join(ROOT, "assets", "ui", "credits.txt")

# Libraries without a licence file in third_party/ (fetched at configure time, or header-only with
# the licence at the end of the header): name -> (what for, licence).
KNOWN = {
    "SDL3": ("window, input, gamepads", "zlib licence"),
    "Vulkan-Headers": ("Vulkan API headers", "Apache-2.0 / MIT"),
    "Jolt Physics / joltc": ("physics", "MIT"),
    "ENet": ("LAN transport", "MIT"),
    "mbedTLS": ("phone companion TLS", "Apache-2.0"),
    "Steamworks SDK": ("Steam transport, lobbies, friends (optional)", "Valve Steamworks SDK licence"),
    "volk": ("Vulkan loader", "MIT"),
    "Vulkan Memory Allocator": ("GPU memory", "MIT"),
    "cJSON": ("JSON", "MIT"),
    "cgltf": ("glTF loading", "MIT"),
    "miniaudio": ("audio", "public domain / MIT-0"),
    "Nuklear": ("developer editors and the console", "public domain / MIT"),
    "stb (image, image_write, truetype)": ("images, fonts", "public domain / MIT"),
    "FastNoiseLite": ("procedural noise", "MIT"),
    "ufbx": ("FBX import", "MIT / public domain"),
    "meshoptimizer": ("LODs, clusters", "MIT"),
    "AMD FidelityFX FSR": ("upscaling", "MIT"),
    "SMAA": ("anti-aliasing", "MIT"),
    "qrcodegen": ("phone pairing QR codes", "MIT"),
    "quirc": ("QR decoding (tests)", "ISC"),
    "minih264": ("phone video encoder", "CC0"),
    "minimp4": ("MP4 muxer", "CC0"),
    "Barlow typeface (Jeremy Tribby)": ("UI font", "SIL Open Font License 1.1"),
}


# Open data behind the Maps tab globe (tools/globe/build_globe.py, docs/CREDITS.md "Globe data").
DATA = [
    ("Copernicus Sentinel-2 L2A (ESA), via the AWS Open Data registry", ("regional satellite imagery (modified: grey, mosaicked, cloud-masked)", "Copernicus data licence (free, attribution)")),
    ("NASA Blue Marble", ("world imagery", "public domain")),
    ("Natural Earth", ("coasts, borders, lakes, rivers, land cover raster", "public domain")),
    ("AWS Terrain Tiles (Mapzen: SRTM, GMTED2010, ETOPO1, NED and others)", ("height map and ocean depths", "attribution, see the AWS registry")),
    ("geoBoundaries (William & Mary geoLab; US Census TIGER, Romania ADM1 / ADM2)", ("states, counties, judete, communes", "CC BY 4.0")),
]


def licence_hint(path):
    """The first licence line found in a directory's licence file or headers."""
    pats = re.compile(r"(MIT|Apache|BSD|zlib|public domain|Unlicense|ISC|CC0|SIL Open Font)", re.I)
    for root, _, files in os.walk(path):
        for f in sorted(files):
            if not re.match(r"(LICEN[CS]E|COPYING|OFL)", f, re.I) and not f.endswith((".h", ".c")):
                continue
            try:
                with open(os.path.join(root, f), errors="ignore") as fh:
                    for i, line in enumerate(fh):
                        if i > 400:
                            break
                        m = pats.search(line)
                        if m:
                            return m.group(1)
            except OSError:
                pass
    return None


def main():
    lines = ["# Deadground", ""]
    team = os.path.join(ROOT, "tools", "credits_team.txt")
    if os.path.exists(team):
        lines += open(team).read().rstrip().splitlines()
    else:
        lines += ["## Made by", "The Deadground team (edit tools/credits_team.txt)"]
    lines += ["", "## Third-party software", "Deadground uses these libraries; their licences apply to their code.", ""]
    versions = {}
    vpath = os.path.join(TP, "VERSIONS.txt")
    if os.path.exists(vpath):
        for raw in open(vpath, encoding="utf-8-sig"):
            raw = raw.strip()
            if raw:
                versions[raw.split(" ")[0].lower()] = raw
    for name, (what, lic) in sorted(KNOWN.items(), key=lambda kv: kv[0].lower()):
        d = None
        for cand in os.listdir(TP) if os.path.isdir(TP) else []:
            if cand.lower() in name.lower().replace(" ", "").replace("-", "") or name.lower().startswith(cand.lower()):
                d = os.path.join(TP, cand)
                break
        found = licence_hint(d) if d and os.path.isdir(d) else None
        ver = next((v for k, v in versions.items() if k in name.lower()), "")
        lines.append(f"{name}: {what}. Licence: {lic}" + (f" (file: {found})" if found and found.lower() not in lic.lower() else "") + (f". {ver}" if ver else ""))
    lines += ["", "## Map data", "The Maps tab globe (docs/MENU_GLOBE.md) is built from open data:", ""]
    lines += [f"{name}: {what}. Licence: {lic}" for name, (what, lic) in DATA]
    lines += ["", "## Licence texts", "The full licence texts ship next to the libraries in third_party/ and assets/ui/fonts/OFL.txt.", ""]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {os.path.relpath(OUT, ROOT)} ({len(lines)} lines)")


if __name__ == "__main__":
    main()
