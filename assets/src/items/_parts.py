"""Shared weapon / item sub-assemblies for the item scripts (docs/ITEM_PIPELINE.md).
Engine item space: metres, origin = grip_r, +Y up, -Z forward, +X right."""
import math

import item_lib as L
from mathutils import Vector


def ar_pistol_grip(top_z0=-0.016, top_z1=0.024, top_y=0.004, bot_y=-0.078, rake=0.022, half_w=0.0145, mat="polymer_matte"):
    """AR-style pistol grip raked back: finger groove on the front strap, beavertail, stippled side panels (grooves)."""
    fz0, fz1 = top_z0, top_z0 + rake
    rz0, rz1 = top_z1, top_z1 + rake * 1.05
    # Side silhouette (z, y), CCW seen from +X: front strap with a finger swell, rear strap with a beavertail.
    pts = [(fz0, top_y), (fz0 + 0.004, top_y - 0.012), (fz0 + 0.0025, top_y - 0.022), (fz0 + 0.008, top_y - 0.032), (fz0 + 0.011, top_y - 0.045),
           (fz0 + 0.0135, top_y - 0.058), (fz1 + 0.004, bot_y + 0.004), (fz1 + 0.009, bot_y), (rz1 - 0.004, bot_y), (rz1 + 0.002, bot_y + 0.006),
           (rz1 - 0.003, top_y - 0.04), (rz0 + 0.004, top_y - 0.014), (rz0 + 0.012, top_y - 0.004), (rz0 + 0.012, top_y + 0.002), (rz0 + 0.002, top_y + 0.004)]
    pts = [(p[0], p[1]) for p in reversed(pts)]
    g = L.side_prism(pts, -half_w, half_w, bevel=0.0035, segs=3, mat=mat, angle=20)
    # Grip panel texture: horizontal grooves on both sides (stippling stand-in at item scale).
    cuts = []
    for k in range(9):
        y = top_y - 0.018 - k * 0.0055
        t = (top_y - y) / (top_y - bot_y)
        zc = (fz0 + rz0) * 0.5 + rake * t + 0.002
        for sx in (1, -1):
            cuts.append(L.box((sx * (half_w + 0.0003), y, zc), (0.0007, 0.0011, 0.0105), 0, 0, mat))
    g.cut(L.merge(*cuts))
    # Bottom cap with a storage-compartment seam.
    cap = L.side_prism([(fz1 + 0.009, bot_y), (rz1 - 0.004, bot_y), (rz1 - 0.003, bot_y - 0.004), (fz1 + 0.01, bot_y - 0.004)], -half_w + 0.001,
                       half_w - 0.001, bevel=0.0012, segs=2, mat=mat)
    return g, cap


def ar_trigger(pivot, mat="steel_dark"):
    """Curved trigger blade below pivot (side profile), 6 mm wide."""
    px, py, pz = pivot
    pts = [(pz - 0.003, py + 0.002), (pz + 0.004, py + 0.002), (pz + 0.0035, py - 0.008), (pz + 0.001, py - 0.016), (pz - 0.0015, py - 0.022),
           (pz - 0.005, py - 0.025), (pz - 0.0065, py - 0.0235), (pz - 0.004, py - 0.016), (pz - 0.0035, py - 0.006)]
    return L.side_prism(list(reversed(pts)), px - 0.003, px + 0.003, bevel=0.0008, segs=2, mat=mat, angle=25)


