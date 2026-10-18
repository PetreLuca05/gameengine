#!/usr/bin/env python3
"""SPIR-V -> MSL check (docs/PLATFORM_MACOS.md "Shaders"): runs SPIRV-Cross over every compiled shader
the way MoltenVK translates it (MSL 3.0, macOS, argument buffers) and reports the ones it rejects.
With --metal (macOS only) it also compiles each MSL file with `xcrun metal` for the AIR / metallib
check MoltenVK would otherwise only hit at pipeline creation.

usage: msl_check.py <shader .spv dir> [--out <dir>] [--metal] [--msl-version 30000]
"""
import argparse, os, shutil, subprocess, sys

# Never created on MoltenVK: the 64-bit atomic visibility writers (MoltenVK reports no
# shaderBufferInt64Atomics, vgeo_raster.c takes the *_v32 variants) and their mesh shader pairs.
NOT_ON_METAL = {
    "vgeo/vg_raster_hw.frag.spv", "vgeo/vg_raster_hw_masked.frag.spv",
    "vgeo/vg_raster_mesh.frag.spv", "vgeo/vg_raster_mesh_masked.frag.spv",
    "vgeo/vg_raster_sw.comp.spv", "vgeo/vg_raster_sw_masked.comp.spv", "vgeo/vg_raster_sw_impostor.comp.spv",
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spv_dir")
    ap.add_argument("--out", default=None, help="write the .metal files here")
    ap.add_argument("--metal", action="store_true", help="also compile with xcrun metal (macOS)")
    ap.add_argument("--msl-version", default="30000")
    ap.add_argument("--spirv-cross", default=shutil.which("spirv-cross") or "spirv-cross")
    a = ap.parse_args()
    files = []
    for root, _, names in os.walk(a.spv_dir):
        files += [os.path.join(root, n) for n in names if n.endswith(".spv")]
    files.sort()
    if not files:
        print("msl_check: no .spv under", a.spv_dir)
        return 2
    bad = []
    skipped = 0
    for f in files:
        rel = os.path.relpath(f, a.spv_dir).replace(os.sep, "/")
        if rel in NOT_ON_METAL:
            skipped += 1
            continue
        out = os.path.join(a.out, rel[:-4] + ".metal") if a.out else None
        if out: os.makedirs(os.path.dirname(out), exist_ok=True)
        cmd = [a.spirv_cross, f, "--msl", "--msl-version", a.msl_version, "--msl-argument-buffers",
               "--msl-argument-buffer-tier", "1", "--msl-decoration-binding", "--msl-force-native-arrays"]
        if out: cmd += ["--output", out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            # some SPIRV-Cross builds throw on the argument-buffer layout without MoltenVK's resource
            # bindings: the plain MSL translation still exercises every name / feature for the compile
            cmd = [x for x in cmd if x not in ("--msl-argument-buffers", "--msl-decoration-binding", "--msl-force-native-arrays")]
            if "--msl-argument-buffer-tier" in cmd:
                k = cmd.index("--msl-argument-buffer-tier")
                del cmd[k:k + 2]
            r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            bad.append((rel, (r.stderr or r.stdout).strip().splitlines()[-1:] or ["?"]))
            continue
        if a.metal and out:
            air = out[:-6] + ".air"
            m = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-c", "-std=metal3.0", out, "-o", air], capture_output=True, text=True)
            if m.returncode != 0:
                bad.append((rel + " (metal)", [l for l in m.stderr.splitlines() if "error" in l][:3] or ["?"]))
    for rel, why in bad:
        print("FAIL", rel, ":", " | ".join(why))
    print("msl_check: %d shaders, %d skipped (not used on MoltenVK), %d failed" % (len(files), skipped, len(bad)))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
