# Ground match report

Written by `tools/ground_match.py report`. Our captures (`tools/tests/ground_match.cfg`, the presets of `docs/reference/ground/views.json`) against the reference regions (`tools/ground_ref.py`). Values are lit, graded frames: hue and saturation first (exposure-independent), value informative. Tolerances: hue 8 deg, saturation 0.08, value 0.12.

| View | Region | Reference HSV | Ours HSV | d hue | d sat | d val | |
|---|---|---|---|---|---|---|---|
| r1 | r1_blade_green_near | 66 0.48 0.42 | 76 0.37 0.38 | 10 | -0.11 | -0.04 | off |
| r1 | r1_ground_under | 54 0.57 0.31 | 53 0.41 0.32 | 1 | -0.16 | +0.01 | off |
| r1 | r1_field_far | 58 0.43 0.67 | 59 0.31 0.39 | 1 | -0.11 | -0.28 | off (value) |
| r1 | r1_blade_dry_right | 48 0.36 0.62 | 50 0.38 0.52 | 2 | +0.02 | -0.10 | ok |
| r2 | r2_wheat_ears | 36 0.41 0.57 | 36 0.29 0.55 | 0 | -0.12 | -0.02 | off |
| r2 | r2_wheat_ground | 31 0.55 0.28 | 32 0.39 0.34 | 1 | -0.16 | +0.06 | off |
| r3 | r3_grass_mid | 59 0.48 0.31 | 65 0.37 0.46 | 6 | -0.11 | +0.15 | off (value) |
| r3 | r3_pale_ground | 53 0.28 0.37 | 61 0.31 0.46 | 8 | +0.04 | +0.09 | ok |
| r4 | r4_grass_hill | 50 0.61 0.35 | 52 0.43 0.45 | 2 | -0.18 | +0.10 | off |
| r4 | r4_dirt_patch | 41 0.36 0.44 | 53 0.41 0.45 | 12 | +0.04 | +0.01 | off |
| r4 | r4_moss_ground | 50 0.62 0.46 | 53 0.42 0.44 | 3 | -0.20 | -0.02 | off |
| r5 | r5_meadow | 44 0.46 0.38 | 53 0.28 0.47 | 9 | -0.19 | +0.10 | off |
| r5 | r5_path | 46 0.35 0.38 | 50 0.32 0.44 | 3 | -0.03 | +0.06 | ok |
| r5 | r5_ground_moss | 39 0.46 0.47 | 53 0.21 0.50 | 14 | -0.25 | +0.04 | off |

11 region(s) outside the hue / saturation tolerance.

Side by side: `compare_<view>.jpg` (reference left, ours right, regions outlined). Blade and terrain sheets: `blade_compare_*.png`, `terrain_compare.png` (`py tools/ground_match.py blades|terrain`). Texture checks: `gen_report.md`, `py tools/tests/ground_tex_check.py`. Grass density / height against the spec: `test_grass`.
