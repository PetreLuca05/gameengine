"""Chapter 1 ("10th Floor") design sketches: apartment plan + vista site plan + the two locked view frames.

Writes SVGs to docs/story/ch01/. All coordinates are the ones in docs/CHAPTER_01.md (unit-local metres:
origin = north-west interior corner of unit 10A, +x east, +z south, yaw 0 looks -Z = north; floor top y = 30.0).
Run: python tools/story/ch01_sketches.py
"""
import math, os, random

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "story", "ch01")

# --- apartment plan -----------------------------------------------------------------------------------------
ROOMS = [  # name, x0, x1, z0, z1, fill
    ("Master bedroom", 0.0, 4.5, 0.0, 4.5, "#e9dcc9"),
    ("En-suite", 0.0, 2.4, 4.5, 7.0, "#cfe3ea"),
    ("Walk-in closet", 2.4, 4.5, 4.5, 7.0, "#e6dccd"),
    ("Dining", 4.5, 7.5, 0.0, 3.0, "#efe6d2"),
    ("Kitchen", 4.5, 9.6, 3.0, 6.0, "#efe6d2"),
    ("Living", 7.5, 12.0, 0.0, 3.0, "#efe6d2"),
    ("", 9.6, 12.0, 3.0, 6.0, "#efe6d2"),
    ("Hall", 4.5, 6.0, 6.0, 12.0, "#dedede"),
    ("Study nook", 6.0, 8.4, 6.0, 7.2, "#e4ddd0"),
    ("Family bath", 6.0, 8.4, 7.2, 9.6, "#cfe3ea"),
    ("Side hall", 6.0, 8.4, 9.6, 10.6, "#dedede"),
    ("Storage", 6.0, 8.4, 10.6, 12.0, "#d6d0c4"),
    ("Utility", 3.0, 4.5, 7.0, 10.5, "#d8dde0"),
    ("Coats", 3.0, 4.5, 10.5, 12.0, "#d6d0c4"),
    ("Teen room", 8.4, 12.0, 6.0, 9.0, "#dfe6d4"),
    ("Child room", 8.4, 12.0, 9.0, 12.0, "#f1e0e3"),
]
AREA = {"Living / dining / kitchen": 45.0}
# doors: (x, z, width, axis, label)  axis 'x' = opening runs along x (wall at const z)
DOORS = [
    (4.8, 12.0, 0.9, "x", "entrance (security)"),
    (4.5, 6.0, 0.9, "z", "WIC"), (3.3, 4.5, 0.9, "x", ""), (0.6, 4.5, 0.8, "x", ""),
    (6.0, 7.8, 0.8, "z", ""), (4.5, 8.0, 0.8, "z", ""), (4.5, 10.8, 0.9, "z", "bi-fold"),
    (8.4, 6.15, 0.9, "z", ""), (8.4, 9.65, 0.9, "z", ""), (6.5, 10.6, 0.8, "x", ""),
]
PROPS = [  # label, x0, x1, z0, z1
    ("bed", 1.2, 3.2, 1.0, 3.1), ("sofa L", 8.6, 11.2, 2.4, 3.3), ("", 10.4, 11.2, 0.9, 2.4),
    ("island", 6.2, 8.6, 4.1, 5.0), ("kitchen run", 4.6, 9.6, 5.35, 5.95), ("table", 5.4, 7.0, 1.0, 2.0),
    ("TV", 10.2, 11.8, 5.7, 5.95), ("desk", 6.1, 8.3, 6.05, 6.6), ("breaker", 3.05, 3.25, 9.6, 10.3),
    ("W/D", 3.05, 3.7, 7.1, 7.8), ("bunk", 10.4, 11.9, 9.2, 11.1), ("bed", 10.0, 11.9, 6.2, 7.6),
    ("tub", 6.05, 7.7, 8.85, 9.55), ("shower", 0.05, 1.1, 5.8, 6.95), ("reading", 7.6, 8.4, 0.2, 1.0),
]


