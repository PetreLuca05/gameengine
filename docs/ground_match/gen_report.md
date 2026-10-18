# Ground texture generation report

Written by `tools/vfxgen/groundgen.py` (tool v1). Images generated this run: 58, spent $10.90.

| Recipe | Kind | Source | Checks |
|---|---|---|---|
| fern | blade_atlas | generated | ok; root/mid/tip #625017/#697d3b/#6a7f34 · #697333/#9a9c4a/#8f923c |
| grass_broad | blade_atlas | generated | ok; root/mid/tip #8e8749/#5d6d34/#6a6e3e · #5f6525/#5d682a/#697227 · #546133/#727a4a/#85953a · #867933/#877e2f/#89811d |
| grass_short | blade_atlas | generated | 1 failing cell check(s); root/mid/tip #6c603a/#7d6f3f/#848647 · #655f2d/#6c642e/#796f31 · #696139/#797749/#867e4e · #6b6126/#788f0d/#766e1d |
| grass_tall | blade_atlas | generated | ok; root/mid/tip #5e552b/#636c31/#5f5524 · #5a542b/#637035/#76a441 · #594f29/#605d31/#6a6c37 · #686236/#757c4d/#898d69 |
| grass_tussock | blade_atlas | generated | ok; root/mid/tip #524b17/#67691b/#77741c · #616a31/#727230/#7a7b3e · #605d2b/#676b2c/#8b7e2c · #3f3a17/#333c26/#45573a |
| reed | blade_atlas | generated | ok; root/mid/tip #726b3c/#6c6d43/#9aae57 · #686236/#69673c/#86834d |
| shrub_leafy | blade_atlas | generated | 1 failing cell check(s); root/mid/tip #5c642f/#5e712d/#627823 · #586b39/#668b45/#77a455 |
| wheat | blade_atlas | generated | ok; root/mid/tip #8a8a44/#968f52/#9c9d5a · #84832c/#979c4e/#9fa45a |
| dirt | terrain_layer | generated | mean #645b47 (target #5e5540, max delta 0.028), seam 0.8133 |
| dirt_dry | terrain_layer | generated | mean #8e8365 (target #8c7f5c, max delta 0.0365), seam 0.9477 |
| field_soil | terrain_layer | generated | mean #4c3926 (target #4d3a26, max delta 0.0034), seam 0.7055 |
| field_straw | terrain_layer | generated | mean #6b583a (target #6e5a3a, max delta 0.0121), seam 0.8446 |
| forest_floor | terrain_layer | generated | mean #4c3e2d (target #4a3d2c, max delta 0.0059), seam 0.9693 |
| forest_needles | terrain_layer | generated | mean #5c4630 (target #5a4530, max delta 0.0075), seam 0.8574 |
| grass_sparse_on_dirt | transition | generated | mean #645d40 (target #5f5838, max delta 0.0326), seam 0.7348 |
| gravel | terrain_layer | generated | mean #777169 (target #77716a, max delta 0.0027), seam 1.0724 |
| ground_litter | terrain_layer | generated | mean #514b2c (target #524c2c, max delta 0.0048), seam 0.6426 |
| ground_litter_dry | terrain_layer | generated | mean #756a47 (target #6e6440, max delta 0.0271), seam 0.8038 |
| ground_moss | terrain_layer | generated | mean #6c6437 (target #6b6236, max delta 0.0095), seam 0.8771 |
| ground_moss_dry | terrain_layer | generated | mean #7f6e2f (target #75692f, max delta 0.0385), seam 0.8472 |
| meadow_ground | terrain_layer | generated | mean #6f653d (target #6b6236, max delta 0.0278), seam 0.821 |
| mud | terrain_layer | generated | mean #3e3426 (target #3d3326, max delta 0.0042), seam 0.6556 |
| mud_wet_edge | transition | generated | mean #493e2e (target #4a3f2e, max delta 0.0054), seam 0.8711 |
| pale_soil | terrain_layer | generated | mean #7c7964 (target #77735c, max delta 0.0306), seam 0.8424 |
| path | terrain_layer | generated | mean #706853 (target #6a614a, max delta 0.0369), seam 0.8294 |
| path_worn_edge | transition | generated | mean #666048 (target #625a40, max delta 0.0307), seam 0.8497 |
| pebble_band | transition | generated | mean #716a5b (target #6e685a, max delta 0.011), seam 0.8736 |
| rock | terrain_layer | generated | mean #7b766d (target #7a756b, max delta 0.0075), seam 0.8506 |
| rock_moss_edge | transition | generated | mean #696551 (target #6a6650, max delta 0.0051), seam 0.7956 |
| rock_mossy | terrain_layer | generated | mean #72725b (target #6f6f55, max delta 0.0217), seam 0.7386 |
| sand | terrain_layer | generated | mean #aa9d7d (target #a59a7a, max delta 0.0194), seam 0.8968 |
| sand_on_rock | transition | generated | mean #938974 (target #8e8672, max delta 0.02), seam 0.6744 |
| scatter_ground | scatter_atlas | generated | ok |

Contact sheets: `gen_sheet_foliage.png`, `gen_sheet_terrain.png`.