def flip_front_sight(z, rail_top, line, mat="steel_parkerized"):
    """Flip-up front sight standing on a rail at z: clamp base, wings, threaded post with its top on line.
    Returns (base solids, standing assembly solids, hinge (pivot)); the standing part folds about +X at the hinge."""
    base = [L.box((0, rail_top + 0.004, z), (0.0118, 0.004, 0.009), 0.0008, 2, mat),
            L.screw((0.0118, rail_top + 0.004, z), (1, 0, 0), 0.0022, 0.0009, slot=False, hexsock=True)]
    hinge = Vector((0, rail_top + 0.009, z + 0.006))
    h = line - hinge.y
    # The leaf: a frame (wings) around the post.
    leaf_pts = [(-0.0105, 0.0), (0.0105, 0.0), (0.0105, h * 0.55), (0.0095, h * 0.92), (0.0075, h + 0.003), (0.0055, h + 0.003),
                (0.0065, h * 0.6), (-0.0065, h * 0.6), (-0.0055, h + 0.003), (-0.0075, h + 0.003), (-0.0095, h * 0.92), (-0.0105, h * 0.55)]
    leaf = L.front_prism([(p[0], hinge.y + p[1]) for p in leaf_pts], z - 0.0025, z + 0.0035, bevel=0.0006, segs=2, mat=mat)
    leaf.cut(L.box((0, hinge.y + h * 0.42, z + 0.0005), (0.0035, 0.0028, 0.006), 0, 0, mat))
    post = L.cyl((0, hinge.y + h * 0.55, z), (0, line - 0.0016, z), 0.0013, 12, mat, bevel=0.0002)
    post_top = L.box((0, line - 0.0016, z), (0.0013, 0.0016, 0.0009), 0.0002, 1, mat)
    detent = L.cyl((0, hinge.y + h * 0.55, z), (0, hinge.y + h * 0.55 - 0.003, z), 0.0026, 16, mat, bevel=0.0003)
    hinge_pin = L.pin((0, hinge.y, z + 0.0005), (1, 0, 0), 0.0016, 0.024, "steel_dark")
    return base, [leaf, post, post_top, detent, hinge_pin], hinge


def flip_rear_sight(z, rail_top, line, mat="steel_parkerized"):
    """Flip-up dual-aperture rear sight on a rail at z: base with windage knob, leaf with the aperture centred on line."""
    base = [L.box((0, rail_top + 0.005, z + 0.002), (0.0115, 0.005, 0.0135), 0.0008, 2, mat),
            L.cyl((0.0115, rail_top + 0.006, z + 0.006), (0.0175, rail_top + 0.006, z + 0.006), 0.0045, 20, "steel_parkerized", bevel=0.0004)]
    base[1].cut(L.knurl((0.0118, rail_top + 0.006, z + 0.006), (0.0175, rail_top + 0.006, z + 0.006), 0.0045, 20, 0.00035))
    hinge = Vector((0, rail_top + 0.01, z + 0.006))
    h = line - hinge.y
    r_ap = 0.0035
    leaf_pts = [(-0.0085, 0.0), (0.0085, 0.0), (0.0085, h + 0.004), (0.0065, h + r_ap + 0.004), (-0.0065, h + r_ap + 0.004), (-0.0085, h + 0.004)]
    leaf = L.front_prism([(p[0], hinge.y + p[1]) for p in leaf_pts], z - 0.0012, z + 0.0018, bevel=0.0005, segs=2, mat=mat)
    leaf.cut(L.cyl((0, line, z - 0.004), (0, line, z + 0.004), 0.0018, 20, mat, bevel=0), )
    ring = L.ring_lathe([(0.0018, 0), (r_ap, 0), (r_ap, 0.003), (0.0018, 0.003)], (0, line, z + 0.0018), (0, 0, -1), 24, mat)
    return base, [leaf, ring], hinge