def plan_svg():
    S, ox, oz = 50.0, 12.6, 2.6  # px per m, origin offset in m (left/top margin)
    W, H = int(36 * S), int(27.2 * S)
    X = lambda x: (x + ox) * S
    Z = lambda z: (z + oz) * S
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="Segoe UI,Arial" font-size="12">',
         '<rect width="100%" height="100%" fill="#fbfaf7"/>',
         '<defs><pattern id="hatch" width="8" height="8" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
         '<line x1="0" y1="0" x2="0" y2="8" stroke="#9ab" stroke-width="2"/></pattern>'
         '<pattern id="hatch2" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(-45)">'
         '<line x1="0" y1="0" x2="0" y2="6" stroke="#bbb" stroke-width="1.5"/></pattern></defs>']
    # 1 m grid
    for gx in range(-12, 24):
        o.append(f'<line x1="{X(gx)}" y1="{Z(-2.5)}" x2="{X(gx)}" y2="{Z(24.4)}" stroke="#eee"/>')
    for gz in range(-2, 25):
        o.append(f'<line x1="{X(-12.5)}" y1="{Z(gz)}" x2="{X(23.2)}" y2="{Z(gz)}" stroke="#eee"/>')
    rect = lambda x0, x1, z0, z1, **a: o.append(
        f'<rect x="{X(x0)}" y="{Z(z0)}" width="{(x1-x0)*S}" height="{(z1-z0)*S}" ' + " ".join(f'{k.replace("_","-")}="{v}"' for k, v in a.items()) + "/>")
    text = lambda x, z, t, **a: o.append(f'<text x="{X(x)}" y="{Z(z)}" text-anchor="middle" ' + " ".join(f'{k.replace("_","-")}="{v}"' for k, v in a.items()) + f'>{t}</text>')
    # balcony
    rect(4.5, 13.2, -1.2, 0, fill="url(#hatch)", stroke="#6a8")
    rect(12.0, 13.2, 0, 6.0, fill="url(#hatch)", stroke="#6a8")
    text(8.0, -0.45, "BALCONY 1.2 m (glass balustrade)", fill="#365")
    # neighbours / core
    rect(-8.5, 0.0, 0, 7.0, fill="url(#hatch2)", stroke="#999"); rect(-8.5, 3.0, 7.0, 12.0, fill="url(#hatch2)", stroke="#999")
    text(-3.5, 6.2, "10B (neighbour: knocks; co-op slot 4)", fill="#666")
    rect(-8.5, 12.0, 12.3, 14.1, fill="#e8e8e8", stroke="#999"); text(-2.5, 13.35, "SHARED CORRIDOR (10th floor)", fill="#555")
    rect(-12.0, -8.5, 8.0, 20.0, fill="#e0d6c8", stroke="#888"); text(-10.25, 14.0, "STAIR", font_weight="bold"); text(-10.25, 14.6, "core", fill="#666")
    for i in range(6):
        rect(-11.8, -10.4, 9.0 + i * 0.28, 9.0 + i * 0.28 + 0.27, fill="#cbbfae", stroke="#a99")
        rect(-10.1, -8.7, 17.5 - i * 0.28, 17.5 - i * 0.28 + 0.27, fill="#cbbfae", stroke="#a99")
    rect(0.0, 6.0, 14.4, 17.5, fill="#e8e3da", stroke="#999"); text(3.0, 15.9, "ELEVATOR LOBBY", fill="#555")
    rect(1.0, 2.2, 17.5, 18.6, fill="#bbb", stroke="#777"); rect(3.8, 5.0, 17.5, 18.6, fill="#bbb", stroke="#777"); text(3.0, 19.2, "2 lifts (dead)", fill="#666")
    rect(-8.5, 0.0, 14.4, 24.0, fill="url(#hatch2)", stroke="#999"); text(-4.2, 19.5, "10D", fill="#666")
    rect(6.0, 12.0, 17.5, 24.0, fill="url(#hatch2)", stroke="#999"); text(9.0, 21.0, "10C (openable later)", fill="#666")
    for (x0, lab) in ((0.6, "10B"), (-4.5, "10D"), (8.5, "10C")):
        z = 12.3 if lab == "10B" else 14.1
        o.append(f'<line x1="{X(x0)}" y1="{Z(z)}" x2="{X(x0+0.9)}" y2="{Z(z)}" stroke="#a33" stroke-width="5"/>')
    o.append(f'<line x1="{X(-8.5)}" y1="{Z(12.7)}" x2="{X(-8.5)}" y2="{Z(13.6)}" stroke="#c60" stroke-width="6"/>')
    text(-7.0, 12.95, "fire door", fill="#c60", font_size="11")
    o.append(f'<line x1="{X(12.0)}" y1="{Z(12.3)}" x2="{X(12.0)}" y2="{Z(14.1)}" stroke="#39c" stroke-width="6"/>')
    text(13.3, 13.4, "end window (E)", fill="#39c", font_size="11")
    # riser shaft on the corridor side
    rect(2.0, 3.0, 12.0, 12.3, fill="#999")
    # rooms
    for name, x0, x1, z0, z1, fill in ROOMS:
        rect(x0, x1, z0, z1, fill=fill, stroke="#555", stroke_width="1")
    # LDK outline to show the open plan
    o.append(f'<path d="M{X(4.5)},{Z(0)} H{X(12)} V{Z(6)} H{X(4.5)} Z" fill="none" stroke="#c84" stroke-dasharray="6 4" stroke-width="2"/>')
    # props
    for lab, x0, x1, z0, z1 in PROPS:
        rect(x0, x1, z0, z1, fill="#fff", fill_opacity="0.5", stroke="#888", stroke_dasharray="3 2")
        if lab:
            text((x0 + x1) / 2, (z0 + z1) / 2 + 0.12, lab, fill="#777", font_size="9")
    for name, x0, x1, z0, z1, _ in ROOMS:
        if not name or name in ("Dining", "Kitchen", "Living"):
            continue
        a = (x1 - x0) * (z1 - z0)
        text((x0 + x1) / 2, (z0 + z1) / 2, name, font_weight="bold" if a > 9 else "normal", font_size="13" if a > 5 else "10")
        text((x0 + x1) / 2, (z0 + z1) / 2 + 0.38, f"{a:.1f} m²", fill="#666", font_size="10")
    text(6.0, 2.75, "DINING", font_weight="bold"); text(10.2, 1.7, "LIVING", font_weight="bold")
    text(10.3, 4.6, "living / dining / kitchen 45.0 m²", fill="#a63", font_size="11"); text(5.6, 4.3, "KITCHEN", font_weight="bold", font_size="11")
    # partitions are drawn by rect strokes; exterior glass on top
    for x in [i * 1.5 for i in range(9)]:
        o.append(f'<rect x="{X(x)-3}" y="{Z(0)-3}" width="6" height="6" fill="#246"/>')
    for z in [i * 1.5 for i in range(9)]:
        o.append(f'<rect x="{X(12)-3}" y="{Z(z)-3}" width="6" height="6" fill="#246"/>')
    o.append(f'<line x1="{X(0)}" y1="{Z(0)}" x2="{X(12)}" y2="{Z(0)}" stroke="#2aa7d8" stroke-width="5" stroke-opacity="0.8"/>')
    o.append(f'<line x1="{X(12)}" y1="{Z(0)}" x2="{X(12)}" y2="{Z(12)}" stroke="#2aa7d8" stroke-width="5" stroke-opacity="0.8"/>')
    o.append(f'<line x1="{X(10.5)}" y1="{Z(0)-6}" x2="{X(12)}" y2="{Z(0)-6}" stroke="#d33" stroke-width="3"/>')
    text(11.25, -0.75, "sliding door", fill="#d33", font_size="11")
    text(6.0, -1.55, "NORTH WALL: FULL-HEIGHT GLASS, 8 × 1.5 m modules (mullions ■)", fill="#1b7fae", font_weight="bold")
    o.append(f'<text x="{X(14.0)}" y="{Z(6)}" fill="#1b7fae" font-weight="bold" transform="rotate(90 {X(14.0)} {Z(6)})" text-anchor="middle">EAST WALL: FULL-HEIGHT GLASS, 8 × 1.5 m modules</text>')
    # solid walls: west/south exterior-to-neighbour
    o.append(f'<path d="M{X(0)},{Z(0)} V{Z(7)} H{X(3)} V{Z(12)} H{X(12)}" fill="none" stroke="#333" stroke-width="5"/>')
    # doors
    for x, z, w, ax, lab in DOORS:
        if ax == "x":
            o.append(f'<line x1="{X(x)}" y1="{Z(z)}" x2="{X(x+w)}" y2="{Z(z)}" stroke="#fbfaf7" stroke-width="6"/>')
            o.append(f'<path d="M{X(x)},{Z(z)} A{w*S},{w*S} 0 0 1 {X(x+w)},{Z(z-w)}" fill="none" stroke="#a33"/>')
        else:
            o.append(f'<line x1="{X(x)}" y1="{Z(z)}" x2="{X(x)}" y2="{Z(z+w)}" stroke="#fbfaf7" stroke-width="6"/>')
            o.append(f'<path d="M{X(x)},{Z(z)} A{w*S},{w*S} 0 0 1 {X(x+w)},{Z(z+w)}" fill="none" stroke="#a33"/>')
        if lab:
            text(x + w / 2 + 0.2, z + (0.5 if ax == "x" else w + 0.4), lab, fill="#a33", font_size="10")
    # locked view frames (yaw from north, clockwise toward east)
    for (px, pz, yaw, hfov, lab, col) in ((9.4, 3.6, 25, 90, "SOFA frame", "#e07020"), (12.9, -0.9, 35, 90, "BALCONY frame", "#8040c0")):
        r = 4.5
        a0, a1 = math.radians(yaw - hfov / 2), math.radians(yaw + hfov / 2)
        p0 = (px + r * math.sin(a0), pz - r * math.cos(a0)); p1 = (px + r * math.sin(a1), pz - r * math.cos(a1))
        o.append(f'<path d="M{X(px)},{Z(pz)} L{X(p0[0])},{Z(p0[1])} A{r*S},{r*S} 0 0 1 {X(p1[0])},{Z(p1[1])} Z" fill="{col}" fill-opacity="0.12" stroke="{col}"/>')
        o.append(f'<circle cx="{X(px)}" cy="{Z(pz)}" r="5" fill="{col}"/>')
        text(px + 0.2, pz + 0.55, lab, fill=col, font_weight="bold", font_size="11")
    # markers
    for (x, z, lab, col) in ((2.2, 2.0, "① spawn (bed)", "#06c"), (3.15, 9.95, "④ breaker", "#c00"), (5.25, 11.6, "⑥ peephole", "#c00"), (-8.2, 13.15, "⑧ exit", "#c00")):
        text(x, z - 0.45, lab, fill=col, font_weight="bold", font_size="11")
    # scale bar + north arrow + title
    o.append(f'<line x1="{X(-7.5)}" y1="{Z(23.5)}" x2="{X(-2.5)}" y2="{Z(23.5)}" stroke="#000" stroke-width="3"/>'); text(-5, 23.2, "5 m")
    o.append(f'<path d="M{X(20)},{Z(-0.5)} l-10,30 l10,-8 l10,8 Z" fill="#000"/>'); text(20, -0.7, "N (−Z)", font_weight="bold")
    o.append(f'<text x="{X(14.2)}" y="{Z(17.0)}" font-size="20" font-weight="bold">Unit 10A, 10th floor (floor top y = 30.0)</text>')
    for i, l in enumerate(["Corner unit, 129 m² gross (12 × 12 minus the 3 × 5 m notch to 10B)",
                           "Ceiling 2.7 m; walls 0.2 m interior / 0.3 m exterior",
                           "Every partition that meets the glass lands on a mullion (1.5 m)",
                           "Unit-local coords: origin = NW interior corner, +x east, +z south",
                           "Glass walls: cyan; solid party/corridor walls: black",
                           "Dashed boxes: key props; red arcs: door swings",
                           "Orange/purple cones: the two locked vista frames (hfov 90°)"]):
        o.append(f'<text x="{X(14.2)}" y="{Z(17.8 + i * 0.55)}" font-size="12" fill="#444">{l}</text>')
    o.append("</svg>")
    return "\n".join(o)


