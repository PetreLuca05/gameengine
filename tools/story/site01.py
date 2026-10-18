"""Chapter 1 street site (docs/CHAPTER_01.md "Escape route"): everything at street level around the tower
that the player walks through on the rescue morning, plus what the windows see change day by day.

  - the tower's ground storey: a lobby (stair core fire door -> lobby -> glass entrance on the north face)
    and solid ground-floor blocks; the shell above it
  - the street: a plaza north of the tower, the north street, the east street, sidewalks, markings
  - the street walls: box buildings around the two streets, barricades closing the ends
  - Orchard House (the rescue building, NE across the street): 7 storeys, a stair core from the
    ground lobby (south entrance) to a roof hut, the roof with a parapet and a helipad
  - the damage layer: wrecks, scorch, debris, barricades appearing day by day (drawn by the vista from
    the city block's "damage"; the street ones also get collision-only brushes)
  - story points (zombie spawns per day, the helipad) and volumes (escape route)

Coordinates: x east, z south, y up; the tower occupies x -12.3..12.15, z 0..24.3.
build(...) is called by tools/story/build_story01.py.
"""
import math
import random

STOREY = 3.0
RB = {"x0": 44.0, "x1": 68.0, "z0": -50.0, "z1": -26.0, "floors": 7}   # Orchard House
RB_ROOF = RB["floors"] * STOREY                                       # 21 m
RB_CORE = (56.3, -48.0)          # stair_core_dogleg origin: core x 44.3..47.8, z -42..-28, door (47.8, -34.8) facing east
RB_LOBBY = (47.8, 58.0, -38.0, -26.0)
HELIPAD = (60.0, -38.0)
SITE_RECT = (-55.0, -75.0, 80.0, 45.0)   # the vista keeps its own lots out of this (vellmouth.json "own_tower")

CONCRETE = [0.6, 0.59, 0.57]
TILE = [0.72, 0.7, 0.66]


