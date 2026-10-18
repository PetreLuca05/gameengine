"""Red key (data/items.json "key_red"): two keys on a split ring with a red rubber head and a red
"STORAGE A" tag. Palm-held. Procedural (path b): _keys.py."""
import _keys


def build(ctx):
    return _keys.keyring("key_red", "plastic_red", "STORAGE A", (2, 4, 1, 3, 2, 4), (3, 1, 2, 4, 1, 2))
