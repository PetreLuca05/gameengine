"""Extended magazine (assets/attachments/extended_mag.json), the rifle's well: 40-round 5.56 polymer
magazine with a +5 floorplate extension and pull loop. Procedural (path b). Variants for the other wells:
att_extended_mag_smg (9 mm), att_extended_mag_dmr (7.62) ("mesh_for")."""
import _extmag


def build(ctx):
    return _extmag.build_mag("att_extended_mag", 0.0644, 0.245, 0.0112, 0.03, "polymer_matte", label="40")
