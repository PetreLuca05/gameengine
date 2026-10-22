"""Night vision goggles, green phosphor (data/items.json "nvg"): a PVS-14 style monocular. Procedural (path b): _nvg.py."""
import _nvg


def build(ctx):
    return _nvg.nvg("nvg", label="NV-14")
