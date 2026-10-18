#!/usr/bin/env python3
"""Objective measures of a rendered sound (docs/AUDIO_PROCEDURAL.md "Listening"): what the ear
judges, as numbers, so tuning can be checked against physics without listening.

  peak / RMS (dBFS), crest factor, onset (first sample above -40 dB of the peak), 10-90 % rise time,
  the duration above -20 / -40 dB of the peak (envelope, 5 ms windows), per octave band (125 Hz ..
  8 kHz) the energy share and the decay time T20 x 3 from the Schroeder integral, spectral centroid,
  inter-ear level difference.

Run: python3 tools/audio_render/analyze.py file.wav [more.wav ...] [--json]"""
import json, sys, wave
import numpy as np

BANDS = [125, 250, 500, 1000, 2000, 4000, 8000]


def load(path):
    with wave.open(path) as w:
        sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64) / 32768.0
    x = x.reshape(-1, ch)
    return sr, x


def band_filter(x, sr, fc):
    # Causal octave band: two cascaded RBJ band-passes (Q = sqrt(2) for an octave), so decays keep
    # their real time course (an FFT mask would smear energy over the whole file).
    w0 = 2 * np.pi * fc / sr
    al = np.sin(w0) / (2 * np.sqrt(2))
    b = np.array([al, 0.0, -al]) / (1 + al)
    a = np.array([1.0, -2 * np.cos(w0) / (1 + al), (1 - al) / (1 + al)])
    y = x
    for _ in range(2):
        out = np.zeros_like(y)
        z1 = z2 = 0.0
        b0, b1, b2, a1, a2 = b[0], b[1], b[2], a[1], a[2]
        for i, v in enumerate(y):
            o = b0 * v + z1
            z1 = b1 * v - a1 * o + z2
            z2 = b2 * v - a2 * o
            out[i] = o
        y = out
    return y


def t20(e, sr):
    # Schroeder backward integral; T20 (-5 .. -25 dB) extrapolated to 60 dB.
    s = np.cumsum(e[::-1])[::-1]
    if s[0] <= 0:
        return None
    db = 10 * np.log10(np.maximum(s / s[0], 1e-12))
    i5 = np.argmax(db <= -5)
    i25 = np.argmax(db <= -25)
    if i25 <= i5:
        return None
    return 3.0 * (i25 - i5) / sr


def analyze(path):
    sr, x = load(path)
    mono = x.mean(axis=1)
    peak = np.max(np.abs(mono)) + 1e-12
    rms = np.sqrt(np.mean(mono ** 2)) + 1e-12
    a = np.abs(mono)
    onset = int(np.argmax(a > peak * 0.01))
    ipk = int(np.argmax(a))
    # rise 10-90 % of the envelope peak (1 ms windows)
    win = max(1, sr // 1000)
    env = np.sqrt(np.convolve(mono ** 2, np.ones(win) / win, mode="same"))
    ep = env.max() + 1e-12
    r10 = int(np.argmax(env > ep * 0.1))
    r90 = int(np.argmax(env > ep * 0.9))
    w5 = max(1, sr // 200)
    env5 = np.sqrt(np.convolve(mono ** 2, np.ones(w5) / w5, mode="same"))
    e5p = env5.max() + 1e-12
    above20 = np.count_nonzero(env5 > e5p * 0.1) / sr
    above40 = np.count_nonzero(env5 > e5p * 0.01) / sr
    spec = np.abs(np.fft.rfft(mono)) ** 2
    f = np.fft.rfftfreq(len(mono), 1.0 / sr)
    centroid = float(np.sum(f * spec) / (np.sum(spec) + 1e-20))
    bands = {}
    tot = 0.0
    for fc in BANDS:
        y = band_filter(mono[onset:], sr, fc)
        e = y ** 2
        bands[fc] = {"energy": float(e.sum()), "t60": t20(e, sr)}
        tot += e.sum()
    for fc in BANDS:
        bands[fc]["share_db"] = 10 * np.log10(bands[fc]["energy"] / (tot + 1e-20) + 1e-20)
    # Direct (onset .. +5 ms) against everything from +50 ms on (reflections, reverb, echoes).
    e = mono ** 2
    d = e[onset:onset + sr // 200].sum() + 1e-20
    late = e[onset + sr // 20:].sum() + 1e-20
    dr = 10 * np.log10(d / late)
    ild = 10 * np.log10((np.sum(x[:, 1] ** 2) + 1e-20) / (np.sum(x[:, 0] ** 2) + 1e-20)) if x.shape[1] > 1 else 0.0
    return {
        "file": path, "dur": len(mono) / sr, "peak_dbfs": 20 * np.log10(peak), "rms_dbfs": 20 * np.log10(rms), "crest_db": 20 * np.log10(peak / rms),
        "onset_ms": onset * 1000.0 / sr, "peak_ms": ipk * 1000.0 / sr, "rise_ms": max(0, r90 - r10) * 1000.0 / sr,
        "above_m20db_s": above20, "above_m40db_s": above40, "centroid_hz": centroid, "ild_db": ild, "dr_db": dr,
        "bands": {str(k): {"share_db": round(v["share_db"], 1), "t60": None if v["t60"] is None else round(v["t60"], 3)} for k, v in bands.items()},
    }


def main(args):
    as_json = "--json" in args
    res = [analyze(p) for p in args if not p.startswith("--")]
    if as_json:
        print(json.dumps(res, indent=1))
        return
    for r in res:
        print("%s: %.2f s, peak %.1f dBFS, rms %.1f, crest %.1f dB, onset %.1f ms, rise %.2f ms, > -20 dB %.3f s, > -40 dB %.3f s, centroid %.0f Hz, ILD %+.1f dB, D/R %+.1f dB" % (
            r["file"].split("/")[-1], r["dur"], r["peak_dbfs"], r["rms_dbfs"], r["crest_db"], r["onset_ms"], r["rise_ms"], r["above_m20db_s"], r["above_m40db_s"], r["centroid_hz"], r["ild_db"], r["dr_db"]))
        print("   band  " + "  ".join("%6s" % b for b in r["bands"]))
        print("   share " + "  ".join("%6.1f" % v["share_db"] for v in r["bands"].values()))
        print("   T60   " + "  ".join("%6s" % ("-" if v["t60"] is None else "%.2f" % v["t60"]) for v in r["bands"].values()))


if __name__ == "__main__":
    main(sys.argv[1:])