def build(box, ent, point, volume, prefabs, storey_y):
    """box(x0, x1, z0, z1, y0, y1, color, material, **kw), ent(type, pos, **kw), point(name, x, z, yaw, y=),
    volume(name, x0, x1, z0, z1, y0, y1), prefabs: the level's prefab list. Returns the damage list."""
    rng = random.Random(1107)
    damage = []

    def dmg(day, x, z, w, h, d, color, yaw=0.0, y=0.0, collide=True):
        damage.append({"day": day, "p": [round(x, 2), round(y, 2), round(z, 2)], "s": [round(w, 2), round(h, 2), round(d, 2)],
                       "yaw": round(yaw, 1), "c": [round(c, 3) for c in color]})
        if collide:   # street wrecks block the escape route (drawn by the damage layer, so collision only here)
            box(x - w / 2, x + w / 2, z - d / 2, z + d / 2, y, y + h, color, "metal_thick", render=False, yaw=yaw)

    # ------------------------------------------------------------ the tower's ground storey
    G1 = 3.3
    EXT = [0.62, 0.6, 0.58]
    box(6.0, 12.15, 0.0, 24.3, 0.0, G1, EXT, "concrete", facade=True)                 # east block (shops, closed)
    box(-8.35, 6.0, 17.5, 24.3, 0.0, G1, EXT, "concrete")                             # behind the lobby
    box(-12.3, -8.35, 0.0, 5.85, 0.0, G1, EXT, "concrete")                            # north-west corner
    box(-12.3, -8.35, 20.15, 24.3, 0.0, G1, EXT, "concrete")                          # south-west corner
    # Lobby (x -8.35..6, z 0.3..17.5): north facade with the glass entrance, ceiling, floor.
    box(-8.35, -2.8, 0.0, 0.3, 0.0, G1, EXT, "concrete")
    box(-0.4, 6.0, 0.0, 0.3, 0.0, G1, EXT, "concrete")
    box(-2.8, -0.4, 0.0, 0.3, 2.4, G1, EXT, "concrete")
    ent("door", (-1.6, 0.0, 0.15), name="lobby_door", width=2.3, height=2.38, color=[0.7, 0.8, 0.85], glass=True)
    box(-8.35, 6.0, 0.3, 17.5, 3.0, G1, [0.8, 0.8, 0.78], "plaster")
    box(-8.35, 6.0, 0.3, 17.5, -0.02, 0.02, TILE, "tile", fine_grid=False)
    box(-8.2, -7.9, 2.0, 5.5, 0.9, 1.9, [0.55, 0.5, 0.42], "metal_thin")              # mailboxes
    box(0.5, 3.5, 6.0, 7.0, 0.0, 1.05, [0.4, 0.32, 0.25], "wood")                     # concierge desk
    box(2.0, 5.5, 14.4, 17.5, 0.0, G1, EXT, "concrete")                               # lift shaft base
    for x, z in ((-4.0, 4.0), (2.0, 11.0)):
        ent("light", (x, 2.9, z), color=[1.0, 0.92, 0.8], radius=8, intensity=4, group=21, on=False)

    # ------------------------------------------------------------ streets and sidewalks
    box(-16.0, 26.0, -8.0, 0.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")           # plaza north of the tower
    box(12.15, 26.0, 0.0, 34.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")           # east of the tower
    box(-50.0, 44.0, -26.0, -22.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")        # north side of the north street
    box(38.0, 44.0, -22.0, 34.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")          # east side of the east street
    box(22.0, 26.0, -62.0, -26.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")         # east street, west side, north part
    box(38.0, 44.0, -62.0, -26.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")
    box(-50.0, -16.0, -8.0, -4.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")
    box(44.0, 72.0, -8.0, -4.0, 0.0, 0.15, [0.55, 0.54, 0.52], "concrete")
    for x in range(-46, 72, 6):                                                        # north street centre line
        if 25 < x < 39:
            continue
        box(x, x + 3.0, -15.1, -14.9, 0.0, 0.012, [0.85, 0.85, 0.8], "asphalt", collide=False, cast_shadows=False)
    for z in range(-60, 34, 6):                                                        # east street centre line
        if -23 < z < -7:
            continue
        box(31.9, 32.1, z, z + 3.0, 0.0, 0.012, [0.85, 0.85, 0.8], "asphalt", collide=False, cast_shadows=False)
    for k in range(7):                                                                 # zebra crossing to Orchard House
        box(50.0, 54.0, -21.6 + k * 2.0, -20.8 + k * 2.0, 0.0, 0.012, [0.9, 0.9, 0.86], "asphalt", collide=False, cast_shadows=False)

    # ------------------------------------------------------------ street walls (the blocks around the streets)
    def block(x0, x1, z0, z1, h, color):
        box(x0, x1, z0, z1, 0.0, h, color, "brick" if color[0] > color[2] + 0.08 else "concrete", facade=True)
        box(x0 - 0.2, x1 + 0.2, z0 - 0.2, z1 + 0.2, h, h + 0.9, [c * 0.8 for c in color], "concrete")

    # Low enough that the 10th floor sees over them (the river, the bridges); Orchard House stands out.
    block(-50.0, -2.0, -60.0, -26.0, 12.0, [0.55, 0.4, 0.32])        # north-west
    block(-2.0, 22.0, -60.0, -26.0, 15.0, [0.62, 0.6, 0.55])         # north
    block(44.0, 68.0, -74.0, -50.0, 18.0, [0.5, 0.52, 0.55])         # behind Orchard House
    block(68.0, 80.0, -74.0, -26.0, 15.0, [0.6, 0.48, 0.38])         # east of Orchard House
    block(44.0, 72.0, -4.0, 34.0, 12.0, [0.66, 0.62, 0.54])          # south-east, across the east street
    block(-50.0, -16.0, -4.0, 40.0, 18.0, [0.5, 0.36, 0.3])          # west of the tower
    block(12.15, 38.0, 34.0, 45.0, 12.0, [0.6, 0.6, 0.6])            # south end
    # Barricades closing the street ends (there from the start: the city's own; the outbreak adds more).
    box(-16.0, -12.3, 0.5, 0.8, 0.0, 2.4, [0.3, 0.32, 0.3], "metal_thin")              # fence in the alley west of the tower
    for (x0, x1, z0, z1) in ((-50.0, -46.0, -22.0, -8.0), (70.0, 74.0, -26.0, -4.0), (22.0, 44.0, -62.0, -58.0), (12.15, 44.0, 34.0, 38.0)):
        box(x0, x1, z0, z1, 0.0, 1.1, [0.75, 0.72, 0.6], "concrete")                   # jersey barriers
        box(x0, x1, z0, z1, 1.1, 2.6, [0.25, 0.27, 0.26], "metal_thin")                # fencing on top

    # ------------------------------------------------------------ Orchard House (rescue building)
    x0, x1, z0, z1 = RB["x0"], RB["x1"], RB["z0"], RB["z1"]
    lx0, lx1, lz0, lz1 = RB_LOBBY
    RC = [0.78, 0.76, 0.7]
    box(58.0, x1, z0, z1, 0.0, RB_ROOF, RC, "concrete", facade=True)                    # east part, full height
    box(lx0, 58.0, z0, lz0, 0.0, RB_ROOF, RC, "concrete", facade=True)                 # north of the lobby
    box(lx0, 58.0, lz0, z1, G1, RB_ROOF, RC, "concrete", facade=True)                  # above the lobby
    box(x0, lx0, z0, -42.15, 0.0, RB_ROOF, RC, "concrete", facade=True)                # north of the core
    box(x0, lx0, -27.85, z1, 0.0, RB_ROOF, RC, "concrete", facade=True)                # south of the core
    # Lobby south facade with the entrance (glass door), floor.
    box(lx0, 52.0, z1 - 0.3, z1, 0.0, G1, RC, "concrete")
    box(54.4, lx1, z1 - 0.3, z1, 0.0, G1, RC, "concrete")
    box(52.0, 54.4, z1 - 0.3, z1, 2.4, G1, RC, "concrete")
    ent("door", (53.2, 0.0, z1 - 0.15), name="rb_entrance", width=2.3, height=2.38, color=[0.7, 0.8, 0.85], glass=True)
    box(lx0, lx1, lz0, z1 - 0.3, -0.02, 0.02, TILE, "tile")
    ent("light", (53.0, 2.9, -32.0), color=[1.0, 0.95, 0.85], radius=8, intensity=3, group=31, on=True)
    # Stair core: ground (lobby door) to the roof hut (roof door).
    for k in range(RB["floors"] + 1):
        prefabs.append({"prefab": "stair_core_dogleg", "pos": [RB_CORE[0], k * STOREY, RB_CORE[1]], "prefix": "rb%d_" % k,
                        "override": {"fire_door": {"locked": 0 < k < RB["floors"]}}})
    box(44.0, 47.95, -42.15, -27.85, RB_ROOF + 3.0, RB_ROOF + 3.3, RC, "concrete")      # hut roof
    box(44.0, 47.95, -42.15, -27.85, RB_ROOF + 3.3, RB_ROOF + 3.5, [0.4, 0.4, 0.4], "metal_thin")
    # Roof: parapet, helipad.
    for (a0, a1, b0, b1) in ((x0, x1, z0, z0 + 0.3), (x0, x1, z1 - 0.3, z1), (x0, x0 + 0.3, z0, -42.15), (x0, x0 + 0.3, -27.85, z1), (x1 - 0.3, x1, z0, z1)):
        box(a0, a1, b0, b1, RB_ROOF, RB_ROOF + 1.05, [0.7, 0.68, 0.62], "concrete")
    hx, hz = HELIPAD
    box(hx - 7.0, hx + 7.0, hz - 7.0, hz + 7.0, RB_ROOF, RB_ROOF + 0.04, [0.22, 0.23, 0.24], "asphalt")
    for (a0, a1, b0, b1) in ((-6.6, 6.6, -6.6, -6.2), (-6.6, 6.6, 6.2, 6.6), (-6.6, -6.2, -6.2, 6.2), (6.2, 6.6, -6.2, 6.2),
                             (-2.4, -1.6, -3.0, 3.0), (1.6, 2.4, -3.0, 3.0), (-1.6, 1.6, -0.4, 0.4)):
        box(hx + a0, hx + a1, hz + b0, hz + b1, RB_ROOF + 0.04, RB_ROOF + 0.05, [0.95, 0.8, 0.15] if abs(a1 - a0) > 12 or abs(b1 - b0) > 12 else [0.92, 0.92, 0.9],
            "asphalt", collide=False, cast_shadows=False)
    for (ax, az) in ((51.0, -46.0), (64.0, -29.5), (50.0, -29.0)):                     # roof plant
        box(ax - 1.2, ax + 1.2, az - 0.8, az + 0.8, RB_ROOF, RB_ROOF + 1.4, [0.5, 0.52, 0.53], "metal_thin")
    point("heli_pad", hx, hz, 0, y=RB_ROOF + 0.05)
    point("heli_rescue_a", hx + math.sin(math.radians(60)) * 1100, hz - math.cos(math.radians(60)) * 1100, 0, y=160.0)
    volume("vol_heli", hx - 8, hx + 8, hz - 8, hz + 8, RB_ROOF - 0.5, RB_ROOF + 4.0)
    volume("vol_rb_roof", x0, x1, z0, z1, RB_ROOF - 0.5, RB_ROOF + 4.0)
    volume("vol_rb_lobby", lx0, lx1, lz0, z1, -0.5, 3.0)
    volume("vol_rb_stairs", 44.0, 47.95, -42.15, -27.85, 1.0, RB_ROOF - 0.5)

    # ------------------------------------------------------------ escape route volumes, zombie points
    volume("vol_lobby_ground", -8.35, 6.0, 0.3, 17.5, -0.5, 3.0)
    volume("vol_stair_low", -12.0, -8.5, 6.0, 20.0, -0.5, 16.0)
    volume("vol_street", -46.0, 70.0, -26.0, 0.0, -0.5, 4.0)
    volume("vol_street", 12.15, 44.0, -62.0, 34.0, -0.5, 4.0)
    # The corridor's zombies, one more almost every night (docs/CHAPTER_01.md "The hallway"): the first
    # ones right outside 10A's door (x 5.25), in the door viewer's view, the later ones along the corridor.
    for k, (x, z) in enumerate(((5.5, 13.6), (4.3, 13.3), (6.6, 13.5), (5.0, 12.9), (3.4, 13.6), (7.4, 13.0),
                                (2.2, 13.3), (8.6, 13.6), (0.6, 12.9), (-1.6, 13.5), (10.4, 13.0), (-4.0, 13.4))):
        point("hall_z%d" % (k + 1), x, z, rng.uniform(-180, 180), y=storey_y(10))
    for k, fl in enumerate((7, 4, 2)):                                                    # stairwell (the rescue morning)
        point("stair_z%d" % (k + 1), -10.6, 13.0 + k * 0.6, rng.uniform(-180, 180), y=storey_y(fl))
    for k, (x, z) in enumerate(((-5.0, 9.0), (3.5, 3.0))):
        point("lobby_z%d" % (k + 1), x, z, rng.uniform(-180, 180), y=0.0)
    STREET = ((-8.0, -12.0), (6.0, -18.0), (18.0, -11.0), (-24.0, -16.0), (30.0, -4.0), (33.0, -30.0), (28.0, 12.0),
              (41.0, -14.0), (60.0, -12.0), (-36.0, -12.0), (46.0, -23.5), (24.0, -40.0))
    for k, (x, z) in enumerate(STREET):
        point("street_z%d" % (k + 1), x, z, rng.uniform(-180, 180), y=0.0)
    for k, (x, z) in enumerate(((50.0, -33.0), (56.0, -29.0))):
        point("rb_z%d" % (k + 1), x, z, rng.uniform(-180, 180), y=0.0)

    # ------------------------------------------------------------ the damage layer, day by day
    car_cols = [[0.7, 0.7, 0.72], [0.1, 0.1, 0.11], [0.5, 0.1, 0.08], [0.18, 0.25, 0.4], [0.85, 0.85, 0.82], [0.35, 0.36, 0.38], [0.6, 0.55, 0.2]]
    burnt = [0.07, 0.065, 0.06]

    def car(day, x, z, yaw, color=None, collide=True, flipped=False):
        c = color or rng.choice(car_cols)
        if flipped:
            dmg(day, x, z, 1.8, 1.3, 4.4, c, yaw, collide=collide)
            dmg(day, x, z, 1.5, 0.5, 2.4, [cc * 0.5 for cc in c], yaw, y=1.3, collide=False)
        else:
            dmg(day, x, z, 1.8, 0.75, 4.4, c, yaw, y=0.25, collide=collide)
            dmg(day, x, z, 1.5, 0.55, 2.4, [cc * 0.7 for cc in c], yaw, y=1.0, collide=False)
            for sx in (-0.8, 0.8):
                for sz in (-1.4, 1.4):
                    a = math.radians(yaw)
                    dmg(day, x + sx * math.cos(a) + sz * math.sin(a), z - sx * math.sin(a) + sz * math.cos(a), 0.25, 0.6, 0.6, [0.05, 0.05, 0.05], yaw, collide=False)

    # Day 2: abandoned cars, doors open, where the traffic stopped.
    for (x, z, yaw) in ((-20.0, -11.5, 92), (-4.0, -18.5, -84), (14.0, -12.0, 101), (29.0, -40.0, 5), (35.0, 8.0, 176), (52.0, -11.8, 88)):
        car(2, x, z, yaw)
    # Day 3: a pile-up in the junction, a police car, barrier tape, the first rubbish.
    car(3, 29.0, -16.0, 40)
    car(3, 31.5, -13.2, 128, [0.12, 0.14, 0.25])
    car(3, 34.5, -18.5, 15, [0.92, 0.92, 0.92])
    for (x, z) in ((-12.0, -5.0), (20.0, -6.5), (40.0, -24.5)):
        dmg(3, x, z, 1.2, 0.7, 1.0, [0.2, 0.22, 0.2], rng.uniform(0, 90))
    # Day 4: burnt-out cars, an overturned van.
    car(4, -30.0, -18.0, 70, burnt)
    car(4, 8.0, -9.0, 160, burnt)
    car(4, 25.0, -24.0, 20, None, flipped=True)
    dmg(4, -16.0, -15.0, 3.2, 0.02, 4.0, [0.04, 0.04, 0.04], 30, collide=False)          # scorch on the road
    # Day 5: the bus jack-knifed across the north street in front of the tower (the direct crossing is gone).
    dmg(5, -2.0, -15.0, 2.6, 3.1, 12.0, [0.75, 0.62, 0.15], 68)
    dmg(5, 8.6, -12.0, 2.6, 3.1, 7.0, [0.7, 0.58, 0.14], 22)
    car(5, 14.0, -18.0, 120, burnt)
    car(5, -10.5, -19.5, 80)
    car(5, 19.0, -14.5, 10)
    # Day 6: an army truck, sandbags by Orchard House, a residents' barricade, more wrecks.
    dmg(6, 41.0, -10.0, 2.6, 3.2, 8.0, [0.25, 0.3, 0.2], 2)
    for k in range(6):
        dmg(6, 46.0 + k * 1.3, -23.2, 1.2, 0.8, 0.6, [0.55, 0.5, 0.38], 0)
    dmg(6, -40.0, -14.0, 3.0, 1.6, 6.0, [0.35, 0.28, 0.2], 45)                          # furniture pile
    car(6, 36.0, -45.0, 160, burnt)
    car(6, 23.0, 20.0, 80, burnt)
    # Day 7: the burnt bus shell, rubble from the north block, a police barricade line.
    dmg(7, 6.0, -24.5, 6.0, 1.2, 3.0, [0.4, 0.38, 0.35], 10)                            # rubble
    dmg(7, 0.0, -25.0, 3.0, 0.8, 2.0, [0.45, 0.42, 0.38], -20)
    car(7, -24.0, -11.0, 100, burnt)
    for k in range(4):
        dmg(7, 58.0 + k * 2.6, -9.0, 2.4, 1.0, 0.5, [0.85, 0.85, 0.85], 0)
    # Burnt-out floors (visual only): window bays blackened, soot streaks above them. Fire climbed the
    # north block, then the west one, then the low-rise across the east street.
    def burnt(day, a0, a1, face, normal, y0, y1, along_x):
        bay = 3.0
        a = a0 + 0.5
        while a + 2.0 <= a1:
            y = y0
            while y + 3.0 <= y1 + 0.01:
                if rng.random() < 0.72:
                    cx = a + 1.0
                    off = face + normal * 0.08
                    w, d = (1.8, 0.06) if along_x else (0.06, 1.8)
                    x, z = (cx, off) if along_x else (off, cx)
                    dmg(day, x, z, w, 1.6, d, [0.035, 0.03, 0.028], 0, y=y + 0.8, collide=False)
                    off2 = face + normal * 0.06
                    x2, z2 = (cx, off2) if along_x else (off2, cx)
                    w2, d2 = (1.4, 0.04) if along_x else (0.04, 1.4)
                    dmg(day, x2, z2, w2, rng.uniform(0.8, 1.6), d2, [0.1, 0.095, 0.09], 0, y=y + 2.4, collide=False)
                y += 3.0
            a += bay

    burnt(4, -2.0, 10.0, -26.0, -1, 6.0, 15.0, True)
    burnt(5, -50.0, -30.0, -26.0, -1, 3.0, 12.0, True)
    burnt(6, 10.0, 22.0, -26.0, -1, 9.0, 15.0, True)
    burnt(6, 4.0, 20.0, -16.0, 1, 6.0, 18.0, False)
    burnt(7, 44.0, 72.0, -4.0, -1, 3.0, 12.0, True)
    return damage
