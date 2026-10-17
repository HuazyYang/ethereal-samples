# Beach Cove Media

The beach renderer uses checked-in media for the original surveyed terrain,
surface detail, sky, and foam breakup. The optional `--beach-terrain=cove`
terrain keeps its authored reference-shaped coast in `examples/beach/beach_cove.cpp`.
Its fine ground relief, surface color, normals, roughness and plant coverage now
come from the scans listed below, rather than the previous procedural ground noise.
`docs/WaveWorks_ocean.png` is only a visual composition reference and is not sampled
by the renderer.

The survey retains the PBR milestone's generated grass-blade geometry. The cove
instead instances the original rock and plant meshes listed below. Texture mips
are filtered copies of source maps. Stochastic rotated/translated sampling
reduces cove ground tiling. Each material's albedo, normals and roughness share
patch transforms; the coastal displacement and planting mask use that coastal
material's 20 m transforms. The vegetation surface detail has its own 2 m
footprint. The survey's elevations are not replaced.

## Terrain geometry

`hanalei_cove.bin` is a static indexed mesh baked from a Hanalei Bay subset of
NOAA NCEI's **Kauai, Hawaii 1/3 arc-second MHW Coastal Digital Elevation
Model**. The source integrates measured bathymetry and topography. The subset
is west -159.62, east -159.34, south 22.13, north 22.29 with a horizontal
stride of four source samples (approximately 40 metres). Horizontal coordinates
were scaled by 0.012 into the compact example world; topography uses a 0.018
vertical scale and bathymetry a 0.15 vertical scale to preserve useful optical
water thickness. Normals and material UVs were baked, and the final world-space
vertices and indices are stored directly in the media file. No procedural
displacement or noise was added to these surveyed source elevations. The
separate cove mesh does not modify this asset.

- Dataset: https://www.ncei.noaa.gov/access/metadata/landing-page/bin/iso?id=gov.noaa.ngdc.mgg.dem%3A3550
- NOAA source file: https://www.ngdc.noaa.gov/thredds/fileServer/regional/kauai_13_mhw_2012.nc
- Source status: produced by NOAA NCEI and not subject to copyright protection
  within the United States; not for navigational use.

## Surface textures

The following Poly Haven assets are licensed under CC0 1.0:

- **Coastal Cliff 01**, cleanup by Rico Cilliers and photogrammetry/processing
  by Rob Tuytel: https://polyhaven.com/a/coastal_cliff_01
  - `coastal_cliff_01_diff_2k.dds`, converted from the 2K JPG diffuse map

- **Aerial Beach 02**, by Rob Tuytel: https://polyhaven.com/a/aerial_beach_02
  - `aerial_beach_02_diff_1k.dds`, from the 1K JPG diffuse map
  - `aerial_beach_02_nor_dx_1k.dds`, from the 1K JPG DirectX normal map
- **Sparse Grass**, by Amal Kumar: https://polyhaven.com/a/sparse_grass
  - `sparse_grass_diff_1k.dds`, from the 1K JPG diffuse map
  - `sparse_grass_nor_dx_1k.dds`, from the 1K JPG DirectX normal map

Repository modification: each source JPG was decoded to RGBA8 and stored as an
uncompressed DDS for the example's existing cross-backend texture loader. No visual
content was otherwise modified.

## Imported cove ground and individual props

All assets in `scans/` are derived from these CC0 1.0 Poly Haven sources:

- [Coast Land Rocks 01](https://polyhaven.com/a/coast_land_rocks_01), photography
  and processing by Rob Tuytel, tiling and adjustments by Rico Cilliers:
  `coastal_ground_*`, 2K diffuse / DirectX normal / AO-roughness-metallic maps,
  filtered displacement and a planting mask derived from diffuse-map chroma.
  The source covers 20 by 20 metres.
  Its albedo RGB payload remains unchanged. The otherwise unused terrain alpha
  now encodes Gaussianized source luminance; `coastal_ground_luminance.bin`
  stores source-derived inverse histograms for all 12 mip levels. This scalar
  luminance variant of Heitz/Neyret histogram-preserving sampling reduces the
  contrast loss from blended scan patches; it is not a painted color grade.
- [Aerial Beach 02](https://polyhaven.com/a/aerial_beach_02), Rob Tuytel:
  `sand_*`, 1K diffuse / DirectX normal / AO-roughness-metallic maps and filtered
  displacement. Diffuse and normal payloads match the original beach DDS assets.
- [Leafy Grass](https://polyhaven.com/a/leafy_grass), Charlotte Baglioni:
  `vegetation_ground_*`, 2K diffuse / DirectX normal / AO-roughness-metallic maps.
  Its 2 by 2 metre footprint retains photographed blades, litter and soil under
  the individual plant meshes, including when those meshes become subpixel.
- [Rock Moss Set 01](https://polyhaven.com/a/rock_moss_set_01), Kless Gyzen:
  six original rock meshes with source UVs and 1K maps.
- [Grass Bermuda 01](https://polyhaven.com/a/grass_bermuda_01), Rico Cilliers:
  21 original grass mesh variants, 1K maps and source alpha cutouts.
- [Shrub Sorrel 01](https://polyhaven.com/a/shrub_sorrel_01), Rico Cilliers:
  11 original leaf/stem mesh variants, 1K maps and source alpha cutouts.

The rock/plant glTFs retain their vertices, normals, UVs and triangle topology.
The importer removes contact-sheet translations, centres each mesh in XZ and
places its lowest point at Y=0. The renderer then scales, rotates, roots and
instances those meshes. The original Blender scatter systems are not imported.
Model normal maps retain their OpenGL convention; terrain maps use DirectX.
Plant alpha is packed into albedo DDS; its mip chain uses alpha-weighted
linear-light filtering and preserves coverage at the material's 0.45 cutoff.
The rock source roughness map is packed with neutral AO and metallic channels
when the glTF package does not supply packed ARM.

`scans/sources.json` records download URLs and source MD5 checksums. The per-model
JSON files record original variant names, dimensions and mesh counts.
`examples/beach/tools/import_scans.py --download` reproduces the conversion;
subsequent runs work offline from `build/beach-scan-sources/`. Python, NumPy and
Pillow are required only for import, not for the executable. Powered by Poly Haven.

These are visual stand-ins, not scans recovered from `WaveWorks_ocean.png`.
That image does not establish exact plant taxonomy, hidden rock geometry or
individual world-space positions. No generated bitmap assets or reference-image
projection are used. Rock placements and plant group transforms are scene authored.

License: https://creativecommons.org/publicdomain/zero/1.0/

The panorama `media/sky_dome_1.dds` and the WaveWorks foam/bubble media are inherited
from the original NVIDIA WaveWorks sample media distributed with this repository.
