# Credits (third-party assets)

Third-party code libraries and their licences are listed next to their use (`third_party/VERSIONS.txt`,
docs/PHONE.md, docs/UI.md for the font). This file lists third-party art.

## Tree models: Tree It (`assets/treefbx`)

- **What:** the tree, bush and flower meshes (`*.fbx`, `*.obj`, `*.mtl`) and their bark / leaf textures:
  beech, birch, elm, fir, juniper, maple, oak, old, pine, spruce, bamboo, daisy, tree1 (acacia).
- **Source:** exports from Tree It by Evolved Software, <https://www.evolved-software.com/treeit/treeit>
  (<https://www.evolved-software.com>).
- **Licence:** Creative Commons Attribution 4.0 International (CC BY 4.0),
  <https://creativecommons.org/licenses/by/4.0/>.
- **Modified:** converted and decimated. The game imports the meshes at build time (docs/VGEO.md "Tree
  models"): rescaled, re-centred, card triangles re-wound, wind weights added, clustered and
  simplified into the vgeo LOD DAG, impostor atlases baked. The textures are box-filtered down to
  512 px at load. The files' own LOD meshes were not used and are not in the repository.
  `birch leaf_Transmission.png` is derived from `birch leaf.png` (a desaturated, brightened copy); the
  export did not include one.

## Globe data (`assets/globe`)

- **What:** the Maps tab globe's tile pyramid, vectors and height maps (docs/MENU_GLOBE.md "Data").
- **Sources and licences:**
  - Contains modified Copernicus Sentinel-2 data (2023 / 2024), from the Sentinel-2 L2A COGs on the
    AWS Open Data registry. Copernicus data licence: free use with attribution.
  - NASA Blue Marble (public domain).
  - Natural Earth vectors and the NE1_HR_LC raster (public domain), <https://www.naturalearthdata.com>.
  - AWS Terrain Tiles (Mapzen): SRTM, GMTED2010, ETOPO1, USGS NED and others, attributed as the AWS
    Open Data registry asks.
  - geoBoundaries (William & Mary geoLab; the US layers from US Census TIGER), CC BY 4.0,
    <https://www.geoboundaries.org>.
- **Modified:** reprojected to equirectangular and turned grey. The Sentinel-2 scenes are
  mosaicked, cloud-masked and tone-matched. Everything is relief-shaded from the height map,
  simplified and cut into tiles and levels of detail. `tools/globe/build_globe.py` rebuilds it all
  from the sources.
