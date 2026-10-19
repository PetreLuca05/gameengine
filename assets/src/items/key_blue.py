"""Blue key (data/items.json "key_blue"): two keys on a split ring with a blue rubber head and a blue
"STORAGE B" tag. Palm-held. Procedural (path b): _keys.py."""
import _keys


def build(ctx):
    return _keys.keyring("key_blue", "plastic_blue", "STORAGE B", (4, 2, 3, 1, 4, 2), (1, 3, 4, 2, 3, 1))
