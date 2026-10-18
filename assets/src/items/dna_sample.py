"""DNA sample (data/items.json "dna_sample", docs/REVIVE.md): a screw-cap sample tube of blood with a
barcode label, the cap a separate part. Held upright in the fist, placed like the old held box.
Procedural (path b)."""
import item_lib as L

R = 0.0085


def build(ctx):
    it = L.Item("dna_sample", "item")
    tube = L.lathe([(0.0, 0.0), (R * 0.45, 0.0005), (R * 0.85, 0.004), (R, 0.009), (R, 0.068), (R - 0.0012, 0.068), (R - 0.0012, 0.0095), (R * 0.85 - 0.0012, 0.0052),
                    (0.0, 0.0018)], (0, 0, 0), (0, 1, 0), 40, "plastic_clear")
    thread = L.merge(*[L.torus((0, 0.0625 + k * 0.0016, 0), (0, 1, 0), R + 0.0002, 0.0003, 40, 6, "plastic_clear") for k in range(3)])
    blood = L.lathe([(0, 0.0018), (R * 0.85 - 0.0013, 0.0053), (R - 0.0013, 0.0097), (R - 0.0013, 0.046), (0, 0.046)], (0, 0, 0), (0, 1, 0), 32, "liquid_red")
    label = L.ring_lathe([(R + 0.00005, 0), (R + 0.0002, 0), (R + 0.0002, 0.03), (R + 0.00005, 0.03)], (0, 0.022, 0), (0, 1, 0), 40, "paper_label")
    bars = [L.box((0.0, 0.03 + 0.0001, -R - 0.00025), (0.0003 + 0.0002 * (k % 3), 0.0055, 0.00008), 0, 0, "label_black").rotate((0, 1, 0), (k - 7) * 4.2)
            for k in range(15)]
    txt = [L.text("DG-BIO  0427", (0.0, 0.043, -R - 0.0003), 0.0022, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"),
           L.text("BIOHAZARD", (0.0, 0.0255, -R - 0.0003), 0.0019, (0, 0, -1), (0, 1, 0), 0.0001, "label_red")]
    cap = L.lathe([(0, 0), (R + 0.0016, 0), (R + 0.0018, 0.002), (R + 0.0018, 0.012), (R + 0.001, 0.0135), (0, 0.0135)], (0, 0.061, 0), (0, 1, 0), 48, "plastic_red")
    cap.cut(L.knurl((0, 0.0625, 0), (0, 0.0725, 0), R + 0.0018, 36, 0.0004))
    it.part("tube").add(tube, thread, blood, label, *bars, *txt)
    it.part("cap", mech="cap").add(cap)
    it.socket("grip_r", (0, 0, 0))
    it.socket("cap", (0, 0.0745, 0))
    it.hold_small()
    return it