# --- city: landmarks by bearing (degrees clockwise from north) and distance (m) ------------------------------
EYE = 31.65  # floor 30.0 + 1.65 eye
LANDMARKS = [  # name, bearing, dist, height, width, kind
    ("glass spire (Meridian Needle)", 28, 2200, 310, 70, "spire"),
    ("financial cluster", -2, 3600, 230, 380, "cluster"),
    ("Halden Tower (flickers)", 82, 360, 95, 30, "tower"),
    ("St. Aldric Hospital (smoke origin)", 47, 1750, 45, 140, "block"),
    ("crane A", 14, 900, 85, 6, "crane"), ("crane B", 61, 1400, 95, 6, "crane"),
    ("stadium stub", 118, 2700, 40, 260, "stadium"),
    ("modern towers E", 70, 2600, 150, 160, "cluster"),
]


def vista_plan_svg():
    W = H = 1100; c = W / 2; s = W / 2 / 4200.0
    P = lambda b, d: (c + math.sin(math.radians(b)) * d * s, c - math.cos(math.radians(b)) * d * s)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="Segoe UI,Arial" font-size="12">',
         '<rect width="100%" height="100%" fill="#f4f1ea"/>']
    for r, lab, col in ((500, "near: real kit geometry 0–500 m", "#c96"), (1500, "mid: vgeo + impostors to 1.5 km (zfar)", "#69c"), (4000, "far: vista layer 1.5–8 km", "#999")):
        o.append(f'<circle cx="{c}" cy="{c}" r="{r*s}" fill="none" stroke="{col}" stroke-dasharray="6 4"/>')
        o.append(f'<text x="{c+4}" y="{c-r*s-4}" fill="{col}">{lab}</text>')
    # river: curve passing ~450 m north, bending NE
    pts = [P(b, d) for b, d in ((-90, 900), (-60, 640), (-30, 520), (0, 470), (25, 520), (50, 650), (70, 900), (82, 1400), (88, 2200))]
    o.append('<polyline points="' + " ".join(f"{x:.0f},{y:.0f}" for x, y in pts) + '" fill="none" stroke="#5b8fc7" stroke-width="22" stroke-opacity="0.7"/>')
    o.append(f'<text x="{pts[2][0]}" y="{pts[2][1]-16}" fill="#2a5f9e" font-weight="bold">river (bridges at −10° and 35°)</text>')
    for b, d in ((-10, 490), (35, 560)):
        x, y = P(b, d); o.append(f'<rect x="{x-4}" y="{y-24}" width="8" height="48" fill="#555" transform="rotate({b} {x} {y})"/>')
    x, y = P(10, 320); o.append(f'<ellipse cx="{x}" cy="{y}" rx="40" ry="20" fill="#8bb874"/>'); o.append(f'<text x="{x}" y="{y+4}" text-anchor="middle" fill="#355">park</text>')
    for name, b, d, h, w, kind in LANDMARKS:
        x, y = P(b, d); col = {"spire": "#1f6fb0", "crane": "#e0a000", "block": "#b33"}.get(kind, "#444")
        o.append(f'<circle cx="{x}" cy="{y}" r="{max(4, w*s/2):.1f}" fill="{col}" fill-opacity="0.75"/>')
        o.append(f'<text x="{x+8}" y="{y+4}" fill="{col}">{name} · {b}° · {d/1000:.2f} km · {h} m</text>')
    # helicopter path
    a, b2 = P(-80, 1200), P(40, 1600)
    o.append(f'<path d="M{a[0]},{a[1]} Q{c},{c-700*s} {b2[0]},{b2[1]}" fill="none" stroke="#a0a" stroke-dasharray="4 3" stroke-width="2"/>')
    o.append(f'<text x="{b2[0]-30}" y="{b2[1]-12}" fill="#a0a">helicopter → crash (beat 5)</text>')
    # frames
    for yaw, lab, col in ((25, "sofa", "#e07020"), (35, "balcony", "#8040c0")):
        for e in (-45, 45):
            x, y = P(yaw + e, 4100); o.append(f'<line x1="{c}" y1="{c}" x2="{x}" y2="{y}" stroke="{col}" stroke-opacity="0.6"/>')
        x, y = P(yaw - 12, 1900 if lab == "sofa" else 2500); o.append(f'<text x="{x}" y="{y}" fill="{col}" font-weight="bold">{lab} frame (yaw {yaw}°)</text>')
    o.append(f'<rect x="{c-5}" y="{c-5}" width="10" height="10" fill="#000"/><text x="{c+8}" y="{c+18}" font-weight="bold">our tower (16 storeys, unit 10A NE corner)</text>')
    o.append(f'<text x="{c}" y="24" text-anchor="middle" font-weight="bold" font-size="16">Vista site plan: N up, rings at 0.5 / 1.5 / 4 km</text>')
    o.append("</svg>")
    return "\n".join(o)


