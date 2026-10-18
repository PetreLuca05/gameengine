"""DMR (data/weapons/09_dmr.json): AR-10 pattern 7.62 marksman rifle, FDE cerakote receivers, heavy
20" barrel with an integral two-port brake, fixed precision stock with an adjustable cheek riser,
20-round straight 7.62 magazine. Procedural (path b): _ar.py, dimensions from the JSON."""
import _ar


def build(ctx):
    return _ar.AR(ctx.weapon, finish="aluminium_fde", mag="762", mag_len=0.07, mag_drop=0.135, stock="precision", device="brake", barrel_r=0.0118,
                  receiver_len=0.012, marks=("DEADGROUND ARMS", "DG-10  CAL 7.62x51"), serial="SN DG1000561").build("dmr")
