# Ground texture generation report

Written by `tools/vfxgen/groundgen.py` (tool v2). Images generated this run: 0, spent $0.00.

| Recipe | Kind | Source | Checks |
|---|---|---|---|
| fern | blade_atlas | generated | ok; root/mid/tip -/#576734/#5f7139 · #45532c/#546433/#61723c |
| grass_broad | blade_atlas | generated | ok; root/mid/tip #545332/#5d6435/#6f7340 · #515434/#5c6435/#6f7340 · #4d5332/#5e6537/- · #545237/#616335/#727240 |
| grass_short | blade_atlas | generated | ok; root/mid/tip #5c5b3f/#68653e/#72714d · #5a5834/#66633c/- · #585733/#6a6841/- · #5a5a36/-/- |
| grass_tall | blade_atlas | generated | ok; root/mid/tip #565535/#62613c/#65633b · #51512f/#5a5a31/#6e6b41 · #50502f/#5c5c35/- · #4f4f2f/#61613d/#656339 |
| grass_tussock | blade_atlas | generated | ok; root/mid/tip #67674e/#76744c/#817d55 · #696b48/#707046/#7f7c55 · #656544/#76754d/#83815a · #565a40/#66694a/#787554 |
| reed | blade_atlas | generated | ok; root/mid/tip #515334/#5e623a/- · #535536/#5f623a/#737048 |
| shrub_leafy | blade_atlas | generated | 1 failing cell check(s); root/mid/tip #505b33/#566a3a/#62763f · #4d5c30/#5a6c3d/#677843 |
| wheat | blade_atlas | generated | ok; root/mid/tip #73724c/#807e51/- · -/-/- |
| dirt | terrain_layer | generated | mean #635a45 (target #5e5540, max delta 0.02), seam 0.8135 |
| dirt_dry | terrain_layer | generated | mean #8e8361 (target #8c7f5c, max delta 0.02), seam 0.9477 |
| field_soil | terrain_layer | generated | mean #4c3926 (target #4d3a26, max delta 0.0032), seam 0.7054 |
| field_straw | terrain_layer | generated | mean #6c583a (target #6e5a3a, max delta 0.0093), seam 0.8447 |
| forest_floor | terrain_layer | generated | mean #4c3e2d (target #4a3d2c, max delta 0.0059), seam 0.9693 |
| forest_needles | terrain_layer | generated | mean #5c4630 (target #5a4530, max delta 0.0075), seam 0.8574 |
| grass_sparse_on_dirt | transition | generated | mean #645d3d (target #5f5838, max delta 0.02), seam 0.7349 |
| gravel | terrain_layer | generated | mean #777169 (target #77716a, max delta 0.0027), seam 1.0724 |
| ground_litter | terrain_layer | generated | mean #514b2c (target #524c2c, max delta 0.0048), seam 0.6426 |
| ground_litter_dry | terrain_layer | generated | mean #736945 (target #6e6440, max delta 0.02), seam 0.8037 |
| ground_moss | terrain_layer | generated | mean #6c6437 (target #6b6236, max delta 0.0095), seam 0.8771 |
| ground_moss_dry | terrain_layer | generated | mean #7a6e2f (target #75692f, max delta 0.02), seam 0.8466 |
| meadow_ground | terrain_layer | generated | mean #6f653b (target #6b6236, max delta 0.02), seam 0.8209 |
| mud | terrain_layer | generated | mean #3e3426 (target #3d3326, max delta 0.0042), seam 0.6556 |
| mud_wet_edge | transition | generated | mean #493e2e (target #4a3f2e, max delta 0.0053), seam 0.8711 |
| pale_soil | terrain_layer | generated | mean #7c7861 (target #77735c, max delta 0.02), seam 0.8424 |
| path | terrain_layer | generated | mean #6f664f (target #6a614a, max delta 0.02), seam 0.829 |
| path_worn_edge | transition | generated | mean #665f45 (target #625a40, max delta 0.02), seam 0.8496 |
| pebble_band | transition | generated | mean #716a5b (target #6e685a, max delta 0.011), seam 0.8736 |
| rock | terrain_layer | generated | mean #7b766d (target #7a756b, max delta 0.0075), seam 0.8506 |
| rock_moss_edge | transition | generated | mean #696551 (target #6a6650, max delta 0.0051), seam 0.7956 |
| rock_mossy | terrain_layer | generated | mean #72725a (target #6f6f55, max delta 0.02), seam 0.7387 |
| sand | terrain_layer | generated | mean #aa9d7d (target #a59a7a, max delta 0.0194), seam 0.8968 |
| sand_on_rock | transition | generated | mean #938974 (target #8e8672, max delta 0.02), seam 0.6744 |
| scatter_ground | scatter_atlas | generated | ok |

Contact sheets: `gen_sheet_foliage.png`, `gen_sheet_terrain.png`.
