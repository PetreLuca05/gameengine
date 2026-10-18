#!/usr/bin/env python3
"""Imports the recorded CC0 takes listed in assets/audio/sample_sources.json (docs/AUDIO_PROCEDURAL.md
"Recorded samples") into assets/sounds/<name>_<n>.ogg, and writes assets/sounds/CREDITS.md.

Per file: mono at 48 kHz, DC + high-pass (the set's "hp", default 30 Hz), the leading silence trimmed
(2 ms before the onset at -40 dB of the peak), the tail cut at -55 dB or "max_s" with a cosine fade,
duplicates dropped. Per set: every take scaled to the same loudness (the loudest 50 ms at -14 dBFS
RMS) so the takes trade places without a level jump, peaks kept under -1 dBFS. Loops ("loop": true)
get an equal-power crossfade of the end into the start.

Run from the repo root:
  python3 tools/audio_import/import_samples.py --fetch DIR    # clone the mirror at its commit into DIR
  python3 tools/audio_import/import_samples.py --src DIR [--only name ...] [--dry]
  python3 tools/audio_import/import_samples.py --check        # every set has takes, all credited (ctest)"""
import argparse, glob, hashlib, json, os, subprocess, sys
import numpy as np

SR = 48000
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANIFEST = os.path.join(ROOT, "assets", "audio", "sample_sources.json")
OUT = os.path.join(ROOT, "assets", "sounds")


