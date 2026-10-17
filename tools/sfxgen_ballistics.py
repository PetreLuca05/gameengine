#!/usr/bin/env python3
"""Ballistic sounds for docs/BALLISTICS.md "Sound" (numpy, no other dependencies).

    python tools/sfxgen_ballistics.py

Writes into assets/sounds:
  bullet_crack_{0,1,2}.wav  supersonic crack: an N-wave (linear pressure drop between two shocks)
                            of 0.2 / 0.45 / 0.63 ms (the length grows with r^1/4 of the miss
                            distance), its ground reflection and a short airy tail. 48 kHz.
  bullet_whizz.wav          subsonic pass-by: band-passed noise swept down (Doppler).
  bullet_ricochet.wav       ricochet "zing": a tumbling fragment's falling whistle.
  impact_plate_ring.wav     a steel plate struck by a rifle round: inharmonic modes ringing down.
  shot_<gun>_distant.wav    the existing reports through ~600 m of air: low-passed, the attack
                            softened, a long reflection tail (the far report layer past 150 m).
"""
import os
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SND = os.path.join(ROOT, "assets", "sounds")
RNG = np.random.default_rng(0xBA11)


def write(name, x, rate):
    x = np.asarray(x, dtype=np.float64)
    peak = np.max(np.abs(x)) or 1.0
    x = np.clip(x / peak * 0.9, -1, 1)
    with wave.open(os.path.join(SND, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    print("wrote", name, f"{len(x) / rate:.3f} s")


def read(name):
    with wave.open(os.path.join(SND, name + ".wav"), "rb") as w:
        rate = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0
        if w.getnchannels() == 2:
            x = x[::2]
    return x, rate


def one_pole_lp(x, rate, fc):
    a = np.exp(-2 * np.pi * fc / rate)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1 - a) * v + a * acc
        y[i] = acc
    return y


def lowpass(x, rate, fc, order=2):
    for _ in range(order):
        x = one_pole_lp(x, rate, fc)
    return x


def highpass(x, rate, fc):
    return x - lowpass(x, rate, fc, 1)


def n_wave(rate, length_s, amp=1.0):
    n = max(2, int(round(length_s * rate)))
    w = np.linspace(1.0, -1.0, n) * amp
    return w


def crack(variant):
    rate = 48000
    dur = [0.0002, 0.00045, 0.00063][variant]
    out = np.zeros(int(0.09 * rate))
    nw = n_wave(rate, dur)
    out[10:10 + len(nw)] += nw
    # Ground reflection: a weaker, slightly smeared copy 1.5-3 ms later.
    d = int((0.0015 + 0.0008 * variant) * rate)
    refl = lowpass(np.concatenate([nw, np.zeros(8)]), rate, 9000, 1) * 0.55
    out[10 + d:10 + d + len(refl)] += refl
    # The crack excites the surroundings: a short bright tail.
    tail_n = len(out) - 40
    t = np.arange(tail_n) / rate
    tail = highpass(RNG.standard_normal(tail_n), rate, 2500) * np.exp(-t / (0.006 + 0.004 * variant)) * 0.12
    out[40:] += tail
    write(f"bullet_crack_{variant}", out, rate)


def whizz():
    rate = 48000
    n = int(0.42 * rate)
    t = np.arange(n) / rate
    noise = RNG.standard_normal(n)
    # Band-pass by difference of low-passes with a falling centre (approach -> recede).
    centre = 2400 * (1 - 0.45 * (t / t[-1]))
    seg = 1024
    y = np.zeros(n)
    for s in range(0, n, seg):
        c = float(centre[min(s + seg // 2, n - 1)])
        block = noise[max(0, s - 64):s + seg]
        b = lowpass(block, rate, c * 1.3, 2) - lowpass(block, rate, c * 0.7, 2)
        y[s:s + seg] = b[-len(y[s:s + seg]):]
    envl = np.exp(-((t - 0.16) / 0.07) ** 2)
    write("bullet_whizz", y * envl, rate)


def ricochet():
    rate = 48000
    n = int(0.55 * rate)
    t = np.arange(n) / rate
    f = 3600 * np.exp(-t / 0.45) + 900
    phase = 2 * np.pi * np.cumsum(f * (1 + 0.04 * np.sin(2 * np.pi * 31 * t))) / rate
    tone = np.sin(phase) * np.exp(-t / 0.18)
    hit = highpass(RNG.standard_normal(n), rate, 1500) * np.exp(-t / 0.012) * 0.8
    write("bullet_ricochet", tone * 0.7 + hit, rate)


def plate_ring():
    rate = 48000
    n = int(1.2 * rate)
    t = np.arange(n) / rate
    # A struck steel plate: inharmonic bending modes (ratios of a free plate), each ringing down at
    # its own rate, on top of the impact's click.
    y = np.zeros(n)
    for f, a, tau in [(420, 1.0, 0.55), (1020, 0.7, 0.35), (1690, 0.5, 0.25), (2580, 0.35, 0.18), (3610, 0.25, 0.12)]:
        y += a * np.sin(2 * np.pi * f * t + RNG.uniform(0, 6.28)) * np.exp(-t / tau)
    y += highpass(RNG.standard_normal(n), rate, 2000) * np.exp(-t / 0.004) * 1.5
    write("impact_plate_ring", y, rate)


def distant(gun):
    x, rate = read("shot_" + gun)
    # ~600 m of air: strong high-frequency absorption, a softer onset, then echoes off terrain.
    far = lowpass(x, rate, 1400, 3)
    tail_n = int(1.1 * rate)
    t = np.arange(tail_n) / rate
    ir = RNG.standard_normal(tail_n) * np.exp(-t / 0.32) * 0.03
    ir[0] = 1.0
    for k, (dt, g) in enumerate([(0.11, 0.35), (0.27, 0.22), (0.48, 0.12)]):
        ir[int(dt * rate)] += g
    y = np.convolve(far, ir)[: len(far) + tail_n]
    y = lowpass(y, rate, 2200, 1)
    write(f"shot_{gun}_distant", y, rate)


def main():
    for v in range(3):
        crack(v)
    whizz()
    ricochet()
    plate_ring()
    for gun in ["pistol", "smg", "rifle", "lmg", "dmr", "sniper", "shotgun"]:
        distant(gun)


if __name__ == "__main__":
    main()
