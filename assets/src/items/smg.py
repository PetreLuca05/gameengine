"""SMG (data/weapons/07_smg.json): 9 mm AR-pattern carbine with a short M-LOK handguard, blowback (no gas
system), thread protector, telescoping two-rod PDW stock, straight 30-round 9 mm stick magazine.
Procedural (path b): _ar.py, dimensions from the JSON (sight line 0.104, muzzle -0.33, slots)."""
import _ar


def build(ctx):
    return _ar.AR(ctx.weapon, mag="9mm", mag_len=0.034, mag_drop=0.19, stock="pdw", device="thread", gas=False, forward_assist=False, barrel_r=0.0095,
                  marks=("DEADGROUND ARMS", "DG-9  CAL 9x19"), serial="SN DG9001388").build("smg")
