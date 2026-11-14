# Natural environment assets

These source models are from [Poly Haven](https://polyhaven.com), released under **CC0 1.0 Universal**. The original artist/source metadata is retained in each asset folder's `source.json`.

| Asset | Processed triangles | Runtime use |
|---|---:|---|
| Jacaranda Tree | 458,211 | Main broadleaf canopy |
| Island Tree 02 | 103,606 | Smaller coastal tree |
| Fir Sapling | 108,229 | Understory conifer cluster |
| Shrub 04 | 27,327 | Low shrubs |
| Fern 02 | 6,232 | Forest-floor fern cluster |
| Rock Moss Set 01 | 63,127 | Mossy rock cluster |

The original GLB files contain embedded 1K PBR textures. `runtime/` contains derived GLBs used by the open-world demo; their embedded images are downsampled to 256px while geometry, materials, UVs and alpha coverage are preserved. The original models and per-asset `source.json` metadata remain unchanged. All files remain ordinary glTF 2.0 / GLB 2.0 without meshopt-compression requirements. The imported leaf/shrub/fern primitives receive a normalized wind-bend weight at load time.

CC0: https://creativecommons.org/publicdomain/zero/1.0/
