"""Thermal scope (assets/attachments/scope_thermal.json): a microbolometer riflescope on the riflescope
body (_scope.py: the lens planes, sight socket and objective from the JSON), with a germanium objective
(dark, no glass glint), the battery tube (one AA) on the right, and the control buttons on the saddle.
Procedural (path b)."""
import _scope
import item_lib as L


def build(ctx):
    j = ctx.attachment
    it = _scope.scope("att_scope_thermal", j, tube_r=0.016, obj_r=0.026, ring_z=(-0.05, 0.05), base_half=0.07, label="THERMAL")
    ax = j["lens"]["pos"][1]
    oz = j["lens"]["objective"][2]
    extra = [L.cyl((0, ax, oz + 0.003), (0, ax, oz + 0.005), 0.0235, 48, "steel_dark", bevel=0),   # germanium window
             L.cyl((0.026, ax, -0.03), (0.026, ax, 0.035), 0.0085, 32, "aluminium_anodized"),      # battery tube
             L.cyl((0.026, ax, 0.035), (0.026, ax, 0.041), 0.0092, 32, "rubber")]                  # its cap
    for i, z in enumerate((-0.01, 0.0, 0.01)):
        extra.append(L.cyl((0, ax + 0.019, z), (0, ax + 0.023, z), 0.0035, 16, "rubber"))          # buttons
    it.part("thermal").add(*extra)
    return it
