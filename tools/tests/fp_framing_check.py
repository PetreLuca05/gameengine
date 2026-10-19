"""Checks a tools/tests/fp_framing.cfg log (docs/CAMERA_HOLDPOINT.md "Framing").

  python fp_framing_check.py <log>

- rifle80: the rifle against the reference (fp_framing's own PASS: points 2 %, left hand 7 %, angle 3 deg,
  no near-plane cut on screen, the muzzle on screen);
- rifle100 / rifle120: the arms placement does not move with the world's Field of View (within 0.2 %);
- pitch_down / pitch_up: nor with the view pitch (within 1 %);
- narrow / wide: the ADS sight point keeps its place across the Weapon Field of View presets (1 %);
- the hip -> ADS -> hip track: the sight heads for the screen centre without passing it and back
  without overshooting its hip place (0.5 %);
- every other gun: its own PASS (the hand within 3 % of the rifle's, near plane, muzzle).
"""
import re
import sys

PT = r"(-?[\d.]+) (-?[\d.]+)"


def main(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    fails = []
    pts = {}
    for m in re.finditer(r"fp_framing (\w+): front " + PT + r"  rear " + PT + r"  muzzle " + PT + r"  sight " + PT + r"  left hand " + PT, text):
        v = [float(x) for x in m.groups()[1:]]
        pts[m.group(1)] = {"front": v[0:2], "rear": v[2:4], "sight": v[6:8], "left": v[8:10]}
    verdict = dict(re.findall(r"fp_framing (\w+): (?:vs reference|class \w+): .*-> (PASS|FAIL)", text))

    def check(ok, msg):
        print(("ok    " if ok else "FAIL  ") + msg)
        if not ok:
            fails.append(msg)

    def dist(a, b):
        return max(abs(a[0] - b[0]), abs(a[1] - b[1]))

    check(verdict.get("rifle80") == "PASS", "rifle at Field of View 80 against the reference: %s" % verdict.get("rifle80"))
    base = pts.get("rifle80")
    if not base:
        check(False, "no rifle80 measurement")
        return fails
    for tag, tol, what in (("rifle100", 0.002, "Field of View 100"), ("rifle120", 0.002, "Field of View 120"), ("pitch_down", 0.01, "60 deg down"),
                           ("pitch_up", 0.01, "60 deg up")):
        p = pts.get(tag)
        d = max(dist(p[k], base[k]) for k in ("front", "rear", "left")) if p else 1
        check(d <= tol, "%s: the arms placement moved %.4f (tolerance %.3f)" % (what, d, tol))
    for tag in ("narrow", "wide"):
        p = pts.get(tag)
        d = dist(p["sight"], base["sight"]) if p else 1
        check(d <= 0.01, "Weapon Field of View %s: the sight moved %.4f" % (tag, d))
    # hip -> ADS -> hip: the sight's distance from the centre never goes past the end it heads for.
    tr = [(float(a), float(x), float(y)) for a, x, y in re.findall(r"fp_framing_track: frame \d+ ads ([\d.]+) front \S+ \S+ rear \S+ \S+ sight (\S+) (\S+)", text)]
    if len(tr) < 20:
        check(False, "track: %d frames" % len(tr))
    else:
        hip = (tr[0][1], tr[0][2])
        r = [max(abs(x - 0.5), abs(y - 0.5)) for _, x, y in tr]
        peak = max(range(len(tr)), key=lambda i: tr[i][0])
        aimed = r[peak]
        over_in = min(r[: peak + 1]) < aimed - 0.005 and aimed > 0.005   # passed the centre and came back
        r_hip = max(abs(hip[0] - 0.5), abs(hip[1] - 0.5))
        over_out = max(r[peak:]) > r_hip + 0.005
        check(aimed < 0.01, "track: aimed, the sight %.4f from the centre" % aimed)
        check(not over_in and not over_out, "track: hip -> ADS -> hip without overshoot (closest %.4f, aimed %.4f, farthest back %.4f, hip %.4f)" % (
            min(r[: peak + 1]), aimed, max(r[peak:]), r_hip))
    for gun in ("pistol", "smg", "lmg", "shotgun", "dmr", "sniper", "crossbow"):
        check(verdict.get(gun) == "PASS", "%s: %s" % (gun, verdict.get(gun)))
    return fails


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    f = main(sys.argv[1])
    print("fp_framing_check: %d failure(s)" % len(f))
    sys.exit(1 if f else 0)
