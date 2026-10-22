"""Night vision goggles, white phosphor (data/items.json "nvg_white"). Procedural (path b): _nvg.py."""
import _nvg


def build(ctx):
    return _nvg.nvg("nvg_white", white=True, label="NV-14W")