def buffer_tube(z0, z1, y, r=0.0147, mat="aluminium_anodized", positions=6):
    """Carbine buffer tube with castle nut, end plate (QD sling socket) and position holes underneath.
    Returns (tube, castle nut, end plate, [position z ...])."""
    tube = L.cyl((0, y, z0), (0, y, z1), r, 40, mat, bevel=0.0008)
    holes = []
    pz = []
    for k in range(positions):
        zz = z1 - 0.012 - k * 0.0175
        pz.append(zz)
        holes.append(L.cyl((0, y - r - 0.002, zz), (0, y - r + 0.003, zz), 0.0022, 12, mat, bevel=0))
    rail = L.box((0, y - r + 0.0005, (z0 + z1) * 0.5 + 0.01), (0.0045, 0.0025, (z1 - z0) * 0.5 - 0.012), 0.0006, 1, mat)
    tube.union(rail)
    tube.cut(L.merge(*holes))
    nut = L.ring_lathe([(r, 0), (r + 0.0045, 0), (r + 0.0045, 0.008), (r, 0.008)], (0, y, z0 + 0.002), (0, 0, 1), 40, "steel_parkerized", bevel=0.0005)
    notches = [L.box((math.cos(a) * (r + 0.004), y + math.sin(a) * (r + 0.004), z0 + 0.0075), (0.0018, 0.0018, 0.003), 0, 0) for a in
               [k * math.pi / 3 + 0.3 for k in range(6)]]
    for n in notches:
        n.rotate((0, 0, 1), 0, (0, y, z0))
    nut.cut(L.merge(*notches))
    plate = L.front_prism(L.rounded_rect(0.034, 0.05, 0.006, 3, 0, y - 0.004), z0 - 0.001, z0 + 0.002, bevel=0.0005, segs=1, mat="steel_parkerized")
    qd = L.tube((0.0125, y - 0.018, z0 + 0.0005), (0.0205, y - 0.018, z0 + 0.0005), 0.0055, 0.0038, 20, "steel_parkerized", bevel=0.0004)
    return tube, nut, L.merge(plate, qd), pz


def stanag_mag(top_y, bot_y, z_front, z_back, half_w=0.0112, curve=0.022, mat="polymer_tan", windows=True):
    """Curved 30-round polymer magazine (side profile bends forward with depth): body with ribs and a
    floorplate. Returns (body, floorplate, follower, top round case, top round bullet) in item space;
    the feed lips are at top_y."""
    n = 10
    front, back = [], []
    for k in range(n + 1):
        t = k / n
        y = top_y + (bot_y - top_y) * t
        dz = -curve * (t ** 1.6)
        front.append((z_front + dz, y))
        back.append((z_back + dz - 0.003 * t, y))
    pts = [front[0]] + front[1:] + list(reversed(back))
    pts = list(reversed(pts))
    body = L.side_prism(pts, -half_w, half_w, bevel=0.0018, segs=2, mat=mat, angle=20)
    # Lower half widens slightly (texture band / grip ridges): horizontal ribs on both sides.
    cuts = []
    for k in range(6):
        t = 0.45 + k * 0.08
        y = top_y + (bot_y - top_y) * t
        dz = -curve * (t ** 1.6)
        for sx in (1, -1):
            cuts.append(L.box((sx * half_w, y, (z_front + z_back) * 0.5 + dz), (0.0006, 0.0012, (z_back - z_front) * 0.42), 0, 0, mat))
    if windows:   # round-count window on the left
        for k in range(3):
            t = 0.12 + k * 0.1
            y = top_y + (bot_y - top_y) * t
            cuts.append(L.box((-half_w, y, z_back - 0.012 - curve * (t ** 1.6)), (0.0008, 0.004, 0.003), 0, 0, mat))
    body.cut(L.merge(*cuts))
    fz = z_front - curve
    plate = L.side_prism([(fz - 0.004, bot_y + 0.001), (z_back - curve + 0.004, bot_y + 0.001), (z_back - curve + 0.003, bot_y - 0.006),
                          (fz - 0.002, bot_y - 0.007)][::-1], -half_w - 0.0015, half_w + 0.0015, bevel=0.0015, segs=2, mat=mat)
    follower = L.box((0, top_y - 0.006, (z_front + z_back) * 0.5 + 0.002), (half_w - 0.0025, 0.002, (z_back - z_front) * 0.42), 0.0006, 1, "polymer_od")
    case = L.lathe([(0.0, 0.0), (0.0046, 0.0), (0.0048, 0.0012), (0.0048, 0.034), (0.0044, 0.037), (0.0031, 0.039), (0.0029, 0.0445)],
                   (0, top_y - 0.0004, z_back - 0.003), (0, 0, -1), 20, "brass")
    bullet = L.lathe([(0.0028, 0.0), (0.0028, 0.006), (0.0024, 0.012), (0.0012, 0.017), (0.0, 0.0195)], (0, top_y - 0.0004, z_back - 0.003 - 0.0445),
                     (0, 0, -1), 20, "copper")
    return body, plate, follower, case, bullet
