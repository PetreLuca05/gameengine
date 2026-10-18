"""Extended magazine, SMG well (assets/attachments/extended_mag.json "mesh_for" smg): 50-round 9 mm stick
with a floorplate extension. Procedural (path b)."""
import _extmag


def build(ctx):
    return _extmag.build_mag("att_extended_mag_smg", 0.034, 0.27, 0.0105, 0.006, "polymer_matte", label="50")
