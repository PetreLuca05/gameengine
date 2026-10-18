"""8x scope (assets/attachments/scope_8x.json): 8x50 precision scope with an illuminated mil-dot reticle:
large objective bell, side focus with the illumination dial, two rings on a Picatinny base.
Procedural (path b): _scope.py; the lens planes, sight socket and objective come from the JSON."""
import _scope


def build(ctx):
    return _scope.scope("att_scope_8x", ctx.attachment, obj_r=0.0285, ring_z=(-0.065, 0.065), base_half=0.085, side_knob=True, label="8X50 IR")
