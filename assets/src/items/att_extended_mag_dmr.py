"""Extended magazine, DMR well (assets/attachments/extended_mag.json "mesh_for" dmr): 25-round 7.62
polymer magazine with a floorplate extension. Procedural (path b)."""
import _extmag


def build(ctx):
    return _extmag.build_mag("att_extended_mag_dmr", 0.07, 0.17, 0.0118, 0.012, "polymer_tan", window=False, label="25")
