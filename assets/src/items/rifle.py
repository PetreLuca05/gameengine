"""Assault rifle (data/weapons/02_rifle.json): AR-15 pattern carbine, 14.5" barrel, free-float M-LOK
handguard, flip-up irons (the rear hides under an optic, the front folds), A2 flash hider, collapsible
stock on a carbine buffer tube, tan 30-round polymer magazine. Procedural (path b): _ar.py, dimensions
from the JSON (bore 0.05, rail top 0.085, sight line 0.112, muzzle -0.53, slots)."""
import _ar


def build(ctx):
    return _ar.AR(ctx.weapon, finish="aluminium_anodized", mag="stanag", stock="carbine", device="a2").build("rifle")
