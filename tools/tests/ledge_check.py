#!/usr/bin/env python3
"""Checks a ledge.cfg run (docs/CONTROLLER.md): the movement mode and position each case ends in,
the hang prompts (both shown while hanging, never during an auto-climb or a blocked climb), the
regrab cooldown and the stamina rule. Usage: python3 tools/tests/ledge_check.py <log>"""
import re
import sys

CASE = re.compile(r"ledge_case (\S+)")
REP = re.compile(r"ledge_report: mode (\w+) pos ([-\d.]+) ([-\d.]+) ([-\d.]+) .*? ground (\d) .*? prompts (on|off) \((.*?) \| (.*?)\) drawn (\d)")
SHOWN = re.compile(r"ledge: prompts shown: (.*)")

TOP = 2.6        # the yard's walls
FACE_Z = 10.75   # their south face


def expect(case, mode, x, y, z, ground, prompts, shown):
    """Failures of one case (empty = pass)."""
    f = []
    hanging = mode == "hang"
    if case in ("fall_grab", "run_grab", "regrab_free"):
        if not hanging:
            f.append("expected hang, got %s" % mode)
        if prompts != "on":
            f.append("no prompts while hanging")
    elif case in ("let_go", "regrab_blocked", "stamina"):
        if mode != "walk" or not ground or y > 0.2:
            f.append("expected to drop to the floor (walk on the ground), got %s at y %.2f" % (mode, y))
        if z < FACE_Z:
            f.append("ended inside / past the wall (z %.2f)" % z)
    elif case in ("climb", "run_climb"):
        if mode != "walk" or abs(y - TOP) > 0.15 or z > FACE_Z:
            f.append("expected standing on the 2.6 m top, got %s at y %.2f z %.2f" % (mode, y, z))
    elif case == "no_clearance":
        if mode != "walk" or y > 0.2:
            f.append("climbed into the slab: %s at y %.2f" % (mode, y))
    if case in ("run_climb", "no_clearance") and shown:
        f.append("the hang prompts showed (%d time(s)): an auto-climb / blocked climb never hangs" % shown)
    if case in ("fall_grab", "run_grab") and not shown:
        f.append("the HUD never showed the prompts")
    return f


def main():
    case, fails, seen, shown = None, [], set(), 0
    prompt_texts = []
    for line in open(sys.argv[1], errors="replace"):
        m = CASE.search(line)
        if m and "echo" not in line:
            case, shown = m.group(1), 0
            continue
        m = SHOWN.search(line)
        if m and case:
            shown += 1
            prompt_texts.append(m.group(1))
            continue
        m = REP.search(line)
        if m and case:
            mode, x, y, z = m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4))
            ground, prompts = int(m.group(5)), m.group(6)
            seen.add(case)
            if prompts == "on" and ("Climb" not in m.group(7) or "Let go" not in m.group(8)):
                fails.append("%s: prompts '%s' / '%s'" % (case, m.group(7), m.group(8)))
            for f in expect(case, mode, x, y, z, ground, prompts, shown):
                fails.append("%s: %s" % (case, f))
            print("%-15s %-6s pos %.2f %.2f %.2f ground %d prompts %s (shown %d)" % (case, mode, x, y, z, ground, prompts, shown))
    for c in ("fall_grab", "let_go", "climb", "run_climb", "run_grab", "regrab_blocked", "regrab_free", "stamina", "no_clearance"):
        if c not in seen:
            fails.append("%s: no ledge_report" % c)
    if prompt_texts:
        print("prompts: %s" % prompt_texts[0])
    for f in fails:
        print("FAIL " + f)
    print("ledge_check: %d cases, %d failure(s)" % (len(seen), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
