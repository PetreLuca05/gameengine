"""Dual-tube night vision goggles (data/items.json "nvg_dual"): two tubes on a bridge with the IPD hinge.
Procedural (path b): _nvg.py."""
import _nvg


def build(ctx):
    return _nvg.nvg("nvg_dual", dual=True, label="NV-31")
