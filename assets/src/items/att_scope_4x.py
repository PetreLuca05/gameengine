"""4x scope (assets/attachments/scope_4x.json): 4x32 fixed-power riflescope on a two-ring Picatinny base.
Procedural (path b): _scope.py; the lens planes, sight socket and objective come from the JSON."""
import _scope


def build(ctx):
    return _scope.scope("att_scope_4x", ctx.attachment, obj_r=0.0225, ring_z=(-0.055, 0.055), base_half=0.075, label="4X32")