def decode(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def highpass(x, fc):
    # Zero-phase 2nd-order Butterworth magnitude in the frequency domain (and the DC removed).
    n = 1 << int(np.ceil(np.log2(len(x) + SR // 10)))
    X = np.fft.rfft(x - x.mean(), n)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    X *= f ** 2 / np.sqrt(f ** 4 + fc ** 4)
    return np.fft.irfft(X, n)[:len(x)]


def envelope(x, ms):
    w = max(1, int(SR * ms / 1000))
    return np.sqrt(np.convolve(x * x, np.ones(w) / w, mode="same"))


def trim(x, max_s, loop):
    if loop:
        return x
    e = envelope(x, 1)
    pk = e.max()
    on = int(np.argmax(e > pk * 10 ** (-40 / 20)))
    start = max(0, on - SR // 500)
    above = np.nonzero(e > pk * 10 ** (-55 / 20))[0]
    end = int(above[-1]) + 1 if len(above) else len(x)
    if max_s:
        end = min(end, start + int(max_s * SR))
    y = x[start:end].copy()
    fi = min(len(y), SR // 1000)
    if start > 0:
        y[:fi] *= np.linspace(0, 1, fi)
    fo = min(len(y) // 4, SR // 50)
    if fo > 1:
        y[-fo:] *= 0.5 + 0.5 * np.cos(np.linspace(0, np.pi, fo))
    return y


def make_loop(x, fade_s=0.25):
    n = min(int(fade_s * SR), len(x) // 4)
    t = np.linspace(0, np.pi / 2, n)
    head, tail = x[:n], x[-n:]
    y = x[n:].copy()
    y[-n:] = tail * np.cos(t) + head * np.sin(t)
    return y


def loudness(x):
    return envelope(x, 50).max() + 1e-12


def encode(x, path):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", str(SR), "-ac", "1", "-i", "-",
                    "-c:a", "libvorbis", "-q:a", "6", path], input=x.astype(np.float32).tobytes(), check=True)


def files_for(spec, sources, src_root):
    out = []
    groups = [(spec["src"], spec["files"])] + list(spec.get("extra", {}).items())
    for src, pats in groups:
        base = os.path.join(src_root, sources[src]["path"])
        for pat in pats:
            hits = sorted(glob.glob(os.path.join(base, pat)))
            if not hits:
                sys.exit("import_samples: no file for %s/%s" % (sources[src]["path"], pat))
            out += [(src, h) for h in hits]
    return out


def check(man):
    credits = open(os.path.join(OUT, "CREDITS.md")).read() if os.path.exists(os.path.join(OUT, "CREDITS.md")) else ""
    bad = 0
    for name, spec in man["samples"].items():
        if spec["src"] not in man["sources"] or any(s not in man["sources"] for s in spec.get("extra", {})):
            print("import_samples: %s names an unknown source" % name)
            bad += 1
        takes = sorted(glob.glob(os.path.join(OUT, name + "_*.ogg")))
        n = len([t for t in takes if os.path.basename(t)[len(name) + 1:-4].isdigit()])
        if n == 0 or n > 16:
            print("import_samples: %s has %d takes on disk" % (name, n))
            bad += 1
        if "`%s_1..%d`" % (name, n) not in credits:
            print("import_samples: CREDITS.md does not list %s_1..%d" % (name, n))
            bad += 1
    print("import_samples: %d sets checked, %d problem(s)" % (len(man["samples"]), bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", help="the mirror's checkout")
    ap.add_argument("--fetch", help="clone the mirror at the manifest's commit into this directory")
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    man = json.load(open(MANIFEST))
    if a.check:
        sys.exit(check(man))
    if a.fetch:
        m = man["mirror"]
        if not os.path.isdir(a.fetch):
            subprocess.run(["git", "clone", "--filter=blob:none", m["repo"], a.fetch], check=True)
        subprocess.run(["git", "-C", a.fetch, "checkout", "-q", m["commit"]], check=True)
        a.src = a.src or a.fetch
    if not a.src:
        ap.error("--src or --fetch")
    sources = man["sources"]
    credits = {}
    for name, spec in man["samples"].items():
        if a.only and name not in a.only:
            continue
        takes, seen = [], set()
        for src, path in files_for(spec, sources, a.src):
            x = highpass(decode(path), spec.get("hp", 30))
            x = trim(x, spec.get("max_s"), spec.get("loop", False))
            if spec.get("loop"):
                x = make_loop(x)
            h = hashlib.sha1(np.round(x * 32767).astype(np.int16).tobytes()).hexdigest()
            if h in seen or len(x) < SR // 200:
                continue
            seen.add(h)
            takes.append((src, os.path.relpath(path, os.path.join(a.src, sources[src]["path"])), x))
        if len(takes) > 16:
            takes = takes[:16]   # SYN_MAX_VARIANTS
        target = 10 ** (-14 / 20)
        out = []
        for src, rel, x in takes:
            y = x * (target / loudness(x))
            pk = np.max(np.abs(y))
            if pk > 10 ** (-1 / 20):
                y *= 10 ** (-1 / 20) / pk
            out.append((src, rel, y))
        print("%-18s %2d takes  %s" % (name, len(out), ", ".join("%.2fs" % (len(y) / SR) for _, _, y in out)))
        credits[name] = [(src, rel) for src, rel, _ in out]
        if a.dry:
            continue
        for old in glob.glob(os.path.join(OUT, name + "_*.ogg")):
            stem = os.path.basename(old)[len(name) + 1:-4]
            if stem.isdigit():
                os.remove(old)
        for k, (_, _, y) in enumerate(out):
            encode(y, os.path.join(OUT, "%s_%d.ogg" % (name, k + 1)))
    if a.dry or a.only:
        return
    lines = ["# Recorded sounds", "",
             "Generated by `tools/audio_import/import_samples.py` from `assets/audio/sample_sources.json`.",
             "Every file below is CC0 1.0 (public domain): no attribution is required, these credits are a courtesy.",
             "Mirror: %s at `%s`." % (man["mirror"]["repo"], man["mirror"]["commit"]), "", "## Sources", ""]
    for key, s in sources.items():
        lines.append("- **%s**: %s. Author: %s. %s (`%s`)." % (key, s["title"], s["author"], s["license"], s["path"]))
    lines += ["", "## Takes", ""]
    for name, cs in credits.items():
        lines.append("- `%s_1..%d`: %s" % (name, len(cs), ", ".join("%s `%s`" % (s, r) for s, r in cs)))
    open(os.path.join(OUT, "CREDITS.md"), "w", newline="\n").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