def frame_svg(yaw, hfov, title, eye_xyz):
    W, H = 1600, 900
    tx = math.tan(math.radians(hfov / 2)); ty = tx * H / W
    def proj(b, d, h):
        rel = math.radians(((b - yaw + 540) % 360) - 180)
        if abs(rel) > math.radians(80):
            return None
        fwd = d * math.cos(rel); side = d * math.sin(rel)
        return (W / 2 + side / fwd / tx * W / 2, H / 2 - (h - EYE) / fwd / ty * H / 2)
    rnd = random.Random(1010)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="Segoe UI,Arial" font-size="14">',
         '<defs><linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4f7fc0"/><stop offset="0.5" stop-color="#cfdcea"/></linearGradient></defs>',
         f'<rect width="{W}" height="{H}" fill="url(#sky)"/>']
    hy = H / 2
    o.append(f'<rect y="{hy}" width="{W}" height="{H-hy}" fill="#b7b3a8"/>')
    # cloud bands
    for i in range(14):
        cx, cy = rnd.uniform(0, W), rnd.uniform(60, hy - 120)
        o.append(f'<ellipse cx="{cx:.0f}" cy="{cy:.0f}" rx="{rnd.uniform(120,320):.0f}" ry="{rnd.uniform(12,30):.0f}" fill="#fff" fill-opacity="0.55"/>')
    # procedural city by distance bands, far to near (painter's order); colour lerps toward haze
    def city_band(d0, d1, hmin, hmax, n, base, haze, wmin, wmax):
        items = []
        for _ in range(n):
            b = yaw + rnd.uniform(-hfov / 2 - 8, hfov / 2 + 8); d = rnd.uniform(d0, d1)
            items.append((d, b, rnd.uniform(hmin, hmax), rnd.uniform(wmin, wmax)))
        for d, b, h, w in sorted(items, reverse=True):
            top = proj(b, d, h); bot = proj(b, d, 0)
            if not top:
                continue
            pw = w / (d * tx) * W / 2; t = min(1, (d - 300) / 3500)
            col = tuple(int(base[i] * (1 - t) + haze[i] * t) for i in range(3))
            o.append(f'<rect x="{top[0]-pw/2:.1f}" y="{top[1]:.1f}" width="{pw:.1f}" height="{bot[1]-top[1]:.1f}" fill="rgb{col}"/>')
    haze = (196, 206, 218)
    city_band(2500, 5500, 15, 80, 500, (120, 120, 125), haze, 30, 90)
    city_band(1200, 2500, 12, 55, 450, (125, 110, 100), haze, 25, 70)
    # river
    rv = [proj(b, d, 0) for b, d in ((-90, 900), (-60, 640), (-30, 520), (0, 470), (25, 520), (50, 650), (70, 900), (82, 1400), (88, 2200))]
    rv = [p for p in rv if p]
    if len(rv) > 1:
        o.append('<polyline points="' + " ".join(f"{x:.0f},{y:.0f}" for x, y in rv) + '" fill="none" stroke="#6f93b8" stroke-width="16"/>')
    city_band(600, 1200, 10, 40, 380, (130, 100, 85), haze, 20, 55)
    # landmarks
    for name, b, d, h, w, kind in LANDMARKS:
        top, bot = proj(b, d, h), proj(b, d, 0)
        if not top:
            continue
        pw = w / (d * tx) * W / 2
        if kind == "spire":
            o.append(f'<path d="M{top[0]:.0f},{top[1]:.0f} L{top[0]+pw/2:.0f},{bot[1]:.0f} L{top[0]-pw/2:.0f},{bot[1]:.0f} Z" fill="#7d9bb8" stroke="#40607e"/>')
        elif kind == "crane":
            o.append(f'<line x1="{top[0]:.0f}" y1="{top[1]:.0f}" x2="{bot[0]:.0f}" y2="{bot[1]:.0f}" stroke="#d09a20" stroke-width="2"/><line x1="{top[0]-40:.0f}" y1="{top[1]:.0f}" x2="{top[0]+60:.0f}" y2="{top[1]:.0f}" stroke="#d09a20" stroke-width="2"/>')
        elif kind == "cluster":
            for i in range(7):
                hh = h * rnd.uniform(0.45, 1.0); bb = b + rnd.uniform(-1, 1) * w / d * 30
                t2, b2 = proj(bb, d, hh), proj(bb, d, 0)
                if t2:
                    pp = rnd.uniform(25, 55) / (d * tx) * W / 2
                    o.append(f'<rect x="{t2[0]-pp/2:.0f}" y="{t2[1]:.0f}" width="{pp:.0f}" height="{b2[1]-t2[1]:.0f}" fill="#8aa0b4" stroke="#6d8296"/>')
        elif kind == "stadium":
            o.append(f'<ellipse cx="{bot[0]:.0f}" cy="{(top[1]+bot[1])/2:.0f}" rx="{pw/2:.0f}" ry="{(bot[1]-top[1])/2:.0f}" fill="#a9a9a3"/>')
        else:
            col = "#9c4b3a" if kind == "block" else "#7b6a5c"
            o.append(f'<rect x="{top[0]-pw/2:.0f}" y="{top[1]:.0f}" width="{pw:.0f}" height="{bot[1]-top[1]:.0f}" fill="{col}"/>')
        if kind == "tower":
            for k in range(10):
                o.append(f'<rect x="{top[0]-pw/2+6+(k%3)*pw/3.4:.0f}" y="{top[1]+12+k*((bot[1]-top[1])/11):.0f}" width="6" height="4" fill="#ffd36a"/>')
        if kind == "block":  # smoke column (beat 5)
            o.append(f'<path d="M{top[0]:.0f},{top[1]:.0f} C{top[0]-30:.0f},{top[1]-120:.0f} {top[0]+60:.0f},{top[1]-200:.0f} {top[0]+140:.0f},{top[1]-330:.0f}" fill="none" stroke="#444" stroke-width="34" stroke-opacity="0.45" stroke-linecap="round"/>')
        o.append(f'<line x1="{top[0]:.0f}" y1="{top[1]-6:.0f}" x2="{top[0]:.0f}" y2="{top[1]-40:.0f}" stroke="#000" stroke-width="1"/><text x="{top[0]+4:.0f}" y="{top[1]-44:.0f}" font-size="13">{name}</text>')
    # near rooftops (real kit geometry): big shapes in the lower frame
    for _ in range(28):
        b = yaw + rnd.uniform(-hfov / 2 - 10, hfov / 2 + 10); d = rnd.uniform(160, 550); h = rnd.uniform(10, 24)
        top, bot = proj(b, d, h), proj(b, d, 0)
        if not top:
            continue
        pw = rnd.uniform(20, 45) / (d * tx) * W / 2
        o.append(f'<rect x="{top[0]-pw/2:.0f}" y="{top[1]:.0f}" width="{pw:.0f}" height="{H-top[1]:.0f}" fill="rgb({rnd.randint(120,150)},{rnd.randint(85,105)},{rnd.randint(70,85)})" stroke="#5a4538" stroke-width="0.5"/>')
        o.append(f'<rect x="{top[0]-pw/2:.0f}" y="{top[1]-3:.0f}" width="{pw:.0f}" height="3" fill="#6b625a"/>')
        if rnd.random() < 0.4:
            o.append(f'<rect x="{top[0]+pw/5:.0f}" y="{top[1]-10:.0f}" width="{pw/7:.0f}" height="10" fill="#777"/>')
    # window frame overlay: mullions every 1.5 m (approx) + floor/ceiling
    if title.startswith("SOFA"):
        for xm in (230, 760, 1290):
            o.append(f'<rect x="{xm}" y="0" width="14" height="{H}" fill="#2b2b2e"/>')
        o.append(f'<rect x="0" y="0" width="{W}" height="58" fill="#e8e3da"/><rect x="0" y="{H-60}" width="{W}" height="60" fill="#8d7457"/>')
        o.append(f'<path d="M0,{H-60} h560 v-90 h-560 Z" fill="#5d5f66"/><text x="20" y="{H-90}" fill="#ddd">sofa back</text>')
    else:
        o.append(f'<rect x="0" y="{H-130}" width="{W}" height="6" fill="#999"/><rect x="0" y="{H-124}" width="{W}" height="124" fill="#9fc3cf" fill-opacity="0.25"/>')
        o.append(f'<rect x="{W*0.22:.0f}" y="{H-130}" width="8" height="130" fill="#888"/><rect x="{W*0.68:.0f}" y="{H-130}" width="8" height="130" fill="#888"/>')
        o.append(f'<text x="20" y="{H-140}" fill="#333">glass balustrade (1.1 m)</text>')
    o.append(f'<rect x="0" y="0" width="{W}" height="34" fill="#000" fill-opacity="0.55"/><text x="14" y="23" fill="#fff" font-size="16" font-weight="bold">{title}</text>')
    o.append(f'<text x="{W-14}" y="23" fill="#fff" text-anchor="end">eye {eye_xyz} · yaw {yaw}° · hfov {hfov}° · 16:9 · composition sketch, not a render</text>')
    o.append("</svg>")
    return "\n".join(o)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    files = {
        "layout_plan.svg": plan_svg(),
        "vista_site_plan.svg": vista_plan_svg(),
        "vista_sofa.svg": frame_svg(25, 90, "SOFA frame: locked reference composition", "(9.4, 31.65, 3.6) unit-local"),
        "vista_balcony.svg": frame_svg(35, 90, "BALCONY corner frame: locked reference composition", "(12.9, 31.65, -0.9) unit-local"),
    }
    for n, s in files.items():
        with open(os.path.join(OUT, n), "w", newline="") as f:
            f.write(s)
        print("wrote", os.path.join(OUT, n))
